/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
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

                /* He who waits until circumstances completely favour *
                 * his undertaking will never accomplish anything.    *
                 *              -- Martin Luther                      */


/**
 * @file
 *
 * Actually serve remote requests.  Called from daemon.c.
 *
 * @todo Make sure wait statuses are packed in a consistent format
 * (exit<<8 | signal).  Is there any platform that doesn't do this?
 *
 * @todo The server should catch signals, and terminate the compiler process
 * group before handling them.
 *
 * @todo It might be nice to detect that the client has dropped the
 * connection, and then kill the compiler immediately.  However, we probably
 * won't notice that until we try to do IO.  SIGPIPE won't help because it's
 * not triggered until we try to do IO.  I don't think it matters a lot,
 * though, because the client's not very likely to do that.  The main case is
 * probably somebody getting bored and interrupting compilation.
 *
 * What might help is to select() on the network socket while we're waiting
 * for the child to complete, allowing SIGCHLD to interrupt the select() when
 * the child completes.  However I'm not sure if it's really worth the trouble
 * of doing that just to handle a fairly marginal case.
 **/



#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_SIGNAL_H
#  include <sys/signal.h>
#endif /* HAVE_SYS_SIGNAL_H */
#include <sys/param.h>

#include "exitcode.h"

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
        rs_log_warning("signal(SIGPIPE, %s) failed: %s",
                       val ? "ignore" : "default",
                       strerror(errno));
        return EXIT_DISTCC_FAILED;
    }
    return 0;
}


#if 0
/**
 * Find the absolute path for the first occurrence of @p compiler_name on the
 * PATH.  Print a warning if it looks like a symlink to distcc.
 *
 * We want to guard against somebody accidentally running the server with a
 * masqueraded compiler on its $PATH.  The worst that's likely to happen here
 * is wasting some time running a distcc or ccache client that does nothing,
 * so it's not a big deal.  (This could be easy to do if it's on the default
 * PATH and they start the daemon from the command line.)
 *
 * At the moment we don't look for the compiler too.
 **/
static int dcc_check_compiler_masq(char *compiler_name)
{
    const char *envpath, *newpath, *p, *n;
    char *buf = NULL;
    struct stat sb;
    int len;
    char linkbuf[MAXPATHLEN];

    if (compiler_name[0] == '/')
        return 0;

    if (!(envpath = getenv("PATH"))) {
        rs_trace("PATH seems not to be defined");
        return 0;
    }

    for (n = p = envpath, newpath = NULL; *n; p = n) {
        n = strchr(p, ':');
        if (n)
            len = n++ - p;
        else {
            len = strlen(p);
            n = p + len;
        }
        if (asprintf(&buf, "%.*s/%s", len, p, compiler_name) == -1) {
            rs_log_crit("asnprintf failed");
            return EXIT_DISTCC_FAILED;
        }

        if (lstat(buf, &sb) == -1)
            continue;           /* ENOENT, EACCESS, etc */
        if (!S_ISLNK(sb.st_mode)) {
            rs_trace("%s is not a symlink", buf);
            break;              /* found it */
        }
        if ((len = readlink(buf, linkbuf, sizeof linkbuf)) <= 0)
            continue;
        linkbuf[len] = '\0';

        if (strstr(linkbuf, "distcc")) {
            rs_log_warning("%s on distccd's path is %s and really a link to %s",
                           compiler_name, buf, linkbuf);
            break;              /* but use it anyhow */
        } else {
            rs_trace("%s is a safe symlink to %s", buf, linkbuf);
            break;              /* found it */
        }
    }

    free(buf);
    return 0;
}

#endif

/**
 * Read a request, run the compiler, and send a response.
 **/
int run_job(int in_fd,
            int out_fd)
{
    char **argv;
    int status;
    char *temp_i, *temp_o, *err_fname, *out_fname;
    int ret, compile_ret;
    char *orig_input, *orig_output;
    pid_t cc_pid;

    /* Ignore SIGPIPE; we consistently check error codes and will see the
     * EPIPE.  Note that it is set back to the default behaviour when spawning
     * a child, to handle cases like the assembler dying while its being fed
     * from the compiler */
    dcc_ignore_sigpipe(1);

    if ((ret = dcc_r_request_header(in_fd, &protover))
        || (ret = dcc_r_argv(in_fd, &argv))
        || (ret = dcc_scan_args(argv, &orig_input, &orig_output, &argv)))
        goto out_cleanup;

    rs_trace("output file %s", orig_output);

    if ((ret = dcc_input_tmpnam(orig_input, &temp_i)))
        goto out_cleanup;
    if ((ret = dcc_make_tmpnam("distccd", ".o", &temp_o)))
        goto out_cleanup;

    //    compr = (protover == 2) ? DCC_COMPRESS_LZO1X : DCC_COMPRESS_NONE;

    if ((ret = dcc_r_token_file(in_fd, "DOTI", temp_i, compr))
        || (ret = dcc_set_input(argv, temp_i))
        || (ret = dcc_set_output(argv, temp_o)))
        goto out_cleanup;

    if ((ret = dcc_check_compiler_masq(argv[0])))
        goto out_cleanup;

    if ((compile_ret = dcc_spawn_child(argv, &cc_pid,
                                       "/dev/null", out_fname, err_fname))
        || (compile_ret = dcc_collect_child("cc", cc_pid, &status))) {
        /* We didn't get around to finding a wait status from the actual compiler */
        status = W_EXITCODE(compile_ret, 0);
    }

    if ((ret = dcc_x_result_header(out_fd, protover))
        || (ret = dcc_x_cc_status(out_fd, status))
        || (ret = dcc_x_file(out_fd, err_fname, "SERR", compr))
        || (ret = dcc_x_file(out_fd, out_fname, "SOUT", compr))
        || WIFSIGNALED(status)
        || WEXITSTATUS(status)) {
        /* Something went wrong, so send DOTO 0 */
        dcc_x_token_int(out_fd, "DOTO", 0);
    } else {
        ret = dcc_x_file(out_fd, temp_o, "DOTO", compr);
    }

    // dcc_critique_status(status, argv[0], dcc_hostdef_local, 0);
    // tcp_cork_sock(out_fd, 0);

    // rs_log(RS_LOG_INFO|RS_LOG_NONAME, "job complete");

    out_cleanup:
    // dcc_remove_log_to_file();
    // dcc_cleanup_tempfiles();

    return ret;
}
