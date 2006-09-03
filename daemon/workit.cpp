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
#include "workit.h"
#include "tempfile.h"
#include "assert.h"
#include "exitcode.h"
#include "logging.h"
#include <sys/select.h>
#include <algorithm>

#ifdef __FreeBSD__
#include <sys/param.h>
#endif

/* According to earlier standards */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/socket.h>

#ifdef __FreeBSD__
#include <signal.h>
#include <sys/resource.h>
#ifndef RUSAGE_SELF
#define   RUSAGE_SELF     (0)
#endif
#ifndef RUSAGE_CHILDREN
#define   RUSAGE_CHILDREN     (-1)
#endif
#endif

#include <stdio.h>
#include <errno.h>
#include <string>

#include "comm.h"

using namespace std;

// code based on gcc - Copyright (C) 1999, 2000, 2001, 2002 Free Software Foundation, Inc.

/* Heuristic to set a default for GGC_MIN_EXPAND.  */
static int
ggc_min_expand_heuristic(unsigned int mem_limit)
{
    double min_expand = mem_limit;

    /* The heuristic is a percentage equal to 30% + 70%*(RAM/1GB), yielding
       a lower bound of 30% and an upper bound of 100% (when RAM >= 1GB).  */
    min_expand /= 1024;
    min_expand *= 70;
    min_expand = std::min (min_expand, 70.);
    min_expand += 30;

    return int( min_expand );
}

/* Heuristic to set a default for GGC_MIN_HEAPSIZE.  */
static unsigned int
ggc_min_heapsize_heuristic(unsigned int mem_limit)
{
    /* The heuristic is RAM/8, with a lower bound of 4M and an upper
       bound of 128M (when RAM >= 1GB).  */
    mem_limit /= 8;
    mem_limit = std::max (mem_limit, 4U);
    mem_limit = std::min (mem_limit, 128U);

    return mem_limit * 1024;
}


volatile static bool must_reap = false;

static void theSigCHLDHandler( int )
{
    must_reap = true;
}

static void
error_client( MsgChannel *client, string error )
{
    if ( IS_PROTOCOL_23( client ) )
        client->send_msg( StatusTextMsg( error ) );
}

int work_it( CompileJob &j, unsigned int job_stat[], MsgChannel* client,
             CompileResultMsg& rmsg, string &outfilename, unsigned long int mem_limit, int client_fd )
{
    rmsg.out.erase(rmsg.out.begin(), rmsg.out.end());
    rmsg.out.erase(rmsg.out.begin(), rmsg.out.end());

    std::list<string> list = j.remoteFlags();
    appendList( list, j.restFlags() );
    int ret;

    char tmp_output[PATH_MAX];
    char prefix_output[PATH_MAX]; // I'm too lazy to calculate how many digits 2^64 is :)
    sprintf( prefix_output, "icecc-%d", j.jobID() );
    if ( ( ret = dcc_make_tmpnam(prefix_output, ".o", tmp_output, 1 ) ) != 0 )
        return ret;

    outfilename = tmp_output;

    int sock_err[2];
    int sock_out[2];
    int sock_in[2];
    int main_sock[2];

    if ( pipe( sock_err ) )
	return EXIT_DISTCC_FAILED;
    if ( pipe( sock_out ) )
	return EXIT_DISTCC_FAILED;
    if ( pipe( main_sock ) )
	return EXIT_DISTCC_FAILED;

    if ( fcntl( sock_out[0], F_SETFL, O_NONBLOCK ) )
	return EXIT_DISTCC_FAILED;
    if ( fcntl( sock_err[0], F_SETFL, O_NONBLOCK ) )
	return EXIT_DISTCC_FAILED;

    if ( fcntl( sock_out[0], F_SETFD, FD_CLOEXEC ) )
	return EXIT_DISTCC_FAILED;
    if ( fcntl( sock_err[0], F_SETFD, FD_CLOEXEC ) )
	return EXIT_DISTCC_FAILED;
    if ( fcntl( sock_out[1], F_SETFD, FD_CLOEXEC ) )
	return EXIT_DISTCC_FAILED;
    if ( fcntl( sock_err[1], F_SETFD, FD_CLOEXEC ) )
	return EXIT_DISTCC_FAILED;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sock_in) < 0)
        return EXIT_DISTCC_FAILED;

    int maxsize = 2*1024*2024;
#ifdef SO_SNDBUFFORCE
    if (setsockopt(sock_in[1], SOL_SOCKET, SO_SNDBUFFORCE, &maxsize, sizeof(maxsize)) < 0)
#endif
    {
        setsockopt(sock_in[1], SOL_SOCKET, SO_SNDBUF, &maxsize, sizeof(maxsize));
    }

    must_reap = false;

    /* Testing */
    struct sigaction act;
    sigemptyset( &act.sa_mask );

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigaction( SIGPIPE, &act, 0L );

    act.sa_handler = theSigCHLDHandler;
    act.sa_flags = SA_NOCLDSTOP;
    sigaction( SIGCHLD, &act, 0 );

    sigaddset( &act.sa_mask, SIGCHLD );
    // Make sure we don't block this signal. gdb tends to do that :-(
    sigprocmask( SIG_UNBLOCK, &act.sa_mask, 0 );
    char buffer[4096];

    pid_t pid = fork();
    if ( pid == -1 ) {
        close( sock_err[0] );
        close( sock_err[1] );
        close( main_sock[0] );
        close( main_sock[1] );
        close( sock_out[0] );
        close( sock_out[1] );
        close( sock_in[0] );
        close( sock_in[1] );
        close( sock_err[0] );
        unlink( tmp_output );
        return EXIT_OUT_OF_MEMORY;
    } else if ( pid == 0 ) {

        close( main_sock[0] );
        close( sock_in[1] );
        close( sock_out[0] );
        dup2( sock_in[0], 0);
        close (sock_in[0]);
        fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);
        setenv( "PATH", "usr/bin", 1 );
        // Safety check
        if (getuid() == 0 || getgid() == 0) {
            error_client( client, "UID is 0 - aborting." );
            _exit(142);
        }


#ifdef RLIMIT_AS
        struct rlimit rlim;
        if ( getrlimit( RLIMIT_AS, &rlim ) ) {
            error_client( client, "getrlimit failed." );
            log_perror( "getrlimit" );
        }

        rlim.rlim_cur = mem_limit*1024*1024;
        rlim.rlim_max = mem_limit*1024*1024;
        if ( setrlimit( RLIMIT_AS, &rlim ) ) {
            error_client( client, "setrlimit failed." );
            log_perror( "setrlimit" );
        }
#endif

        int argc = list.size();
        argc++; // the program
        argc += 6; // -x c - -o file.o -fpreprocessed
        argc += 4; // gpc parameters
        char **argv = new char*[argc + 1];
	int i = 0;
        if (j.language() == CompileJob::Lang_C)
            argv[i++] = strdup( "usr/bin/gcc" );
        else if (j.language() == CompileJob::Lang_CXX)
            argv[i++] = strdup( "usr/bin/g++" );
        else
            assert(0);

        //TODOlist.push_back( "-Busr/lib/gcc-lib/i586-suse-linux/3.3.1/" );

        for ( std::list<string>::const_iterator it = list.begin();
              it != list.end(); ++it) {
            argv[i++] = strdup( it->c_str() );
        }
        argv[i++] = strdup("-fpreprocessed");
        argv[i++] = strdup("-x");
        argv[i++] = strdup((j.language() == CompileJob::Lang_CXX) ? "c++" : "c");
        argv[i++] = strdup( "-" );
        argv[i++] = strdup( "-o" );
        argv[i++] = tmp_output;
        argv[i++] = strdup( "--param" );
        sprintf( buffer, "ggc-min-expand=%d", ggc_min_expand_heuristic( mem_limit ) );
        argv[i++] = strdup( buffer );
        argv[i++] = strdup( "--param" );
        sprintf( buffer, "ggc-min-heapsize=%d", ggc_min_heapsize_heuristic( mem_limit ) );
        argv[i++] = strdup( buffer );
        // before you add new args, check above for argc
        argv[i] = 0;
	if (i > argc)
	    printf ("Ohh bummer.  You can't count.\n");
#if 0
        printf( "forking " );
        for ( int index = 0; argv[index]; index++ )
            printf( "%s ", argv[index] );
        printf( "\n" );
#endif
        close_debug();
        dup2 (sock_out[1], STDOUT_FILENO );
        close(sock_out[1]);
        dup2( sock_err[1], STDERR_FILENO );
        close(sock_err[1]);

#ifdef ICECC_DEBUG
        for(int f = STDERR_FILENO+1; f < 4096; ++f) {
           long flags;
           assert((flags = fcntl(f, F_GETFD, 0)) < 0 || (flags & FD_CLOEXEC));
        }
#endif

        execv( argv[0], const_cast<char *const*>( argv ) ); // no return
        perror( "ICECC: execv" );

        char resultByte = 1;
        write(main_sock[1], &resultByte, 1);
        _exit(-1);
    } else {
        close( main_sock[1] );
        close( sock_in[0] );
        close( sock_out[1] );
        close( sock_err[1] );

        struct timeval starttv;
        gettimeofday(&starttv, 0 );

        for (;;) {
            Msg* msg  = client->get_msg(60);

            if ( !msg || (msg->type != M_FILE_CHUNK && msg->type != M_END) )
              {
                log_error() << "protocol error while reading preprocessed file\n";
                delete msg;
                msg = 0;
                close (sock_in[1]);
                while ( waitpid(pid, 0, 0) < 0 && errno == EINTR)
                    ;
                throw myexception (EXIT_IO_ERROR);
              }

            if ( msg->type == M_END )
              {
                delete msg;
                msg = 0;
                break;
              }

            FileChunkMsg *fcmsg = static_cast<FileChunkMsg*>( msg );
            job_stat[JobStatistics::in_uncompressed] += fcmsg->len;
            job_stat[JobStatistics::in_compressed] += fcmsg->compressed;

            ssize_t len = fcmsg->len;
            off_t off = 0;
            while ( len ) {
                ssize_t bytes = write( sock_in[1], fcmsg->buffer + off, len );
                if ( bytes < 0 && errno == EINTR )
                    continue;

                if ( bytes == -1 ) {
                    log_perror("write to caching socket failed. ");

                    fd_set rfds;
                    FD_ZERO( &rfds );
                    FD_SET( sock_err[0], &rfds );

                    struct timeval tv;
                    tv.tv_sec = 1;
                    tv.tv_usec = 0;

                    ret =  select( sock_err[0]+1, &rfds, 0, 0, &tv );

                    if ( ret > 0 && FD_ISSET(sock_err[0], &rfds) ) {
                        ssize_t bytes = read( sock_err[0], buffer, sizeof(buffer)-1 );
                        if ( bytes > 0 ) {

                            while ( bytes > 0 && buffer[bytes - 1] == '\n' )
                                bytes--;
                            buffer[bytes] = 0;

                            rmsg.err = buffer;
                        }
                    }

                    if ( rmsg.err.size() )
                        error_client( client, "compiler failed: " + rmsg.err );
                    else
                        error_client( client, "compiler failed." );
                    delete msg;
                    msg = 0;
                    kill( pid, SIGTERM ); // make sure it's dead
                    while ( waitpid(pid, 0, 0) < 0 && errno == EINTR)
                        ;
                    throw myexception (EXIT_COMPILER_CRASHED);
                    break;
                }
                len -= bytes;
                off += bytes;
            }

            delete msg;
            msg = 0;
        }
        close( sock_in[1] );
        log_block parent_wait("parent, waiting");
        // idea borrowed from kprocess
        for(;;)
        {
            char resultByte;
            ssize_t n = ::read(main_sock[0], &resultByte, 1);
            if (n == 1)
            {
                rmsg.status = resultByte;
                close(main_sock[0]);

                while ( waitpid(pid, 0, 0) < 0 && errno == EINTR)
                    ;
                unlink( tmp_output );
                error_client( client, "compiler did not start" );
                return EXIT_COMPILER_MISSING; // most likely cause
            }
            if (n == -1)
            {
                if (errno == EINTR)
                    continue; // Ignore
            }
            break; // success
        }
        close( main_sock[0] );

        log_block bwrite("write block");

        for(;;)
        {
            fd_set rfds;
            FD_ZERO( &rfds );
            if (sock_out[0] >= 0)
                FD_SET( sock_out[0], &rfds );
            if (sock_err[0] >= 0)
                FD_SET( sock_err[0], &rfds );
            FD_SET( client_fd, &rfds );

            int max_fd = std::max( sock_out[0], sock_err[0] );
            if ( client_fd > max_fd )
                max_fd = client_fd;

            ret = 0;
            if (!must_reap)
                ret = select( max_fd+1, &rfds, 0, 0, NULL );

            switch( ret )
            {
            default:
                if ( sock_out[0] >= 0 && FD_ISSET(sock_out[0], &rfds) ) {
                    ssize_t bytes = read( sock_out[0], buffer, sizeof(buffer)-1 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        rmsg.out.append( buffer );
                    }
                    else if (bytes == 0) {
                        close(sock_out[0]);
                        sock_out[0] = -1;
                    }
                }
                if ( sock_err[0] >= 0 && FD_ISSET(sock_err[0], &rfds) ) {
                    ssize_t bytes = read( sock_err[0], buffer, sizeof(buffer)-1 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        rmsg.err.append( buffer );
                    }
                    else if (bytes == 0) {
                        close(sock_err[0]);
                        sock_err[0] = -1;
                    }
                }
                if ( FD_ISSET( client_fd, &rfds ) ) {
                    rmsg.err.append( "client cancelled\n" );
                    close( client_fd );
                    kill( pid, SIGTERM );
                    while ( waitpid(pid, 0, 0) < 0 && errno == EINTR)
                        ;
                    return EXIT_CLIENT_KILLED;
                }
                // fall through
            case -1:
		if ( ret < 0 && errno != EINTR ) { // this usually means the logic broke
                    error_client( client, string( "select returned " ) + strerror( errno ) );
                    kill( pid, SIGTERM ); // make sure it's dead
                    while ( waitpid(pid, 0, 0) < 0 && errno == EINTR)
                        ;
                    return EXIT_DISTCC_FAILED;
                }
                // fall through; should happen if tvp->tv_sec < 0
            case 0:
                struct rusage ru;
                int status;

                if (wait4(pid, &status, must_reap ? WUNTRACED : WNOHANG, &ru) == pid)
                {
                    close( sock_err[0] );
                    close( sock_out[0] );

                    if ( !WIFEXITED(status) || WEXITSTATUS(status) ) {
                        unsigned long int mem_used = ( ru.ru_minflt + ru.ru_majflt ) * getpagesize() / 1024;
                        rmsg.status = EXIT_OUT_OF_MEMORY;

                        if ( mem_used * 100 > 85 * mem_limit * 1024 ||
                             rmsg.err.find( "memory exhausted" ) != string::npos )
                        {
                            // the relation between ulimit and memory used is pretty thin ;(
                            return EXIT_OUT_OF_MEMORY;
                        }
                    }

                    if ( WIFEXITED(status) ) {
                        struct timeval endtv;
                        gettimeofday(&endtv, 0 );
                        rmsg.status = WEXITSTATUS(status);
                        job_stat[JobStatistics::exit_code] = WEXITSTATUS(status);
                        job_stat[JobStatistics::real_msec] = (endtv.tv_sec - starttv.tv_sec) * 1000 +
                            (long(endtv.tv_usec) - long(starttv.tv_usec)) / 1000;
                        job_stat[JobStatistics::user_msec] = ru.ru_utime.tv_sec * 1000
                            + ru.ru_utime.tv_usec / 1000;
                        job_stat[JobStatistics::sys_msec] = ru.ru_stime.tv_sec * 1000
                            + ru.ru_stime.tv_usec / 1000;
                        job_stat[JobStatistics::sys_pfaults] = ru.ru_majflt + ru.ru_nswap + ru.ru_minflt;
                    }

                    return 0;
                }
                break;
            }
        }
    }
    assert( false );
    return 0;
}
