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
#include <cassert>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_SIGNAL_H
#  include <sys/signal.h>
#endif /* HAVE_SYS_SIGNAL_H */
#include <sys/param.h>

#include <job.h>
#include <comm.h>

#include "exitcode.h"
#include "tempfile.h"
#include "workit.h"
#include "logging.h"
#include "serve.h"
#include <sys/sendfile.h>
// #include <scheduler.h>

using namespace std;

/**
 * Read a request, run the compiler, and send a response.
 **/
int handle_connection( CompileJob *job, MsgChannel *serv )
{
    pid_t pid = fork();
    if ( pid == -1 )
        return EXIT_DISTCC_FAILED;

    if ( pid ) // parent
        return 0;

    if ( !scheduler || !scheduler->send_msg( JobBeginMsg( job->jobID() ) ) ) {
        log_error() << "can't reach scheduler to tell him about job start of " << job->jobID() << endl;
        exit ( EXIT_DISTCC_FAILED );
    }

    const char *dot;
    if (job->language() == CompileJob::Lang_C)
        dot = ".i";
    else if (job->language() == CompileJob::Lang_CXX)
        dot = ".ii";
    else
        assert(0);

    int ret;
    char tmp_input[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input ) ) != 0 ) {
        delete job;
        exit ( ret );
    }

    int ti = open( tmp_input, O_CREAT|O_WRONLY|O_LARGEFILE );
    if ( ti == -1 ) {
        delete job;
        log_error() << "open failed\n";
        exit ( EXIT_DISTCC_FAILED );
    }

    while ( 1 ) {
        Msg *msg = serv->get_msg();

        if ( !msg ) {
            delete job;
            log_error() << "no message while reading file chunk\n";
            exit ( EXIT_PROTOCOL_ERROR );
        }

        if ( msg->type == M_END ) {
            delete msg;
            break;
        }

        if ( msg->type != M_FILE_CHUNK ) {
            delete job;
            delete msg;
            exit ( EXIT_PROTOCOL_ERROR );
        }

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>( msg );
        if ( !fcmsg ) {
            delete job;
            delete msg;
            log_error() << "FileChunkMsg\n";
            exit( EXIT_PROTOCOL_ERROR );
        }
        if ( write( ti, fcmsg->buffer, fcmsg->len ) != ( ssize_t )fcmsg->len ) {
            delete job;
            delete msg;
            exit( EXIT_DISTCC_FAILED );
        }

        delete msg;
    }
    close( ti );

    CompileResultMsg rmsg;

    string filename;
    ret = work_it( *job, tmp_input,
                   rmsg.out, rmsg.err, rmsg.status, filename );
    unlink( tmp_input );
    unsigned int job_id = job->jobID();
    delete job;

    if ( ret )
        exit ( ret );

    if ( !serv->send_msg( rmsg ) ) {
        log_info() << "write of result failed\n";
        exit(  EXIT_DISTCC_FAILED );
    }

    int obj_fd = open( filename.c_str(), O_RDONLY|O_LARGEFILE );
    if ( obj_fd == -1 ) {
        log_error() << "open failed\n";
    }

    unsigned char buffer[100000];
    do {
        ssize_t bytes = read(obj_fd, buffer, sizeof(buffer));
        if (!bytes)
            break;
        FileChunkMsg fcmsg( buffer, bytes );
        if ( !serv->send_msg( fcmsg ) ) {
            log_info() << "write of chunk failed" << endl;
            exit( EXIT_DISTCC_FAILED );
        }
    } while (1);
    EndMsg emsg;
    serv->send_msg( emsg );

    scheduler->send_msg( JobDoneMsg( job_id ) );

    close( obj_fd );
    unlink( filename.c_str() );

    exit( 0 );
}
