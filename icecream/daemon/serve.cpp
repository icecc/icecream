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
#include "client_comm.h"
#include "arg.h"
#include "tempfile.h"

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

/**
 * Run @p argv in a child asynchronously.
 *
 * stdin, stdout and stderr are redirected as shown, unless those
 * filenames are NULL.
 *
 * Better server-side load limitation must still be organized.
 *
 * @warning When called on the daemon, where stdin/stdout may refer to random
 * network sockets, all of the standard file descriptors must be redirected!
 **/
int dcc_spawn_child(char **argv, pid_t *pidptr,
                    const char *stdin_file,
                    const char *stdout_file,
                    const char *stderr_file)
{
    pid_t pid;

    printf( "forking to execute " );
    for ( int i = 0; argv[i]; i++ )
        printf( "%s ", argv[i] );
    printf( "\n" );

#if 0
    pid = fork();
    if (pid == -1) {
        rs_log_error("failed to fork: %s", strerror(errno));
        return EXIT_OUT_OF_MEMORY; /* probably */
    } else if (pid == 0) {
        exit(dcc_inside_child(argv, stdin_file, stdout_file, stderr_file));
        /* !! NEVER RETURN FROM HERE !! */
    } else {
        *pidptr = pid;
        rs_trace("child started as pid%d", (int) pid);
        return 0;
    }
#endif
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
 * Blocking wait for a child to exit.  This is used when waiting for
 * cpp, gcc, etc.
 *
 * This is not used by the daemon-parent; it has its own
 * implementation in dcc_reap_kids().  They could be unified, but the
 * parent only waits when it thinks a child has exited; the child
 * waits all the time.
 **/
int dcc_collect_child(const char *what, pid_t pid,
                      int *wait_status)
{
    struct rusage ru;
    pid_t ret_pid;

    while (1) {
        if ((ret_pid = wait4(pid, wait_status, 0, &ru)) != -1) {
            /* This is not the main user-visible message, that comes from
             * critique_status(). */
            rs_trace("%s child %ld terminated with status %#x",
                     what, (long) ret_pid, *wait_status);

            rs_log_info("%s times: user %ld.%06lds, system %ld.%06lds, "
                        "%ld minflt, %ld majflt",
                        what,
                        ru.ru_utime.tv_sec, ru.ru_utime.tv_usec,
                        ru.ru_stime.tv_sec, ru.ru_stime.tv_usec,
                        ru.ru_minflt, ru.ru_majflt);

            return 0;
        } else if (errno == EINTR) {
            rs_trace("wait4 was interrupted; retrying");
            continue;
        } else {
            rs_log_error("sys_wait4(pid=%d) borked: %s", (int) pid, strerror(errno));
            return EXIT_DISTCC_FAILED;
        }
    }
}


/**
 * Read a request, run the compiler, and send a response.
 **/
int run_job(int in_fd,
            int out_fd)
{
    int status;
    char *temp_i, *temp_o, *err_fname, *out_fname;
    int ret, compile_ret;
    pid_t cc_pid;

    /* Ignore SIGPIPE; we consistently check error codes and will see the
     * EPIPE.  Note that it is set back to the default behaviour when spawning
     * a child, to handle cases like the assembler dying while its being fed
     * from the compiler */
    dcc_ignore_sigpipe(1);

    Client_Message m;
    client_read_message( in_fd, &m );
    if ( m.type != C_VERSION && ( int )m.length != ICECC_PROTO_VERSION ) {
        close( in_fd );
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }

    ret = client_read_message( in_fd, &m );
    if ( ret )
        return ret;

    if ( m.type != C_ARGC )
        return EXIT_PROTOCOL_ERROR;

    int argc = m.length;
    printf( "argc = %d\n", argc );
    char **argv = new char*[argc + 1 +1 ]; // +1 for -fpreprocessed
    for ( int i = 0; i < argc; i++ ) {
        ret = client_read_message( in_fd, &m );
        if ( ret )
            return ret;
        if ( m.type != C_ARGV )
            return EXIT_PROTOCOL_ERROR;
        argv[i] = new char[m.length + 1];
        read( in_fd, argv[i], m.length );
        argv[i][m.length] = 0;
        printf( "argv[%d] = '%s'\n", i, argv[i] );
    }
    argv[argc++] = strdup( "-fpreprocessed" );
    argv[argc] = 0;

    // TODO: PROF data if available

    size_t preproc_length = 0;
    size_t preproc_bufsize = 8192;
    char *preproc = ( char* )malloc( preproc_bufsize );

    while ( 1 ) {
        ret = client_read_message( in_fd, &m );
        if ( ret )
            return ret;

        if ( m.type == C_DONE )
            break;

        if ( m.type != C_PREPROC )
            return EXIT_PROTOCOL_ERROR;
        if ( preproc_length + m.length > preproc_bufsize ) {
            preproc_bufsize *= 2;
            preproc = (char* )realloc(preproc, preproc_bufsize);
        }
        if ( read( in_fd, preproc + preproc_length, m.length ) != ( ssize_t )m.length )
            return EXIT_PROTOCOL_ERROR;
        preproc_length += m.length;
    }

    printf( "pre %s\n", preproc );

    /* NOW WE'VE GOT ALL DATA AND SHOULD DISTRIBUTE */
    const char *orig_input;
    const char *orig_output;

    find_in_and_out(argv, &orig_input, &orig_output);

    const char *dot = strrchr(orig_input, '.');
    if (dot && dot[1] == '\0')
        dot = NULL;

    // if using gcc: dot = dcc_preproc_exten( dot );
    char tmp_input[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input ) ) != 0 )
        return ret;
    char tmp_output[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", ".o", tmp_output ) ) != 0 )
        return ret;

    for ( int i = 0; i < argc; i++ ) {
        if ( argv[i] == orig_input )
            argv[i] = tmp_input;
        else if ( argv[i] == orig_output )
            argv[i] = tmp_output;
    }

    FILE *ti = fopen( tmp_input, "wt" );
    fwrite( preproc, 1, preproc_length, ti );
    fclose( ti );

    if ((compile_ret = dcc_spawn_child(argv, &cc_pid,
                                       "/dev/null", out_fname, err_fname))
        || (compile_ret = dcc_collect_child("cc", cc_pid, &status))) {
        /* We didn't get around to finding a wait status from the actual compiler */
        status = W_EXITCODE(compile_ret, 0);
    }

#if 0
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

#endif
    out_cleanup:
    // dcc_remove_log_to_file();
    // dcc_cleanup_tempfiles();

    return ret;
}
