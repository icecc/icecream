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

#include <job.h>

#include "exitcode.h"
#include "client_comm.h"
#include "arg.h"
#include "tempfile.h"
#include "workit.h"

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

const char * dcc_find_basename(const char *sfile)
{
    char *slash;

    if (!sfile)
        return sfile;

    slash = strrchr(sfile, '/');

    if (slash == NULL || slash[1] == '\0')
        return sfile;

    return slash+1;
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
    char **argv = new char*[argc +1 ];
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

    CompileJob j;
    std::string bn = dcc_find_basename( argv[0] );
    if ( bn == "gcc" || bn == "cc" )
        j.setLanguage( CompileJob::Lang_C );
    else if ( bn == "g++" || bn == "c++" )
        j.setLanguage( CompileJob::Lang_CXX );
    else if ( bn == "objc" )
        j.setLanguage( CompileJob::Lang_OBJC );
    else if ( bn == "as" )
        j.setLanguage( CompileJob::Lang_ASM );
    else {
        printf( "Not a known compiler: %s\n", bn.c_str() );
        return EXIT_PROTOCOL_ERROR;
    }

    find_flags( argv, j );
    work_it( j, preproc, preproc_length );

    return ret;
}
