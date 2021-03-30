/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "util.h"

#include <cassert>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>

#include "comm.h"
#include "logging.h"
#include "exitcode.h"

using namespace std;

/**
 * Return a pointer to the basename of the file (everything after the
 * last slash.)  If there is no slash, return the whole filename,
 * which is presumably in the current directory.
 **/
string find_basename(const string &sfile)
{
    size_t index = sfile.rfind('/');

    if (index == string::npos) {
        return sfile;
    }

    return sfile.substr(index + 1);
}

string find_prefix(const string &basename)
{
    size_t index = basename.rfind('-');

    if (index == string::npos) {
        return "";
    }

    return basename.substr(0, index);
}

/*
 We support these compilers:
 - cc/c++
    Only in this form, no prefix or suffix. It is usually a symlink to the real compiler
 - gcc/g++
    Including a possible prefix or postfix separated by '-' (e.g. aarch64-suse-linux-gcc-6)
 - clang/clang++
    Including a possible prefix or postfix separated by '-' (e.g. clang-8).
*/

bool is_c_compiler(const string& compiler)
{
    string name = find_basename(compiler);
    if( name.find("++") != string::npos )
        return false;
    return name.find("gcc") != string::npos || name.find("clang") != string::npos
        || name == "cc";
}

bool is_cpp_compiler(const string& compiler)
{
    string name = find_basename(compiler);
    return name.find("g++") != string::npos || name.find("clang++") != string::npos
        || name == "c++";
}

string get_c_compiler(const string& compiler)
{
    if(compiler.empty())
        return compiler;
    size_t slash = compiler.rfind('/');
    size_t pos = compiler.rfind( "++" );
    if( pos == string::npos || pos < slash )
        return compiler;
    pos = compiler.rfind( "clang++" );
    if( pos != string::npos && pos >= slash + 1 )
        return compiler.substr( 0, pos ) + "clang" + compiler.substr( pos + strlen( "clang++" ));
    pos = compiler.rfind( "g++" ); // g++ must go after clang++, it's a substring
    if( pos != string::npos && pos >= slash + 1 )
        return compiler.substr( 0, pos ) + "gcc" + compiler.substr( pos + strlen( "g++" ));
    pos = compiler.rfind( "c++" );
    if( pos != string::npos && pos == slash + 1 ) // only exactly "c++"
        return compiler.substr( 0, pos ) + "cc" + compiler.substr( pos + strlen( "c++" ));
    assert( false );
    return string();
}

string get_cpp_compiler(const string& compiler)
{
    if(compiler.empty())
        return compiler;
    size_t slash = compiler.rfind('/');
    size_t pos = compiler.rfind( "++" );
    if( pos != string::npos && pos >= slash + 1 )
        return compiler;
    pos = compiler.rfind( "gcc" );
    if( pos != string::npos && pos >= slash + 1 )
        return compiler.substr( 0, pos ) + "g++" + compiler.substr( pos + strlen( "gcc" ));
    pos = compiler.rfind( "clang" );
    if( pos != string::npos && pos >= slash + 1 )
        return compiler.substr( 0, pos ) + "clang++" + compiler.substr( pos + strlen( "clang" ));
    pos = compiler.rfind( "cc" );
    if( pos != string::npos && pos == slash + 1 ) // only exactly "cc"
        return compiler.substr( 0, pos ) + "c++" + compiler.substr( pos + strlen( "cc" ));
    assert( false );
    return string();
}

string read_command_output(const string& command, const vector<string>& args, int output_fd)
{
    flush_debug();
    int pipes[2];
    if (pipe(pipes) == -1) {
        log_error() << "failed to create pipe: " << strerror(errno) << endl;
        _exit(147);
    }
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork");
        _exit(147);
    }

    if (pid) { // parent
        if (close(pipes[1]) < 0){
            log_perror("close failed");
        }
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        if(shell_exit_status(status) != 0)
            return string();
        string output;
        char buf[1024];
        for (;;) {
            int r = read(pipes[0], buf, sizeof(buf) - 1 );
            if( r > 0 ) {
                buf[r] = '\0';
                output += buf;
            }
            if (r == 0)
                break;
            if (r < 0 && errno != EINTR)
                break;
        }
        return output;
    }

    // child

    if (close(pipes[0]) < 0){
        log_perror("close failed");
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    if (dup2(pipes[1], output_fd) < 0){
        log_perror("dup2 failed");
        return string();
    }

    if (close(pipes[1]) < 0){
        log_perror("close failed");
    }

    const char **argv;
    argv = new const char*[args.size() + 2];
    int pos = 0;
    argv[pos++] = strdup(command.c_str());
    for (const string& arg : args)
        argv[pos++] = strdup(arg.c_str());
    argv[pos++] = nullptr;

    execvp(argv[0], const_cast<char * const *>(argv));
    ostringstream errmsg;
    errmsg << "execv " << argv[0] << " failed";
    log_perror(errmsg.str());
    _exit(-1);
}

string read_command_line(const string& command, const vector<string>& args, int output_fd)
{
    string output = read_command_output( command, args, output_fd );
    // get rid of the endline
    if( output[ output.length() - 1 ] == '\n' )
        return output.substr(0, output.length() - 1);
    else
        return output;
}

bool pollfd_is_set(const vector<pollfd>& pollfds, int fd, int flags, bool check_errors)
{
    for(auto pollfd : pollfds)
    {
        if( pollfd.fd == fd )
        {
            if( pollfd.revents & flags )
                return true;
            // Unlike with select(), where readfds gets set even on EOF, with poll()
            // POLLIN doesn't imply EOF and we need to check explicitly.
            if( check_errors && ( pollfd.revents & ( POLLERR | POLLHUP | POLLNVAL )))
                return true;
            return false;
        }
    }
    return false;
}

string supported_features_to_string(unsigned int features)
{
    string ret;
    if( features & NODE_FEATURE_ENV_XZ )
        ret += " env_xz";
    if( features & NODE_FEATURE_ENV_ZSTD )
        ret += " env_zstd";
    if( ret.empty())
        ret = "--";
    else
        ret.erase( 0, 1 ); // remove leading " "
    return ret;
}
