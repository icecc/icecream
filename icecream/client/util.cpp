/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78; -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <netdb.h>

#include <sys/stat.h>
#include <sys/time.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <sys/param.h>

#include "util.h"
#include "exitcode.h"
#include "logging.h"

using namespace std;

			/* I will make a man more precious than fine
			 * gold; even a man than the golden wedge of
			 * Ophir.
			 *		-- Isaiah 13:12 */


int str_endswith(const char *tail, const char *tiger)
{
        size_t len_tail = strlen(tail);
	size_t len_tiger = strlen(tiger);

	if (len_tail > len_tiger)
		return 0;

	return !strcmp(tiger + len_tiger - len_tail, tail);
}


int str_startswith(const char *head, const char *worm)
{
    return !strncmp(head, worm, strlen(head));
}



/**
 * Skim through NULL-terminated @p argv, looking for @p s.
 **/
int argv_contains(char **argv, const char *s)
{
	while (*argv) {
		if (!strcmp(*argv, s))
			return 1;
		argv++;
	}
	return 0;
}

char *dcc_gethostname(void)
{
    static char myname[100] = "\0";

    if (!myname[0]) {
        if (gethostname(myname, sizeof myname - 1) == -1)
            strcpy(myname, "UNKNOWN");
    }

    return myname;
}

/**
 * Set the `FD_CLOEXEC' flag of DESC if VALUE is nonzero,
 * or clear the flag if VALUE is 0.
 *
 * From the GNU C Library examples.
 *
 * @returns 0 on success, or -1 on error with `errno' set.
 **/
int set_cloexec_flag (int desc, int value)
{
    int oldflags = fcntl (desc, F_GETFD, 0);
    /* If reading the flags failed, return error indication now. */
    if (oldflags < 0)
        return oldflags;
    /* Set just the flag we want to set. */
    if (value != 0)
        oldflags |= FD_CLOEXEC;
    else
        oldflags &= ~FD_CLOEXEC;
    /* Store modified flag word in the descriptor. */
    return fcntl (desc, F_SETFD, oldflags);
}


/**
 * Ignore or unignore SIGPIPE.
 *
 * The server and child ignore it, because distcc code wants to see
 * EPIPE errors if something goes wrong.  However, for invoked
 * children it is set back to the default value, because they may not
 * handle the error properly.
 **/
int dcc_ignore_sigpipe(int val)
{
    if (signal(SIGPIPE, val ? SIG_IGN : SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, " << ( val ? "ignore" : "default" ) << ") failed: "
                      << strerror(errno) << endl;
        return EXIT_DISTCC_FAILED;
    }
    return 0;
}

/**
 * If we find a distcc masquerade dir on the PATH, remove all the dirs up
 * to that point.
 **/
int dcc_trim_path(const char *compiler_name)
{
    const char *envpath, *newpath, *p, *n;
    char linkbuf[MAXPATHLEN], *buf;
    struct stat sb;
    int len;

    if (!(envpath = getenv("PATH"))) {
        trace() << "PATH seems not to be defined" << endl;
        return 0;
    }

    trace() << "original PATH " << envpath << endl;

    /* Allocate a buffer that will let us append "/cc" onto any PATH
     * element, even if there is only one item in the PATH. */
    if (!(buf = ( char* )malloc(strlen(envpath)+1+strlen(compiler_name)+1))) {
        log_error() << "failed to allocate buffer for PATH munging" << endl;
        return EXIT_OUT_OF_MEMORY;
    }

    for (n = p = envpath, newpath = NULL; *n; p = n) {
        n = strchr(p, ':');
        if (n)
            len = n++ - p;
        else {
            len = strlen(p);
            n = p + len;
        }
        strncpy(buf, p, len);

        sprintf(buf + len, "/%s", compiler_name);
        if (lstat(buf, &sb) == -1)
            continue;           /* ENOENT, EACCESS, etc */
        if (!S_ISLNK(sb.st_mode))
            break;
        if ((len = readlink(buf, linkbuf, sizeof linkbuf)) <= 0)
            continue;
        linkbuf[len] = '\0';
        if (strstr(linkbuf, "distcc")) {
            /* Set newpath to the part of the PATH past our match. */
            newpath = n;
        }
    }

    if (newpath) {
        int ret = setenv("PATH", newpath, 1);
        if (ret)
            return ret;
    } else
        trace() << "not modifying PATH" << endl;

    free(buf);
    return 0;
}

/* Return the supplied path with the current-working directory prefixed (if
 * needed) and all "dir/.." references removed.  Supply path_len if you want
 * to use only a substring of the path string, otherwise make it 0. */
char *dcc_abspath(const char *path, int path_len)
{
    static char buf[MAXPATHLEN];
    unsigned len;
    char *p, *slash;

    if (*path == '/')
        len = 0;
    else {
#ifdef HAVE_GETCWD
        getcwd(buf, sizeof buf);
#else
        getwd(buf);
#endif
        len = strlen(buf);
        if (len >= sizeof buf) {
            log_error() << "getwd overflowed in dcc_abspath()" << endl;
            abort();
        }
        buf[len++] = '/';
    }
    if (path_len <= 0)
        path_len = strlen(path);
    if (path_len >= 2 && *path == '.' && path[1] == '/') {
	path += 2;
	path_len -= 2;
    }
    if (len + (unsigned)path_len >= sizeof buf) {
        log_error() << "path overflowed in dcc_abspath()" << endl;
        exit(EXIT_OUT_OF_MEMORY);
    }
    strncpy(buf + len, path, path_len);
    buf[len + path_len] = '\0';
    for (p = buf+len-(len > 0); (p = strstr(p, "/../")) != NULL; p = slash) {
	*p = '\0';
	if (!(slash = strrchr(buf, '/')))
	    slash = p;
	strcpy(slash, p+3);
    }
    return buf;
}


int dcc_remove_if_exists(const char *fname)
{
    if (unlink(fname) && errno != ENOENT) {
        log_warning() << "failed to unlink " << fname << ": "
                      << strerror(errno) << endl;
        return EXIT_IO_ERROR;
    }
    return 0;
}


#ifndef HAVE_STRLCPY
/* like strncpy but does not 0 fill the buffer and always null
   terminates. bufsize is the size of the destination buffer */
 size_t strlcpy(char *d, const char *s, size_t bufsize)
{
	size_t len = strlen(s);
	size_t ret = len;
	if (bufsize <= 0) return 0;
	if (len >= bufsize) len = bufsize-1;
	memcpy(d, s, len);
	d[len] = 0;
	return ret;
}
#endif

