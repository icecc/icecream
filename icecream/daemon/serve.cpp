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
#include "client_comm.h"
#include "tempfile.h"
#include "workit.h"
#include "logging.h"
#include "serve.h"
#include <sys/sendfile.h>
// #include <scheduler.h>

using namespace std;

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

int client_write_message( int fd, enum ClientMsgType type, const string& str )
{
    int ret;
    if ( ( ret = client_write_message( fd, type, str.size() ) ) != 0 )
        return ret;
    if ( write( fd, str.c_str(), str.size() ) != ( ssize_t )str.size() )
        return -1;
    return 0;
}

/**
 * Read a request, run the compiler, and send a response.
 **/
int handle_connection( MsgChannel *serv )
{
    int ret;

    /* Ignore SIGPIPE; we consistently check error codes and will see the
     * EPIPE.  Note that it is set back to the default behaviour when spawning
     * a child, to handle cases like the assembler dying while its being fed
     * from the compiler */
    dcc_ignore_sigpipe(1);

    Msg *msg = serv->get_msg();
    if ( !msg ) {
        log_error() << "no message?\n";
        return EXIT_PROTOCOL_ERROR;
    }

    if ( msg->type == M_GET_SCHEDULER ) {
        UseSchedulerMsg m( *scheduler_host, scheduler_port );
        serv->send_msg( m );
        return 0;
    }

    if ( msg->type != M_COMPILE_FILE ) {
        log_error() << "not compile\n";
        return EXIT_PROTOCOL_ERROR;
    }

    CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->job;
    delete msg;

    // teambuilder needs faking
    const char *dot;
    if (job->language() == CompileJob::Lang_C)
        dot = ".c";
    else if (job->language() == CompileJob::Lang_CXX)
        dot = ".cpp";
    else
        assert(0);
    CompileJob::ArgumentsList list= job->restFlags();
    list.push_back( "-fpreprocessed" );
    job->setRestFlags( list );
    // end faking

    char tmp_input[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input ) ) != 0 ) {
        delete job;
        return ret;
    }

    int ti = open( tmp_input, O_CREAT|O_WRONLY|O_LARGEFILE );
    if ( ti == -1 ) {
        delete job;
        log_error() << "open failed\n";
        return EXIT_DISTCC_FAILED;
    }

    while ( 1 ) {
        msg = serv->get_msg();
        if ( !msg )
            return EXIT_PROTOCOL_ERROR;

        if ( msg->type == M_END )
            break;

        if ( msg->type != M_FILE_CHUNK )
            return EXIT_PROTOCOL_ERROR;

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>( msg );
        if ( !fcmsg )
            return EXIT_PROTOCOL_ERROR;

        if ( write( ti, fcmsg->buffer, fcmsg->len ) != ( ssize_t )fcmsg->len )
            return EXIT_DISTCC_FAILED;
    }
    close( ti );

    CompileResultMsg rmsg;

    string filename;
    ret = work_it( *job, tmp_input,
                   rmsg.out, rmsg.err, rmsg.status, filename );
    unlink( tmp_input );

    if ( ret )
        return ret;

    if ( !serv->send_msg( rmsg ) ) {
        log_info() << "write of result failed\n";
        return EXIT_DISTCC_FAILED;
    }

    trace() << "wrote all stream data\n";

    int obj_fd = open( filename.c_str(), O_RDONLY|O_LARGEFILE );
    if ( obj_fd == -1 ) {
        log_error() << "open failed\n";
    }

    unsigned char buffer[50000];
    do {
        ssize_t bytes = read(obj_fd, buffer, sizeof(buffer));
        if (!bytes)
            break;
        FileChunkMsg fcmsg( buffer, bytes );
        if ( !serv->send_msg( fcmsg ) ) {
            log_info() << "write of chunk failed" << endl;
            return EXIT_DISTCC_FAILED;
        }
    } while (1);
    EndMsg emsg;
    serv->send_msg( emsg );

    close( obj_fd );
    unlink( filename.c_str() );

    return 0;
}
