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

using namespace std;

// code based on gcc - Copyright (C) 1999, 2000, 2001, 2002 Free Software Foundation, Inc.

/* Heuristic to set a default for GGC_MIN_EXPAND.  */
int
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
unsigned int
ggc_min_heapsize_heuristic(unsigned int mem_limit)
{
    /* The heuristic is RAM/8, with a lower bound of 4M and an upper
       bound of 128M (when RAM >= 1GB).  */
    mem_limit /= 8;
    mem_limit = std::max (mem_limit, 4U);
    mem_limit = std::min (mem_limit, 128U);

    return mem_limit * 1024;
}


volatile bool must_reap = false;

void theSigCHLDHandler( int )
{
    must_reap = true;
}

int work_it( CompileJob &j,
             const string& infilename,
             string &str_out, string &str_err,
             int &status, string &outfilename, unsigned long int mem_limit, int client_fd )
{
    str_out.erase(str_out.begin(), str_out.end());
    str_out.erase(str_out.begin(), str_out.end());

    std::list<string> list = j.remoteFlags();
    appendList( list, j.restFlags() );
    int ret;

    char tmp_output[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", ".o", tmp_output, 1 ) ) != 0 )
        return ret;

    outfilename = tmp_output;

    int sock_err[2];
    int sock_out[2];
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

    must_reap = false;

    /* Testing */
    struct sigaction act;
    sigemptyset( &act.sa_mask );

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigaction( SIGPIPE, &act, 0L );

    act.sa_handler = theSigCHLDHandler;
    act.sa_flags = SA_NOCLDSTOP | SA_RESTART;
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
        unlink( tmp_output );
        return EXIT_OUT_OF_MEMORY;
    } else if ( pid == 0 ) {

        close( main_sock[0] );
        fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);
        setenv( "PATH", "usr/bin", 1 );
	if (getuid() == 0)
            setenv( "LD_LIBRARY_PATH", "usr/lib:lib", 1 );

#ifdef RLIMIT_AS
        struct rlimit rlim;
        if ( getrlimit( RLIMIT_AS, &rlim ) )
            log_perror( "getrlimit" );

        rlim.rlim_cur = mem_limit*1024*1024;
        rlim.rlim_max = mem_limit*1024*1024;
        if ( setrlimit( RLIMIT_AS, &rlim ) )
            log_perror( "setrlimit" );
#endif

        int argc = list.size();
        argc++; // the program
        argc += 3; // file.i -o file.o
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
        argv[i++] = strdup( infilename.c_str() );
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

        close( STDOUT_FILENO );
        close( sock_out[0] );
        dup2( sock_out[1], STDOUT_FILENO );
        close( STDERR_FILENO );
        close( sock_err[0] );
        dup2( sock_err[1], STDERR_FILENO );

        ret = execvp( argv[0], const_cast<char *const*>( argv ) ); // no return
        printf( "all failed\n" );

        char resultByte = 1;
        write(main_sock[1], &resultByte, 1);
        exit(-1);
    } else {
        close( main_sock[1] );

        // idea borrowed from kprocess
        for(;;)
        {
            char resultByte;
            ssize_t n = ::read(main_sock[0], &resultByte, 1);
            if (n == 1)
            {
                status = resultByte;
                // exec() failed
                close(main_sock[0]);
                close( sock_err[0] );
                close( sock_err[1] );
                close( sock_out[0] );
                close( sock_out[1] );

                waitpid(pid, 0, 0);
                unlink( tmp_output );
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

        for(;;)
        {
            fd_set rfds;
            FD_ZERO( &rfds );
            FD_SET( sock_out[0], &rfds );
            FD_SET( sock_err[0], &rfds );
            FD_SET( client_fd, &rfds );

            int max_fd = std::max( sock_out[0], sock_err[0] );
            if ( client_fd > max_fd )
                max_fd = client_fd;

            struct timeval tv;
            /* Wait up to five seconds. */
            tv.tv_sec = 5;
            tv.tv_usec = 0;

            ret =  select( max_fd+1, &rfds, 0, 0, &tv );
            switch( ret )
            {
            case -1:
		if ( errno != EINTR ) // this usually means the logic broke
                    return EXIT_DISTCC_FAILED;
                // fall through; should happen if tvp->tv_sec < 0
            case 0:
                struct rusage ru;
                if (wait4(pid, &status, must_reap ? WUNTRACED : WNOHANG, &ru) != 0) // error finishes, too
                {
                    close( sock_err[0] );
                    close( sock_err[1] );
                    close( sock_out[0] );
                    close( sock_out[1] );
                    if ( WIFEXITED( status ) )
                        status = WEXITSTATUS( status );
                    else
                        status = 1;

                    if ( status ) {
                        unsigned long int mem_used = ( ru.ru_minflt + ru.ru_majflt ) * getpagesize() / 1024;
                        if ( mem_used * 100 > 85 * mem_limit * 1024 ||
                             str_err.find( "virtual memory exhausted: Cannot allocate memory" ) != string::npos )
                        {
                            // the relation between ulimit and memory used is pretty thin ;(
                            return EXIT_OUT_OF_MEMORY;
                        }
                    }

                    return 0;
                }
                break;
            default:
                if ( FD_ISSET(sock_out[0], &rfds) ) {
                    ssize_t bytes = read( sock_out[0], buffer, 4096 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        str_out.append( buffer );
                    }
                }
                if ( FD_ISSET(sock_err[0], &rfds) ) {
                    ssize_t bytes = read( sock_err[0], buffer, 4096 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        str_err.append( buffer );
                    }
                }
                if ( FD_ISSET( client_fd, &rfds ) ) {
                    str_err.append( "client cancelled\n" );
                    close( client_fd );
                    kill( pid, SIGTERM );
                    return EXIT_CLIENT_KILLED;
                }
            }
        }
    }
    assert( false );
    return 0;
}
