/*
 * insserv(.c)
 *
 * Copyright 2000 Werner Fink, 2000 SuSE GmbH Nuernberg, Germany.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <regex.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include "listing.h"

#ifndef  INITDIR
# define INITDIR	"/etc/init.d"
#endif
#ifndef  INSCONF
# define INSCONF	"/etc/insserv.conf"
#endif

/*
 * For a description of regular expressions see regex(7).
 */
#define COMM		"^#[[:blank:]]*"
#define VALUE		":[[:blank:]]*([[:print:][:blank:]]*)"
/* The second substring contains our value (the first is all) */
#define SUBNUM		2
#define SUBNUM_SHD	3
#define START		"[-_]+start"
#define STOP		"[-_]+stop"

/* The main regular search expressions */
#define PROVIDES	COMM "provides" VALUE
#define REQUIRED	COMM "required"
#define SHOULD		COMM "(x[-_]+[a-z0-9_-]+)?should"
#define DEFAULT		COMM "default"
#define REQUIRED_START  REQUIRED START VALUE
#define REQUIRED_STOP	REQUIRED STOP  VALUE
#define SHOULD_START	SHOULD   START VALUE
#define SHOULD_STOP	SHOULD   STOP  VALUE
#define DEFAULT_START	DEFAULT  START VALUE
#define DEFAULT_STOP	DEFAULT  STOP  VALUE
#define DESCRIPTION	COMM "description" VALUE

/* System facility search within /etc/insserv.conf */
#define EQSIGN		"([[:blank:]]?[=:]+[[:blank:]]?|[[:blank:]]+)"
#define CONFLINE	"^(\\$[a-z0-9_-]+)" EQSIGN "([[:print:][:blank:]]*)"
#define SUBCONF		2
#define SUBCONFNUM	4

/* The main line buffer if unique */
static char buf[LINE_MAX];

/* Search results points here */
typedef struct lsb_struct {
    char *provides;
    char *required_start;
    char *required_stop;
    char *should_start;
    char *should_stop;
    char *default_start;
    char *default_stop;
    char *description;
} lsb_t;

static lsb_t script_inf = {NULL, NULL, NULL, NULL, NULL, NULL };
static char empty[1] = "";

/* Delimeters used for spliting results with strsep(3) */
const char *delimeter = " ,;\t";

/*
 * push and pop directory changes: pushd() and popd()
 */
typedef struct pwd_struct {
    list_t	deep;
    char	*pwd;
} pwd_t;
#define getpwd(list)	list_entry((list), struct pwd_struct, deep)

static list_t pwd = { &(pwd), &(pwd) }, * topd = &(pwd);

static void pushd(const char * path)
{
    pwd_t *  dir;

    dir = (pwd_t *)malloc(sizeof(pwd_t));
    if (dir) {
	if (!(dir->pwd = getcwd(NULL, 0)))
	    goto err;
	insert(&(dir->deep), topd->prev);
	goto out;
    }
err:
    error("%s", strerror(errno));
out:
    if (chdir(path) < 0)
	    error ("pushd() can not change to directory %s: %s\n", path, strerror(errno));
}

static void popd(void)
{
    list_t * tail = topd->prev;
    pwd_t *  dir;

    if (tail == topd)
	goto out;
    dir = getpwd(tail);
    if (chdir(dir->pwd) < 0)
	error ("popd() can not change directory %s: %s\n", dir->pwd, strerror(errno));
    free(dir->pwd);
    delete(tail);
    free(dir);
out:
    return;
}

/*
 * Linked list of system facilities services and their replacment
 */
typedef struct faci {
    list_t	 list;
    char	*name;
    char	*repl;
} faci_t;
#define getfaci(arg)	list_entry((arg), struct faci, list)

static list_t sysfaci = { &(sysfaci), &(sysfaci) }, *sysfaci_start = &(sysfaci);

/*
 * Linked list of required services of a service
 */
typedef struct req_serv {
    list_t	 list;
    char       * serv;
} req_t;
#define getreq(arg)	list_entry((arg), struct req_serv, list)

/*
 * Linked list to hold hold information of services
 */
typedef struct serv_struct {
    list_t	   id;
    req_t         req;
    req_t         shd;
    char	order;
    char       * name;
    unsigned int lvls;
#ifndef SUSE
    unsigned int lvlk;
#endif
} serv_t;
#define getserv(arg)	list_entry((arg), struct serv_struct, id)

static list_t serv = { &(serv), &(serv) }, *serv_start = &(serv);

/*
 * Find or add and initialize a service
 */
static serv_t * addserv(const char * serv)
{
    serv_t * this;
    list_t * ptr;

    for (ptr = serv_start->next; ptr != serv_start;  ptr = ptr->next)
	if (!strcmp(getserv(ptr)->name, serv))
	    goto out;

    this = (serv_t *)malloc(sizeof(serv_t));
    if (this) {
	list_t * req_start = &(this->req.list);
	list_t * shd_start = &(this->shd.list);
	insert(&(this->id), serv_start->prev);
	req_start->next = req_start;
	req_start->prev = req_start;
	shd_start->next = shd_start;
	shd_start->prev = shd_start;
	this->req.serv = NULL;
	this->shd.serv = NULL;
	this->name  = xstrdup(serv);
	this->order = 0;
	this->lvls  = 0;
	ptr = serv_start->prev;
	goto out;
    }
    ptr = NULL;
    error("%s", strerror(errno));
out:
    return getserv(ptr);
}

static serv_t * findserv(const char * serv)
{
    list_t * ptr;
    serv_t * ret = NULL;

    if (!serv)
	goto out;

    for (ptr = serv_start->next; ptr != serv_start;  ptr = ptr->next)
	if (!strcmp(getserv(ptr)->name, serv)) {
	    ret = getserv(ptr);
	    break;
	}
out:
    return ret;
}

/*
 * Expand requested services for sorting
 */
static void expandreq(const char *name, req_t * cur)
{
    list_t * req_start = &(cur->list);
    list_t * ptr;

    if (cur->serv)
	requiresv(name, cur->serv);

    for (ptr = req_start->next; ptr != req_start; ptr = ptr->next) {
	char * needed = getreq(ptr)->serv;
	if (needed)
	    requiresv(name, needed);
    }
}

/*
 * Remember requests for required or should services and expand `$' token
 */
static void rememberreq(serv_t *serv, req_t * cur, char * required)
{
    char * token, * tmp = strdupa(required);
    req_t * old = cur;

    while ((token = strsep(&tmp, delimeter))) {
	list_t * ptr;

	if (!*token)
	    continue;
	cur = old;			/* The default list */

	if (*token != '$') {
	    req_t * this = NULL;

	    /*
	     * Optional real services are noted by a `+' sign
	     */
	    if (*token == '+') {
		token++;
		cur = &serv->shd;
		if (*token == '$')
		    goto dollar;	/* Someone uses `+$' */
	    }

	    if (!cur->serv) {
		this = cur;
		this->serv = xstrdup(token);
	    } else {
		list_t * req_start = &(cur->list);
		boolean found = false;

		/* Do not link in if already done */
		if (!strcmp(cur->serv, token)) {
		    found = true;
		} else {
		    for (ptr = req_start->next; ptr != req_start; ptr = ptr->next) {
			if (!strcmp(getreq(ptr)->serv, token)) {
			    found = true;
			    break;
			}
		    }
		}

		if (!found) {
		    this = (req_t *)malloc(sizeof(req_t)); 
		    if (!this) 
			error("%s", strerror(errno));
		    insert(&(this->list), req_start->prev);
		    this->serv = xstrdup(token);
		}
	    }

	    continue;			/* Below we handle `$' token */
	}
dollar:
	/* Expand the `$' token recursively down */
	for (ptr = sysfaci_start->next; ptr != sysfaci_start; ptr = ptr->next) {
	    if (!strcmp(token, getfaci(ptr)->name)) {
		rememberreq(serv, cur, getfaci(ptr)->repl);
		break;
	    }
	}
    }
    /* At last: expand requested services for sorting */
    expandreq(serv->name, old);
}

/*
 * Check required services for name
 */
static boolean chkrequired(const char * name)
{
    serv_t * serv = findserv(name);
    list_t * req_start, * ptr;
    serv_t * required;
    boolean ret = true;

    if (serv && serv->req.serv)
	req_start = &(serv->req.list);
    else
	goto out;

    /*
     * If the first required service is misssed we run on an error.
     */
    if (!(required = findserv(serv->req.serv)) || !(required->lvls)) {
	warn("Service %s has to be enabled for service %s\n", serv->req.serv, name);
	ret = false;
    }

    for (ptr = req_start->next; ptr != req_start; ptr = ptr->next)
	if (!(required = findserv(getreq(ptr)->serv)) || !(required->lvls)) {
	    warn("Service %s has to be enabled for service %s\n", getreq(ptr)->serv, name);
	    ret = false;
	}
out:
    return ret;
}

/*
 * Check dependencies for name as a service
 */
static boolean chkdependencies(const char * name)
{
    list_t * srv;
    boolean ret = true;

    for (srv = serv_start->next; srv != serv_start; srv = srv->next) {
	serv_t * cur = getserv(srv);
	list_t * req_start, * req;

	if (!cur || !cur->req.serv)
	    continue;

	if (!cur->lvls)
	    continue;

	req_start = &(cur->req.list);

	if (!strcmp(cur->req.serv, name)) {
	    warn("Service %s has to be enabled for service %s\n", name, cur->name);
	    ret = false;
	}

	for (req = req_start->next; req != req_start; req = req->next) {
	    if (!strcmp(getreq(req)->serv, name)) {
		warn("Service %s has to be enabled for service %s\n", name, cur->name);
		ret = false;
	    }
	}
    }
    return ret;
}

/*
 * This helps us to work out the current symbolic link structure
 */
static serv_t * current_structure(const char * this, const char order, const int runlvl)
{
    serv_t * serv = addserv(this);

    if (serv->order < order)
	serv->order = order;

    switch (runlvl) {
	case 0: serv->lvls |= LVL_HALT;   break;
	case 1: serv->lvls |= LVL_ONE;    break;
	case 2: serv->lvls |= LVL_TWO;    break;
	case 3: serv->lvls |= LVL_THREE;  break;
	case 4: serv->lvls |= LVL_FOUR;   break;
	case 5: serv->lvls |= LVL_FIVE;   break;
	case 6: serv->lvls |= LVL_REBOOT; break;
	case 7: serv->lvls |= LVL_SINGLE; break;
	case 8: serv->lvls |= LVL_BOOT;   break;
	default: break;
    }

    return serv;
}


/*
 * Internal logger
 */
char *myname = NULL;
static void _logger (const char *fmt, va_list ap)
{
    char buf[strlen(myname)+2+strlen(fmt)+1];
    strcat(strcat(strcpy(buf, myname), ": "), fmt);
    vfprintf(stderr, buf, ap);
    return;
}

/*
 * Cry and exit.
 */
void error (const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _logger(fmt, ap);
    va_end(ap);
    popd();
    exit (1);
}

/*
 * Warn the user.
 */
void warn (const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _logger(fmt, ap);
    va_end(ap);
    return;
}

/*
 * Open a runlevel directory, if it not
 * exists than create one.
 */
static DIR * openrcdir(const char * rcpath)
{
   DIR * rcdir;
   struct stat st;

    if (stat(rcpath, &st) < 0) {
	if (errno == ENOENT)
	    mkdir(rcpath, (S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH));
	else
	    error("can not stat(%s): %s\n", rcpath, strerror(errno));
    }

    if ((rcdir = opendir(rcpath)) == NULL)
	error("can not opendir(%s): %s\n", rcpath, strerror(errno));

    return rcdir;
}

/*
 * Wrapper for regcomp(3)
 */
static void regcompiler (regex_t *preg, const char *regex, int cflags)
{
    register int ret = regcomp(preg, regex, cflags);
    if (ret) {
	regerror(ret, preg, buf, sizeof (buf));
	regfree (preg);
	error("%s\n", buf);
    }
    return;
}

/*
 * Wrapper for regexec(3)
 */
static boolean regexecutor (regex_t *preg, const char *string,
			size_t nmatch, regmatch_t pmatch[], int eflags)
{
    register int ret = regexec(preg, string, nmatch, pmatch, eflags);
    if (ret > REG_NOMATCH) {
	regerror(ret, preg, buf, sizeof (buf));
	regfree (preg);
	error("%s\n", buf);
    }
    return (ret ? false : true);
}

/*
 * The script scanning engine.
 */
static void scan_script_defaults(const char *path)
{
    regex_t reg_prov;
    regex_t reg_req_start;
    regex_t reg_req_stop;
    regex_t reg_shl_start;
    regex_t reg_shl_stop;
    regex_t reg_def_start;
    regex_t reg_def_stop;
    regex_t reg_desc;
    regmatch_t subloc[SUBNUM_SHD+1], *val = &subloc[SUBNUM-1], *shl = &subloc[SUBNUM_SHD-1];
    FILE *script;
    char *pbuf = buf;

#define provides	script_inf.provides
#define required_start	script_inf.required_start
#define required_stop	script_inf.required_stop
#define should_start	script_inf.should_start
#define should_stop	script_inf.should_stop
#define default_start	script_inf.default_start
#define default_stop	script_inf.default_stop
#define description	script_inf.description

    regcompiler(&reg_prov,	PROVIDES,	REG_EXTENDED|REG_ICASE);
    regcompiler(&reg_req_start, REQUIRED_START, REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg_req_stop,  REQUIRED_STOP,	REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg_shl_start, SHOULD_START,   REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg_shl_stop,  SHOULD_STOP,	REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg_def_start, DEFAULT_START,	REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg_def_stop,  DEFAULT_STOP,	REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg_desc,	DESCRIPTION,	REG_EXTENDED|REG_ICASE|REG_NEWLINE);

    script = fopen(path, "r");
    if (!script)
	error("fopen(%s): %s\n", path, strerror(errno));

    /* Reset old results */
    xreset(provides);
    xreset(required_start);
    xreset(required_stop);
    xreset(should_start);
    xreset(should_stop);
    xreset(default_start);
    xreset(default_stop);
    xreset(description);

#define COMMON_ARGS	buf, SUBNUM, subloc, 0
#define COMMON_SHD_ARGS	buf, SUBNUM_SHD, subloc, 0
    while (fgets(buf, sizeof(buf), script)) {
	if (!provides       && regexecutor(&reg_prov,      COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
	        provides = xstrdup(pbuf+val->rm_so);
	    } else
		provides = empty;
	}
	if (!required_start && regexecutor(&reg_req_start, COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		required_start = xstrdup(pbuf+val->rm_so);
	    } else
		required_start = empty;
	}
	if (!required_stop  && regexecutor(&reg_req_stop,  COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		required_stop = xstrdup(pbuf+val->rm_so);
	    } else
		required_stop = empty;
	}
	if (!should_start && regexecutor(&reg_shl_start,   COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		should_start = xstrdup(pbuf+shl->rm_so);
	    } else
		should_start = empty;
	}
	if (!should_stop  && regexecutor(&reg_shl_stop,    COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		should_stop = xstrdup(pbuf+shl->rm_so);
	    } else
		should_stop = empty;
	}
	if (!default_start  && regexecutor(&reg_def_start, COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		default_start = xstrdup(pbuf+val->rm_so);
	    } else
		default_start = empty;
	}
#ifndef SUSE
	if (!default_stop   && regexecutor(&reg_def_stop,  COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		default_stop = xstrdup(pbuf+val->rm_so);
	    } else
		default_stop = empty;
	}
#endif
	if (!description    && regexecutor(&reg_desc,      COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		description = xstrdup(pbuf+val->rm_so);
	    } else
		description = empty;
	}
    }
#undef COMMON_ARGS
#undef COMMON_SHD_ARGS
    regfree(&reg_prov);
    regfree(&reg_req_start);
    regfree(&reg_req_stop);
    regfree(&reg_shl_start);
    regfree(&reg_shl_stop);
    regfree(&reg_def_start);
    regfree(&reg_def_stop);
    regfree(&reg_desc);
    fclose(script);

#undef provides
#undef required_start
#undef required_stop
#undef should_start
#undef should_stop
#undef default_start
#undef default_stop
#undef description

    return;
}

/*
 * Scan current service structure
 */
static void scan_script_locations(const char * path)
{
    int runlevel;

    pushd(path);
    for (runlevel = 0; runlevel < 9; runlevel++) {
	char * rcd = NULL;
	DIR  * rcdir;
	struct dirent *d;
	char * token;
	struct stat st_script;

	switch (runlevel) {
	    case 0: rcd = "rc0.d/";  break;
	    case 1: rcd = "rc1.d/";  break;
	    case 2: rcd = "rc2.d/";  break;
	    case 3: rcd = "rc3.d/";  break;
	    case 4: rcd = "rc4.d/";  break;
	    case 5: rcd = "rc5.d/";  break;
	    case 6: rcd = "rc6.d/";  break;
	    case 7: rcd = "rcS.d/";  break;  /* runlevel S */
	    case 8: rcd = "boot.d/"; break;  /* runlevel B */
	    default:
		error("Wrong runlevel %d\n", runlevel);
	}

	rcdir = openrcdir(rcd); /* Creates runlevel directory if necessary */
	pushd(rcd);
	while ((d = readdir(rcdir)) != NULL) {
	    char * ptr = d->d_name;
	    char order = 0;
	    char* begin = (char*)NULL; /* Remember address of ptr handled by strsep() */

	    if (*ptr != 'S')
		continue;
	    ptr++;

	    if (strspn(ptr, "0123456789") != 2)
		continue;
	    order = atoi(ptr);
	    ptr += 2;

	    if (stat(d->d_name, &st_script) < 0) {
		xremove(d->d_name);	/* dangling sym link */
		continue;
	    }

	    scan_script_defaults(d->d_name);
	    if (!script_inf.provides || script_inf.provides == empty)
		script_inf.provides = xstrdup(ptr);

	    begin = script_inf.provides;
	    while ((token = strsep(&script_inf.provides, delimeter)) && *token) {
		serv_t * service = NULL;
		if (*token == '$') {
		    warn("script %s provides system facility %s, skiped!\n", d->d_name, token);
		    continue;
		}
		service = current_structure(token, order, runlevel);
		if (!service->req.serv && script_inf.required_start && script_inf.required_start != empty) {
		    rememberreq(service, &service->req, script_inf.required_start);
		    requiresv(token, script_inf.required_start);
		}
		if (!service->shd.serv && script_inf.should_start && script_inf.should_start != empty) {
		    rememberreq(service, &service->shd, script_inf.should_start);
		    requiresv(token, script_inf.should_start);
		}
	    }
	    script_inf.provides = begin;

	    xreset(script_inf.provides);
	    xreset(script_inf.required_start);
	    xreset(script_inf.required_stop);
	    xreset(script_inf.should_start);
	    xreset(script_inf.should_stop);
	    xreset(script_inf.default_start);
	    xreset(script_inf.default_stop);
	    xreset(script_inf.description);
	}
	popd();
	closedir(rcdir);
    }
    popd();
    return;
}

/*
 * The /etc/insserv.conf scanning engine.
 */
static void scan_conf(void)
{
    regex_t reg_conf;
    regmatch_t subloc[SUBCONFNUM], *val = NULL;
    FILE *conf;
    char *pbuf = buf;

    regcompiler(&reg_conf, CONFLINE, REG_EXTENDED|REG_ICASE);

    do {
	char * fptr = INSCONF;
	if (*fptr == '/')
	    fptr++;
	/* Try relativ location first */
	if ((conf = fopen(fptr, "r")))
	    break;
	/* Try absolute location */
	if ((conf = fopen(INSCONF, "r")))
	    break;
	goto err;
    } while (1);

    while (fgets(buf, sizeof(buf), conf)) {
	if (*pbuf == '#')
	    continue;
	if (regexecutor(&reg_conf, buf, SUBCONFNUM, subloc, 0) == true) {
	    char * virt = NULL, * real = NULL;
	    val = &subloc[SUBCONF - 1];
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		virt = pbuf+val->rm_so;
	    }
	    val = &subloc[SUBCONFNUM - 1];
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		real = pbuf+val->rm_so;
	    }
	    if (virt) {
		list_t * ptr;
		boolean found = false;
		for (ptr = sysfaci_start->next; ptr != sysfaci_start; ptr = ptr->next) {
		    if (!strcmp(getfaci(ptr)->name, virt)) {
			found = true;
			break;
		    }
		}
		if (!found) {
		    faci_t * this = (faci_t *)malloc(sizeof(faci_t));
		    if (!this)
			error("%s", strerror(errno));
		    insert(&(this->list), sysfaci_start->prev);
		    this->name = xstrdup(virt);
		    this->repl = xstrdup(real);
		}
	    }
	}
    }
    regfree(&reg_conf);
    fclose(conf);
    return;
err:
    warn("fopen(%s): %s\n", INSCONF, strerror(errno));
}

/*
 * Expand the system facilitis after all scripts have been scanned.
 */
static void expand_conf()
{
    list_t * ptr;
    for (ptr = sysfaci_start->next; ptr != sysfaci_start; ptr = ptr->next)
	virtprov(getfaci(ptr)->name, getfaci(ptr)->repl);
}

/*
 * Scan for a Start or Kill script within a runlevel directory.
 * We start were we leave the directory, the upper level
 * has to call rewinddir(3) if necessary.
 */
static char * scan_for(DIR * rcdir, const char * script, char type)
{
    struct dirent *d;
    char * ret = NULL;

    while ((d = readdir(rcdir)) != NULL) {
	char * ptr = d->d_name;

	if (*ptr != type)
	    continue;
	ptr++;

	if (strspn(ptr, "0123456789") != 2)
	    continue;
	ptr += 2;

	if (!strcmp(ptr, script)) {
	    ret = d->d_name;
	    break;
	}
    }
    return ret;
}

/*
 *  Check for script in list.
 */
static int curr_argc = -1;
static boolean chkfor(const char * script, char **list, const int cnt)
{
    boolean isinc = false;
    register int c = cnt;

    curr_argc = -1;
    while (c--) {
	if (!strcmp(script, list[c])) {
	    isinc = true;
	    curr_argc = c;
	    break;
	}
    }
    return isinc;
}

static struct option long_options[] =
{
    {"default",	0, NULL, 'd'},
    {"remove",	0, NULL, 'r'},
    {"force",	0, NULL, 'f'},
    {"help",	0, NULL, 'h'},
    { 0,	0, NULL,  0 },
};

static void help(const char * name)
{
    printf("Usage: %s [<options>] [init_script|init_directory]\n", name);
    printf("Available options:\n");
    printf("  -h, --help       This help.\n");
    printf("  -r, --remove     Remove the listed scripts from all runlevels.\n");
    printf("  -f, --force      Ignore if a required service is missed.\n");
    printf("  -d, --default    Use default runlevels a defined in the scripts\n");
}


/*
 * Do the job.
 */
int main (int argc, char *argv[])
{
    DIR * initdir;
    struct dirent *d;
    struct stat st_script;
    char * argr[argc];
    char * path = INITDIR;
    int runlevel, order, c;
    boolean del = false;
    boolean defaults = false;
    boolean ignore = false;

    myname = basename(*argv);

    for (c = 0; c < argc; c++)
	argr[c] = NULL;

    while ((c = getopt_long(argc, argv, "dfrh", long_options, NULL)) != -1) {
	switch (c) {
	    case 'd':
		defaults = true;
		break;
	    case 'r':
		del = true;
		break;
	    case 'f':
		ignore = true;
		break;
	    case '?':
		error("For help use: %s -h\n", myname);
	    case 'h':
		help(myname);
		exit(0);
	    default:
		break;
	}
    }
    argv += optind;
    argc -= optind;

    if (!argc && del)
	error("usage: %s [[-r] init_script|init_directory]\n", myname);

    if (*argv) {
	char * token = strpbrk(*argv, delimeter);

	/*
	 * Let us separate the script/service name from the additional arguments.
	 */
	if (token && *token) {
	    *token = '\0';
	    *argr = ++token;
	}

	/* Catch `/path/script', `./script', and `path/script' */
	if (strchr(*argv, '/')) {
	    if ( stat(*argv, &st_script) < 0)
		error("%s: %s\n", *argv, strerror(errno));
	} else {
	    pushd(path);
	    if (stat(*argv, &st_script) < 0)
		error("%s: %s\n", *argv, strerror(errno));
	    popd();
	}

	if (S_ISDIR(st_script.st_mode)) {
	    path = *argv;
	    if (del)
		error("usage: %s [[-r] init_script|init_directory]\n", myname);
	    argv++;
	    argc--;
	    if (argc)
		error("usage: %s [[-r] init_script|init_directory]\n", myname);
	} else {
	    char * base, * ptr = xstrdup(*argv);

	    if ((base = strrchr(ptr, '/'))) {
		*(++base) = '\0';
		path = ptr;
	    } else
		free(ptr);
	}
    }

    c = argc;
    while (c--) {
	char * base;
	char * token = strpbrk(argv[c], delimeter);

	/*
	 * Let us separate the script/service name from the additional arguments.
	 */
	if (token && *token) {
	    *token = '\0';
	    argr[c] = ++token;
	}

	if (stat(argv[c], &st_script) < 0) {
	    if (errno != ENOENT)
		error("%s: %s\n", argv[c], strerror(errno));
	    pushd(path);
	    if (stat(argv[c], &st_script) < 0)
		error("%s: %s\n", argv[c], strerror(errno));
	    popd();
	}
	if ((base = strrchr(argv[c], '/'))) {
	    base++;
	    argv[c] = base;
	}
    }

#if defined(DEBUG) && (DEBUG > 0)
    for (c = 0; c < argc; c++)
	if (argr[c])
	    printf("Overwrite argument for %s is %s\n", argv[c], argr[c]);
#endif

    /*
     * Scan and set our configuration for virtual services.
     */
    scan_conf();

    /*
     * Scan always for the runlevel links to see the current
     * link scheme of the services.
     */
    scan_script_locations(path);

    if ((initdir = opendir(path)) == NULL)
	error("can not opendir(%s): %s\n", path, strerror(errno));
    /*
     * Now scan for the service scripts and their LSB comments.
     */
    pushd(path);
    while ((d = readdir(initdir)) != NULL) {
	serv_t * service = NULL;
	char * end;
	char * token;
	char* begin = (char*)NULL;  /* Remember address of ptr handled by strsep() */
	errno = 0;

	/* d_type seems not to work, therefore use stat(2) */
	if (stat(d->d_name, &st_script) < 0) {
	    warn("can not stat(%s)\n", d->d_name);
	    continue;
	}
	if (!S_ISREG(st_script.st_mode) || !(S_IXUSR & st_script.st_mode))
	    continue;

	if (!strncmp(d->d_name, "README", strlen("README")))
	    continue;

	if (!strncmp(d->d_name, "core", strlen("core")))
	    continue;

	/* Common scripts not used within runlevels */
	if (!strcmp(d->d_name, "rc")	   ||
	    !strcmp(d->d_name, "rx")	   ||
	    !strcmp(d->d_name, "skeleton") ||
	    !strcmp(d->d_name, "powerfail"))
	    continue;

	if (!strcmp(d->d_name, "boot"))
	    continue;

	if ((end = strrchr(d->d_name, '.'))) {
	    end++;
	    if (!strcmp(end,  "local"))
		continue;
	    /* .rmporig, .rpmnew, .rmpsave, ... */
	    if (!strncmp(end, "rpm", 3))
		continue;
	    /* .bak, .backup, ... */
	    if (!strncmp(end, "ba", 2))
		continue;
	    if (!strcmp(end,  "old"))
		continue;
	    if (!strcmp(end,  "new"))
		continue;
	    if (!strcmp(end,  "save"))
		continue;
	    /* Used by vi like editors */
	    if (!strcmp(end,  "swp"))
		continue;
	    /* modern core dump */
	    if (!strcmp(end,  "core"))
		continue;
	}

	/* Leaved by emacs like editors */
	if (d->d_name[strlen(d->d_name)-1] == '~')
	    continue;

	if (strspn(d->d_name, "0123456789$.#_-\\*"))
	    continue;

	/* main scanner for LSB comment in current script */
	scan_script_defaults(d->d_name);

	/* Common script ... */
	if (!strcmp(d->d_name, "halt")) {
	    makeprov("halt",   d->d_name);
	    runlevels("halt",   "0");
	    continue;
	}

	/* ... and its link */
	if (!strcmp(d->d_name, "reboot")) {
	    makeprov("reboot", d->d_name);
	    runlevels("reboot", "6");
	    continue;
	}

	/* Common script for single mode */
	if (!strcmp(d->d_name, "single")) {
	    makeprov("single", d->d_name);
	    runlevels("single", "1 S");
	    requiresv("single", "kbd");
	    continue;
	}

	/*
	 * Oops, no comment found, guess one
	 */
	if (!script_inf.provides || script_inf.provides == empty) {
	    serv_t *guess;
	    script_inf.provides = xstrdup(d->d_name);

	    /*
	     * Use guessed service to find it within the the runlevels
	     * (by using the list from the first scan for script locations).
	     */
	    if ((guess = findserv(script_inf.provides))) {
		/*
		 * Try to guess required services out from current scheme.
		 * Note, this means that all services are required.
		 */
		if (!script_inf.required_start || script_inf.required_start == empty) {
		    list_t * ptr = NULL;
		    for (ptr = (&(guess->id))->prev; ptr != serv_start; ptr = ptr->prev) {
			if (getserv(ptr)->order >= guess->order)
			    continue;
			if (getserv(ptr)->lvls & guess->lvls) {
			    script_inf.required_start = xstrdup(getserv(ptr)->name);
			    break;
			}
		    }
		}
		if (!script_inf.default_start || script_inf.default_start == empty) {
		    if (guess->lvls)
			script_inf.default_start = xstrdup(lvl2str(guess->lvls));
		}
	    } else {
		/*
		 * Find out which levels this service may have out from current scheme.
		 * Note, this means that the first requiring service wins.
		 */
		if (!script_inf.default_start || script_inf.default_start == empty) {
		    list_t * ptr = NULL;
		    for (ptr = serv_start->next; ptr != serv_start; ptr = ptr->next) {
			serv_t * cur = getserv(ptr);
			list_t * req_start, * req;

			if (script_inf.default_start && script_inf.default_start != empty)
			   break;

			if (!cur || !cur->req.serv || !cur->lvls)
			    continue;

			if (!strcmp(cur->req.serv, script_inf.provides)) {
			    script_inf.default_start = xstrdup(lvl2str(getserv(ptr)->lvls));
			    break;
			}

			req_start = &(cur->req.list);
			for (req = req_start->next; req != req_start; req = req->next) {
			    if (!strcmp(getreq(req)->serv, script_inf.provides)) {
				script_inf.default_start = xstrdup(lvl2str(getserv(ptr)->lvls));
				break;
			    }
			}
		    }
		}
	    } /* !(guess = findserv(script_inf.provides)) */
	}

	/*
	 * Use guessed service to find it within the the runlevels
	 * (by using the list from the first scan for script locations).
	 */
	if (!service) {
	    char * provides = xstrdup(script_inf.provides);

	    begin = provides;
	    while ((token = strsep(&provides, delimeter)) && *token) {
		if (*token == '$') {
		    warn("script %s provides system facility %s, skiped!\n", d->d_name, token);
		    continue;
		}

		if (makeprov(token, d->d_name) < 0) {
		    warn("script %s: service %s already provided!\n", d->d_name, token);
		    continue;
	    	}

		if (!(service = findserv(token)))
			addserv(token);

		if ((service = findserv(token))) {

		    if (!service->req.serv && script_inf.required_start && script_inf.required_start != empty) {
			rememberreq(service, &service->req, script_inf.required_start);
			requiresv(token, script_inf.required_start);
		    }

		    if (!service->shd.serv && script_inf.should_start && script_inf.should_start != empty) {
			rememberreq(service, &service->shd, script_inf.should_start);
			requiresv(token, script_inf.should_start);
		    }

		    /*
		     * Use information from symbolic link structure to
		     * check if all services are around for this script.
		     */
		    if (chkfor(d->d_name, argv, argc) && !ignore) {
			boolean ok = true;
			if (del)
			    ok = chkdependencies(token);
			else
			    ok = chkrequired(token);
			if (!ok)
			    error("exiting now!\n");
		    }

		    if (script_inf.default_start && script_inf.default_start != empty) {
		 	unsigned int deflvls = str2lvl(script_inf.default_start);

			/*
			 * Compare all bits, which means `==' and not `&' and overwrite
			 * the defaults of the current script.
			 */
			if ((deflvls != service->lvls) && service->lvls && !defaults) {
			    if (!del && chkfor(d->d_name, argv, argc) && !(argr[curr_argc]))
				warn("Warning, current runlevel(s) of script `%s' overwrites defaults.\n",
				     d->d_name);
			}
		    } else
			script_inf.default_start = lvl2str(service->lvls);

		    /*
		     * required_stop, should_stop, and default_stop arn't used in SuSE Linux.
		     */
		}
	    }
	    free(begin);
	}

	/* Ahh ... set default multiuser with network */
	if (!script_inf.default_start)
	    script_inf.default_start = xstrdup("3 5");

	if (chkfor(d->d_name, argv, argc) && !defaults) {
	    if (argr[curr_argc]) {
		char * ptr = argr[curr_argc];
		struct _mark {
		    const char * wrd;
		    char * order;
		    char ** str;
		} mark[] = {
		    {"start=", NULL, &script_inf.default_start},
		    {"stop=",  NULL, &script_inf.default_stop },
		    {NULL, NULL, NULL}
		};

		for (c = 0; mark[c].wrd; c++) {
		    char * order = strstr(ptr, mark[c].wrd);
		    if (order)
			mark[c].order = order;
		}

		for (c = 0; mark[c].wrd; c++)
		    if (mark[c].order) {
			*(mark[c].order) = '\0';
			mark[c].order += strlen(mark[c].wrd);
		    }

		for (c = 0; mark[c].wrd; c++)
		    if (mark[c].order) {
			xreset(*(mark[c].str));
			*(mark[c].str) = xstrdup(mark[c].order);
		    }
	    }
	}

	begin = script_inf.provides;
	while ((token = strsep(&script_inf.provides, delimeter)) && *token) {
	    if (*token == '$')
		continue;

	    if (service && del)
		runlevels(token, lvl2str(service->lvls));
	    else
		runlevels(token, script_inf.default_start);

	    /*
	     * required_stop, should_stop, and default_stop arn't used in SuSE Linux.
	     */
	}
	script_inf.provides = begin;

    }
    /* Reset remaining pointers */
    xreset(script_inf.provides);
    xreset(script_inf.required_start);
    xreset(script_inf.required_stop);
    xreset(script_inf.should_start);
    xreset(script_inf.should_stop);
    xreset(script_inf.default_start);
    xreset(script_inf.default_stop);
    xreset(script_inf.description);

    /* back */
    popd();
    closedir(initdir);

    expand_conf();

    /*
     * Now generate for all scripts the dependencies
     */
    follow_all();

    /*
     * Re-order some well known scripts to get
     * a more stable order collection.
     * Stable means that new scripts should not
     * force a full re-order of all starting numbers.
     */

    if ((order = getorder("network")) > 0) {
	if (order < 5) setorder("network", 5);
	if (getorder("route") > 0)
	    setorder("route", (getorder("network") + 2));
    }
    if ((order = getorder("inetd"))  > 0 && order < 20) setorder("inetd",  20);

    /*
     * Set order of some singluar scripts
     * (no dependencies or single link).
     */
    if ((order = getorder("halt"))   > 0 && order < 20) setorder("halt",   20);
    if ((order = getorder("reboot")) > 0 && order < 20) setorder("reboot", 20);
    if ((order = getorder("single")) > 0) {
	if (order < 20) setorder("single", 20);
	if (getorder("kbd") > 0)
	    setorder("single", (getorder("kbd") + 2));
    }

    /*
     * Do not overwrite good old links.
     */
    if ((order = getorder("serial"))     > 0 && order < 10) setorder("serial",     10);
    if ((order = getorder("boot.setup")) > 0 && order < 20) setorder("boot.setup", 20);
    if ((order = getorder("gpm"))        > 0 && order < 20) setorder("gpm",        20);

    /*
     * Sorry but we support only [KS][0-9][0-9]<name>
     */
    if (maxorder > 99)
	error("Maximum of 99 in ordering reached\n");

#if defined(DEBUG) && (DEBUG > 0)
    printf("Maxorder %d\n", maxorder);
    show_all();
#else
# ifdef SUSE
    pushd(path);
    for (runlevel = 0; runlevel < 9; runlevel++) {
	char * script;
	char nlink[PATH_MAX+1], olink[PATH_MAX+1];
	char * rcd = NULL;
	DIR  * rcdir;

	switch (runlevel) {
	    case 0: rcd = "rc0.d/";  break;
	    case 1: rcd = "rc1.d/";  break;
	    case 2: rcd = "rc2.d/";  break;
	    case 3: rcd = "rc3.d/";  break;
	    case 4: rcd = "rc4.d/";  break;
	    case 5: rcd = "rc5.d/";  break;
	    case 6: rcd = "rc6.d/";  break;
	    case 7: rcd = "rcS.d/";  break;  /* runlevel S */
	    case 8: rcd = "boot.d/"; break;  /* runlevel B */
	    default:
		error("Wrong runlevel %d\n", runlevel);
	}

	script = NULL;
	rcdir = openrcdir(rcd); /* Creates runlevel directory if necessary */
	pushd(rcd);

	/*
	 * See if we found scripts which should not be
	 * included within this runlevel directory.
	 */
	while ((d = readdir(rcdir)) != NULL) {
	    char * ptr = d->d_name;

	    if (*ptr != 'S' && *ptr != 'K')
		continue;
	    ptr++;

	    if (strspn(ptr, "0123456789") != 2)
		continue;
	    ptr += 2;

	    if (stat(d->d_name, &st_script) < 0)
		xremove(d->d_name);	/* dangling sym link */

	    if (defaults && notincluded(ptr, runlevel))
		xremove(d->d_name);
	}

	/*
	 * Seek for scripts which are included, link or
	 * correct order number if necessary.
	 */

	while (foreach(&script, &order, runlevel)) {
	    char * clink;
	    boolean found, this = chkfor(script, argv, argc);

	    if (*script == '$')		/* Do not link in virtual dependencies */
		continue;

	    sprintf(olink, "../%s",   script);
	    sprintf(nlink, "S%.2d%s", order, script);

	    found = false;
	    rewinddir(rcdir);
	    while ((clink = scan_for(rcdir, script, 'S'))) {
		found = true;
		if (strcmp(clink, nlink)) {
		    xremove(clink);		/* Wrong order, remove link */
		    if (!this)
			xsymlink(olink, nlink);	/* Not ours, but correct order */
		    if (this && !del)
			xsymlink(olink, nlink);	/* Restore, with correct order */
		} else {
		    if (del && this)
			xremove(clink);		/* Found it, remove link */
		}
	    }

	    if (this) {
		/*
		 * If we haven't found it and we shouldn't delete it
		 * we try to add it.
		 */
		if (!del && !found)
		    xsymlink(olink, nlink);
	    }

	    /* Start link done, now Kill link */

	    if (!strcmp(script, "kbd"))
		continue;  /* kbd should run on any runlevel change */

	    sprintf(nlink, "K%.2d%s", (maxorder + 1) - order, script);

	    found = false;
	    rewinddir(rcdir);
	    while ((clink = scan_for(rcdir, script, 'K'))) {
		found = true;
		if (strcmp(clink, nlink)) {
		    xremove(clink);		/* Wrong order, remove link */
		    if (!this)
			xsymlink(olink, nlink);	/* Not ours, but correct order */
		    if (this && !del)
			xsymlink(olink, nlink);	/* Restore, with correct order */
		} else {
		    if (del && this)
			xremove(clink);		/* Found it, remove link */
		}
	    }

	    /*
	     * One way runlevels:
	     * Remove kill links from rc0.d/, rc6.d/, and boot.d/.
	     */
	    if (runlevel < 1 || runlevel == 6 || runlevel > 7) {
		if (found)
		    xremove(nlink);
	    } else {
		if (this) {
		    /*
		     * If we haven't found it and we shouldn't delete it
		     * we try to add it.
		     */
		    if (!del && !found)
			xsymlink(olink, nlink);
		}
	    }
	}
	popd();
	closedir(rcdir);
    }
# else  /* SUSE */
#  error Additive link scheme not yet implemented
# endif /* SUSE */
#endif

    /*
     * Back to the root(s)
     */
    popd();

    return 0;
}