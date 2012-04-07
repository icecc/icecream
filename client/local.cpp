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
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef HAVE_SIGNAL_H
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

static string get_compiler_name( CompileJob::Language lang )
{
    string compiler_name = "gcc";

    const char* env;
    if ( (env = getenv( "ICECC_CC" )) )
        compiler_name = env;

    if (lang == CompileJob::Lang_CXX) {
        compiler_name = "g++";
        if ((env = getenv ("ICECC_CXX")))
            compiler_name = env;
    }

    return compiler_name;
}


static string path_lookup(const string& compiler)
{
    if ( compiler.at( 0 ) == '/' )
        return compiler;

    string path = ::getenv( "PATH" );
    string::size_type begin = 0;
    string::size_type end = 0;
    struct stat s;
    bool after_selflink = false;
    string best_match;

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
                if ( target == rs_program_name 
                     || (after_selflink
                         && (target == "tbcompiler" || target == "distcc"
                            || target == "colorgcc"))) {
                    // this is a link pointing to us, ignore it
                    after_selflink = true;
                    continue;
                }
            } else if ( !S_ISREG( s.st_mode ) ) {
                // It's not a link and not a file, so just ignore it. We don't
                // want to accidentially attempt to execute directories.
                continue;
            }
            best_match = part;

            if ( after_selflink )
                return part;
        }
    }
    if ( best_match.empty() ) 
    	log_error() << "couldn't find any " << compiler << endl;
    return best_match;
}

string find_compiler( const CompileJob& job )
{
    return path_lookup(job.compilerName());
}

string find_compiler( CompileJob::Language lang )
{
    string compiler = get_compiler_name( lang );

    return path_lookup(compiler);
}

static volatile int lock_fd = 0;
static volatile int user_break_signal = 0;
static volatile pid_t child_pid;

static void handle_user_break( int sig )
{
    if ( lock_fd )
        dcc_unlock( lock_fd );
    lock_fd = 0;
    user_break_signal = sig;

    if (child_pid != 0)
      kill ( sig, child_pid);

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
int build_local(CompileJob& job, MsgChannel *local_daemon, struct rusage *used)
{
    list<string> arguments;

    string compiler_name = find_compiler( job );

    trace() << "invoking: " << compiler_name << endl;

    if ( compiler_name.empty() ) {
        log_error() << "could not find " << job.compilerName() << " in PATH." << endl;
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

    bool color_output = job.language() != CompileJob::Lang_Custom
        && colorify_wanted();
    int pf[2];

    if (color_output && pipe(pf))
        color_output = false;

    if ( used || color_output ) {
        flush_debug();
        child_pid = fork();
    }

    if ( child_pid ) {
        if (color_output)
            close(pf[1]);

        // setup interrupt signals, so that the JobLocalBeginMsg will
        // have a matching JobLocalDoneMsg
        void (*old_sigint)(int) = signal( SIGINT, handle_user_break );
        void (*old_sigterm)(int) = signal( SIGTERM, handle_user_break );
        void (*old_sigquit)(int) = signal( SIGQUIT, handle_user_break );
        void (*old_sighup)(int) = signal( SIGHUP, handle_user_break );

        if (color_output) {
            string s_ccout;
            char buf[250];
            int r;
            for(;;) {
                while((r = read(pf[0], buf, sizeof(buf)-1)) > 0) {
                    buf[r] = '\0';
                    s_ccout.append(buf);
                }
                if (r == 0)
                    break;
                if ( r < 0 && errno != EINTR)
                    break;
            }
            colorify_output(s_ccout);
        }

        int status = 1;
        while( wait4( child_pid, &status, 0, used ) < 0 && errno == EINTR)
            ;

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

        if (color_output) {
            close(pf[0]);
            close(2);
            dup2(pf[1], 2);
        }

        int ret = execv( argv[0], argv );
        if ( lock_fd )
            dcc_unlock( lock_fd ); 
        if (ret) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ICECC[%d]: %s:", getpid(), argv[0]);
            log_perror(buf);
        }
        _exit ( ret );
    }
}
