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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef __FreeBSD__
#include <signal.h>
#endif

#include <comm.h>
#include "client.h"

using namespace std;

extern const char * rs_program_name;

#define CLIENT_DEBUG 0

/*
 * Get the name of the compiler depedant on the
 * language of the job and the environment
 * variable set. This is useful for native cross-compilers.
 * (arm-linux-gcc for example)
 */
string get_compiler_name( const CompileJob &job ) {
    string compiler_name = "gcc";

    if ( getenv( "ICECC_CC" ) != 0 )
        compiler_name = getenv( "ICECC_CC" );

    if ( job.language() == CompileJob::Lang_CXX )
        compiler_name = getenv( "ICECC_CXX" ) != 0 ?
                        getenv( "ICECC_CXX" ) : "g++";

    return compiler_name;
}

string find_compiler( const string &compiler )
{
    if ( compiler.at( 0 ) == '/' )
        return compiler;

    string path = ::getenv( "PATH" );
    string::size_type begin = 0;
    string::size_type end = 0;
    struct stat s;

    while ( end != string::npos ) {
        end = path.find_first_of( ':', begin );
        string part;
        if ( end == string::npos )
            part = path.substr( begin );
        else
            part = path.substr( begin, end - begin );
        begin = end + 1;

        part = part + '/' + compiler;
        if ( !lstat( part.c_str(), &s ) ) {
            if ( S_ISLNK( s.st_mode ) ) {
                char buffer[PATH_MAX];
                int ret = readlink( part.c_str(), buffer, PATH_MAX );
                if ( ret == -1 ) {
                    log_error() << "readlink failed " << strerror( errno ) << endl;
                    continue;
                }
                buffer[ret] = 0;
                string target = find_basename( buffer );
                if ( target == rs_program_name || target == "tbcompiler" || target == "distcc" || target == "colorgcc" ) {
                    // this is a link pointing to us, ignore it
                    continue;
                }
            }
            return part;
        }
    }
    log_error() << "couldn't find any " << compiler << endl;
    return string();
}

static volatile int lock_fd = 0;
static volatile int user_break_signal = 0;
void handle_user_break( int sig )
{
    if ( lock_fd )
        dcc_unlock( lock_fd );
    lock_fd = 0;
    user_break_signal = sig;
    signal( sig, handle_user_break );
}

/**
 * Invoke a compiler locally.  This is, obviously, the alternative to
 * dcc_compile_remote().
 *
 * The server does basically the same thing, but it doesn't call this
 * routine because it wants to overlap execution of the compiler with
 * copying the input from the network.
 *
 * This routine used to exec() the compiler in place of distcc.  That
 * is slightly more efficient, because it avoids the need to create,
 * schedule, etc another process.  The problem is that in that case we
 * can't clean up our temporary files, and (not so important) we can't
 * log our resource usage.
 *
 **/
int build_local(CompileJob &job, MsgChannel *local_daemon, struct rusage *used)
{
    list<string> arguments;

    string compiler_name = get_compiler_name( job );
    trace() << "build_local " << local_daemon << " compiler: " << compiler_name <<  endl;
    compiler_name = find_compiler( compiler_name );

    if ( compiler_name.empty() ) {
        log_error() << "could not find " << get_compiler_name (job ) << " in PATH." << endl;
        return EXIT_NO_SUCH_FILE;
    }

    arguments.push_back( compiler_name );
    appendList( arguments, job.allFlags() );

    if ( !job.inputFile().empty() )
        arguments.push_back( job.inputFile() );
    if ( !job.outputFile().empty() ) {
        arguments.push_back( "-o" );
        arguments.push_back( job.outputFile() );
    }
    char **argv = new char*[arguments.size() + 1];
    int argc = 0;
    for ( list<string>::const_iterator it = arguments.begin();
          it != arguments.end(); ++it )
        argv[argc++] = strdup( it->c_str() );
    argv[argc] = 0;
#if CLIENT_DEBUG
    trace() << "execing ";
    for ( int i = 0; argv[i]; i++ )
        trace() << argv[i] << " ";
    trace() << endl;
#endif

    if ( !local_daemon ) {
        int fd;
        if ( !dcc_lock_host(fd ) ) {
            log_error() << "can't lock for local job" << endl;
            return EXIT_DISTCC_FAILED;
        }
        lock_fd = fd;
    }

    pid_t child = 0;

    if ( used )
        child = fork();

    if ( child ) {
        // setup interrupt signals, so that the JobLocalBeginMsg will
        // have a matching JobLocalDoneMsg
        void (*old_sigint)(int) = signal( SIGINT, handle_user_break );
        void (*old_sigterm)(int) = signal( SIGTERM, handle_user_break );
        void (*old_sigquit)(int) = signal( SIGQUIT, handle_user_break );
        void (*old_sighup)(int) = signal( SIGHUP, handle_user_break );

        int status = 1;
        if( wait4( child, &status, 0, used ) > 0 )
            status = WEXITSTATUS(status);

        signal( SIGINT, old_sigint );
        signal( SIGTERM, old_sigterm );
        signal( SIGQUIT, old_sigquit );
        signal( SIGHUP, old_sighup );
        if( user_break_signal )
            raise( user_break_signal );
        if ( lock_fd )
            dcc_unlock( lock_fd );
        return status;
    } else {
        dcc_increment_safeguard();

    int ret = execv( argv[0], argv );
        if ( used )
            _exit ( errno );
        if ( lock_fd )
            dcc_unlock( lock_fd ); 
        if (ret) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ICECREAM: %s:", argv[0]);
            log_perror(buf);
        }
        return ret;
    }
}
