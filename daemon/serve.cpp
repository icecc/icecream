/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

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

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#include <exception>
#include <sys/time.h>

#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#ifndef _PATH_TMP
#define _PATH_TMP "/tmp"
#endif

using namespace std;

int nice_level = 5;

class myexception: public exception
{
    int code;
public:
    myexception( int _exitcode ) : exception(), code( _exitcode ) {}
    int exitcode() const { return code; }
};

/**
 * Read a request, run the compiler, and send a response.
 **/
int handle_connection( const string &basedir, CompileJob *job,
                       MsgChannel *client, int &out_fd,
                       unsigned int mem_limit, uid_t nobody_uid, gid_t nobody_gid )
{
    int socket[2];
    if ( pipe( socket ) == -1)
        return -1;

    pid_t pid = fork();
    if ( pid != 0) { // parent
        close( socket[1] );
        out_fd = socket[0];
        return pid;
    }

    reset_debug(0);
    close( socket[0] );
    out_fd = socket[1];

    nice( nice_level );

    Msg *msg = 0; // The current read message
    char tmp_input[PATH_MAX];
    tmp_input[0] = 0;
    int ti = 0; // the socket
    unsigned int job_id = 0;
    int obj_fd = -1; // the obj_fd
    string obj_file;

    try {
        if ( job->environmentVersion().size() ) {
	    log_info() << "should use " << job->environmentVersion() << "(" << job->targetPlatform() << ") " << job->jobID() << endl;
            string dirname = basedir + "/target=" + job->targetPlatform() + "/" + job->environmentVersion();
            if ( ::access( string( dirname + "/usr/bin/gcc" ).c_str(), X_OK ) ) {
                log_error() << "I don't have environment " << job->environmentVersion() << "(" << job->targetPlatform() << ") " << job->jobID() << endl;
                throw myexception( EXIT_DISTCC_FAILED ); // the scheduler didn't listen to us!
            }

            if ( getuid() == 0 ) {
                // without the chdir, the chroot will escape the
                // jail right away
                if ( chdir( dirname.c_str() ) < 0 ) {
                    log_perror("chdir() failed" );
                    exit(145);
                }
                if ( chroot( dirname.c_str() ) < 0 ) {
                    log_perror("chroot() failed" );
                    exit(144);
                }
                if ( setgid( nobody_gid ) < 0 ) {
                    log_perror("setgid() failed" );
                    exit(143);
                }
                if ( setuid( nobody_uid ) < 0) {
                    log_perror("setuid() failed" );
                    exit(142);
                }
            }
            else
                if ( chdir( dirname.c_str() ) ) {
                    log_perror( "chdir" );
                } else {
                    trace() << "chdir to " << dirname << endl;
                }
        }
        else
            chdir( "/" );

        if ( ::access( _PATH_TMP + 1, W_OK ) ) {
            log_error() << "can't write into " << _PATH_TMP << " " << strerror( errno ) << endl;
            throw myexception( -1 );
        }

        const char *dot;
        if (job->language() == CompileJob::Lang_C)
            dot = ".i";
        else if (job->language() == CompileJob::Lang_CXX)
            dot = ".ii";
        else
            assert(0);

        int ret;
        if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input, 1 ) ) != 0 ) {
            log_error() << "can't create tmpfile " << strerror( errno ) << endl;
            throw myexception( ret );
        }

        ti = open( tmp_input, O_CREAT|O_WRONLY|O_LARGEFILE );
        if ( ti == -1 ) {
            log_error() << "open of " << tmp_input << " failed " << strerror( errno ) << endl;
            throw myexception( EXIT_DISTCC_FAILED );
        }

        unsigned int in_compressed = 0;
        unsigned int in_uncompressed = 0;

        while ( 1 ) {
            msg = client->get_msg(60);

            if ( !msg ) {
                log_error() << "no message while reading file chunk\n";
                throw myexception( EXIT_PROTOCOL_ERROR );
            }

            if ( msg->type == M_END ) {
                delete msg;
                msg = 0;
                break;
            }

            if ( msg->type != M_FILE_CHUNK ) {
                log_error() << "protocol error while looking for FILE_CHUNK\n";
                throw myexception( EXIT_PROTOCOL_ERROR );
            }

            FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>( msg );
            if ( !fcmsg ) {
                log_error() << "FileChunkMsg not dynamic_castable\n";
                throw myexception( EXIT_PROTOCOL_ERROR );
            }
            in_uncompressed += fcmsg->len;
            in_compressed += fcmsg->compressed;

            ssize_t len = fcmsg->len;
            off_t off = 0;
            while ( len ) {
                ssize_t bytes = write( ti, fcmsg->buffer + off, len );
                if ( bytes == -1 ) {
                    log_error() << "write to " << tmp_input << " failed. " << strerror( errno ) << endl;
                    throw myexception( EXIT_DISTCC_FAILED );
                }
                len -= bytes;
                off += bytes;
            }

            delete msg;
            msg = 0;
        }
        close( ti );
        ti = -1;

        struct timeval begintv;
        gettimeofday( &begintv, 0 );

        CompileResultMsg rmsg;

        ret = work_it( *job, tmp_input,
                       rmsg.out, rmsg.err, rmsg.status, obj_file, mem_limit, client->fd );

        unlink( tmp_input );
        tmp_input[0] = 0; // unlinked

        job_id = job->jobID();
        delete job;
        job = 0;

        if ( ret ) {
            if ( ret == EXIT_OUT_OF_MEMORY ) { // we catch that as special case
                rmsg.was_out_of_memory = true;
            } else {
                throw myexception( ret );
            }
        }

        if ( !client->send_msg( rmsg ) ) {
            log_info() << "write of result failed\n";
            throw myexception( EXIT_DISTCC_FAILED );
        }

        if ( rmsg.status == 0 ) {
            obj_fd = open( obj_file.c_str(), O_RDONLY|O_LARGEFILE );
            if ( obj_fd == -1 ) {
                log_error() << "open failed\n";
                throw myexception( EXIT_DISTCC_FAILED );
            }

            unsigned int out_compressed = 0;
            unsigned int out_uncompressed = 0;

            unsigned char buffer[100000];
            do {
                ssize_t bytes = read(obj_fd, buffer, sizeof(buffer));
                if ( bytes < 0 )
                {
                    if ( errno == EINTR )
                        continue;
                    throw myexception( EXIT_DISTCC_FAILED );
                }
                if ( !bytes )
                    break;
                out_uncompressed += bytes;
                FileChunkMsg fcmsg( buffer, bytes );
                if ( !client->send_msg( fcmsg ) ) {
                    log_info() << "write of obj chunk failed " << bytes << endl;
                    throw myexception( EXIT_DISTCC_FAILED );
                }
                out_compressed += fcmsg.compressed;
            } while (1);

            struct timeval endtv;
            gettimeofday(&endtv, 0 );
            unsigned int duration = ( endtv.tv_sec - begintv.tv_sec ) * 1000 + ( endtv.tv_usec - begintv.tv_usec ) / 1000;
            write( out_fd, &in_compressed, sizeof( unsigned int ) );
            write( out_fd, &in_uncompressed, sizeof( unsigned int ) );
            write( out_fd, &out_compressed, sizeof( unsigned int ) );
            write( out_fd, &out_uncompressed, sizeof( unsigned int ) );
            write( out_fd, &duration, sizeof( unsigned int ) );
        }

        throw myexception( rmsg.status );

    } catch ( myexception e )
    {
        if ( client && e.exitcode() == 0 )
            client->send_msg( EndMsg() );
        delete client;
        client = 0;

        delete msg;
        delete job;

        if ( *tmp_input )
            unlink( tmp_input );

        if ( ti > -1 )
            close( ti );

        if ( obj_fd > -1)
            close( obj_fd );

        if ( !obj_file.empty() )
            unlink( obj_file.c_str() );

        if ( out_fd > -1)
            close( out_fd );

        exit( e.exitcode() );
    }
}
