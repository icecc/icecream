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
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_USER_H && !defined(__DragonFly__)
#  include <sys/user.h>
#endif
#include <sys/socket.h>

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__APPLE__)
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
#include "platform.h"

using namespace std;

static int death_pipe[2];

extern "C" {

static void theSigCHLDHandler( int )
{
    char foo = 0;
    write(death_pipe[1], &foo, 1);
}

}

static void
error_client( MsgChannel *client, string error )
{
    if ( IS_PROTOCOL_23( client ) )
        client->send_msg( StatusTextMsg( error ) );
}

/*
 * This is all happening in a forked child.
 * That means that we can block and be lazy about closing fds
 * (in the error cases which exit quickly).
 */

int work_it( CompileJob &j, unsigned int job_stat[], MsgChannel* client,
             CompileResultMsg& rmsg, const string &outfilename, 
             unsigned long int mem_limit, int client_fd, int /*job_in_fd*/ )
{
    rmsg.out.erase(rmsg.out.begin(), rmsg.out.end());
    rmsg.out.erase(rmsg.out.begin(), rmsg.out.end());

    std::list<string> list = j.remoteFlags();
    appendList( list, j.restFlags() );

    int sock_err[2];
    int sock_out[2];
    int sock_in[2];
    int main_sock[2];
    char buffer[4096];

    if ( pipe( sock_err ) )
	return EXIT_DISTCC_FAILED;
    if ( pipe( sock_out ) )
	return EXIT_DISTCC_FAILED;
    if ( pipe( main_sock ) )
	return EXIT_DISTCC_FAILED;
    if ( pipe( death_pipe ) )
        return EXIT_DISTCC_FAILED;

    // We use a socket pair instead of a pipe to get a "slightly" bigger
    // output buffer. This saves context switches and latencies.
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sock_in) < 0)
        return EXIT_DISTCC_FAILED;
    int maxsize = 2*1024*2024;
#ifdef SO_SNDBUFFORCE
    if (setsockopt(sock_in[1], SOL_SOCKET, SO_SNDBUFFORCE, &maxsize, sizeof(maxsize)) < 0)
#endif
    {
        setsockopt(sock_in[1], SOL_SOCKET, SO_SNDBUF, &maxsize, sizeof(maxsize));
    }

    if ( fcntl( sock_in[1], F_SETFL, O_NONBLOCK ) )
       return EXIT_DISTCC_FAILED;

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

    flush_debug();
    pid_t pid = fork();
    if ( pid == -1 ) {
        return EXIT_OUT_OF_MEMORY;
    } else if ( pid == 0 ) {

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
        argc += 1; // -pipe
        argc += 1; // -no-canonical-prefixes
        char **argv = new char*[argc + 1];
	int i = 0;
        bool clang = false;
        if (IS_PROTOCOL_30( client )) {
            assert(!j.compilerName().empty());
            clang = (j.compilerName().find( "clang" ) != string::npos);
            argv[i++] = strdup(( "usr/bin/" + j.compilerName()).c_str());
	} else {
            if (j.language() == CompileJob::Lang_C)
                argv[i++] = strdup( "usr/bin/gcc" );
            else if (j.language() == CompileJob::Lang_CXX)
                argv[i++] = strdup( "usr/bin/g++" );
            else
                assert(0);
        }

        argv[i++] = strdup("-x");
        argv[i++] = strdup((j.language() == CompileJob::Lang_CXX) ? "c++" : "c");

        bool hasPipe = false;
        for ( std::list<string>::const_iterator it = list.begin();
              it != list.end(); ++it) {
            if(*it == "-pipe")
                hasPipe = true;
            argv[i++] = strdup( it->c_str() );
        }
        if (!clang)
            argv[i++] = strdup("-fpreprocessed");
        if(!hasPipe)
           argv[i++] = strdup("-pipe");
        argv[i++] = strdup( "-" );
        argv[i++] = strdup( "-o" );
        argv[i++] = strdup(outfilename.c_str());
        if (!clang) {
            argv[i++] = strdup( "--param" );
            sprintf( buffer, "ggc-min-expand=%d", ggc_min_expand_heuristic( mem_limit ) );
            argv[i++] = strdup( buffer );
            argv[i++] = strdup( "--param" );
            sprintf( buffer, "ggc-min-heapsize=%d", ggc_min_heapsize_heuristic( mem_limit ) );
            argv[i++] = strdup( buffer );
        }
        if (clang)
            argv[i++] = strdup( "-no-canonical-prefixes" ); // otherwise clang tries to access /proc/self/exe
        // before you add new args, check above for argc
        argv[i] = 0;
        assert(i <= argc);

        close_debug();

        close( sock_out[0] );
        dup2 (sock_out[1], STDOUT_FILENO );
        close(sock_out[1]);

        close(sock_err[0]);
        dup2( sock_err[1], STDERR_FILENO );
        close(sock_err[1]);

        close( sock_in[1] );
        dup2( sock_in[0], STDIN_FILENO);
        close (sock_in[0]);

        close( main_sock[0] );
        fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);

        close( death_pipe[0] );
        close( death_pipe[1] );

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
    }
    close( sock_in[0] );
    close( sock_out[1] );
    close( sock_err[1] );

    // idea borrowed from kprocess.
    // check whether the compiler could be run at all.
    close( main_sock[1] );
    for(;;)
    {
        char resultByte;
        ssize_t n = ::read(main_sock[0], &resultByte, 1);
        if (n == -1 && errno == EINTR)
            continue; // Ignore

        if (n == 1)
        {
            rmsg.status = resultByte;

            error_client( client, "compiler did not start" );
            return EXIT_COMPILER_MISSING;
        }
        break; // != EINTR
    }
    close( main_sock[0] );

    struct timeval starttv;
    gettimeofday(&starttv, 0 );

    int return_value = 0;
    // Got EOF for preprocessed input. stdout send may be still pending.
    bool input_complete = false;
    // Pending data to send to stdin
    FileChunkMsg *fcmsg = 0;
    size_t off = 0;

    log_block parent_wait("parent, waiting");

    for(;;)
    {
        if ( client_fd >= 0 && !fcmsg ) {
            if (Msg *msg = client->get_msg(0)) {
                if (input_complete) {
                    rmsg.err.append( "client cancelled\n" );
                    return_value = EXIT_CLIENT_KILLED;
                    client_fd = -1;
                    kill(pid, SIGTERM);
                    delete fcmsg;
                    fcmsg = 0;
                    delete msg;
                } else {
                    if ( msg->type == M_END ) {
                        input_complete = true;
                        if (!fcmsg) {
                            close( sock_in[1] );
                            sock_in[1] = -1;
                        }
                        delete msg;
                    } else if ( msg->type == M_FILE_CHUNK ) {
                        fcmsg = static_cast<FileChunkMsg*>( msg );
                        off = 0;

                        job_stat[JobStatistics::in_uncompressed] += fcmsg->len;
                        job_stat[JobStatistics::in_compressed] += fcmsg->compressed;
                    } else {
                        log_error() << "protocol error while reading preprocessed file\n";
                        return_value = EXIT_IO_ERROR;
                        client_fd = -1;
                        kill(pid, SIGTERM);
                        delete fcmsg;
                        fcmsg = 0;
                        delete msg;
                    }
                }
            } else if (client->at_eof()) {
                log_error() << "unexpected EOF while reading preprocessed file\n";
                return_value = EXIT_IO_ERROR;
                client_fd = -1;
                kill(pid, SIGTERM);
                delete fcmsg;
                fcmsg = 0;
            }
        }

        fd_set rfds;
        FD_ZERO( &rfds );
        if (sock_out[0] >= 0)
            FD_SET( sock_out[0], &rfds );
        if (sock_err[0] >= 0)
            FD_SET( sock_err[0], &rfds );
        int max_fd = std::max( sock_out[0], sock_err[0] );

        if ( client_fd >= 0 && !fcmsg ) {
            FD_SET( client_fd, &rfds );
            if ( client_fd > max_fd )
                max_fd = client_fd;
            // Note that we don't actually query the status of this fd -
            // we poll it in every iteration.
        }

        FD_SET( death_pipe[0], &rfds );
        if ( death_pipe[0] > max_fd )
            max_fd = death_pipe[0];

        fd_set wfds, *wfdsp = 0;
        FD_ZERO( &wfds );
        if (fcmsg) {
            FD_SET( sock_in[1], &wfds );
            wfdsp = &wfds;
            if ( sock_in[1] > max_fd )
                max_fd = sock_in[1];
        }

        struct timeval tv, *tvp = 0;
        if (!input_complete) {
            tv.tv_sec = 60;
            tv.tv_usec = 0;
            tvp = &tv;
        }

        switch( select( max_fd+1, &rfds, wfdsp, 0, tvp ) )
        {
        case 0:
            if (!input_complete) {
                log_error() << "timeout while reading preprocessed file\n";
                kill(pid, SIGTERM); // Won't need it any more ...
                return_value = EXIT_IO_ERROR;
                client_fd = -1;
                input_complete = true;
                delete fcmsg;
                fcmsg = 0;
                continue;
            }
            // this should never happen
            assert( false );
            return EXIT_DISTCC_FAILED;
        case -1:
            if (errno == EINTR)
                continue;
            // this should never happen
            assert( false );
            return EXIT_DISTCC_FAILED;
        default:
            if ( fcmsg && FD_ISSET(sock_in[1], &wfds) ) {
                ssize_t bytes = write( sock_in[1], fcmsg->buffer + off, fcmsg->len - off );
                if ( bytes < 0 ) {
                    if (errno == EINTR)
                        continue;
                    kill(pid, SIGTERM); // Most likely crashed anyway ...
                    return_value = EXIT_COMPILER_CRASHED;
                    client_fd = -1;
                    input_complete = true;
                    delete fcmsg;
                    fcmsg = 0;
                    continue;
                }

                // The fd is -1 anyway
                //write(job_in_fd, fcmsg->buffer + off, bytes);

                off += bytes;

                if (off == fcmsg->len) {
                    delete fcmsg;
                    fcmsg = 0;
                    if (input_complete) {
                        close( sock_in[1] );
                        sock_in[1] = -1;
                    }
                }
            }

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

            if ( FD_ISSET(death_pipe[0], &rfds) ) {
                // Note that we have already read any remaining stdout/stderr:
                // the sigpipe is delivered after everything was written,
                // and the notification is multiplexed into the select above.

                struct rusage ru;
                int status;
                if (wait4(pid, &status, 0, &ru) != pid) {
                    // this should never happen
                    assert( false );
                    return EXIT_DISTCC_FAILED;
                }

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

                return return_value;
            }
        }
    }
}
