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

#include "comm.h"

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
    Only in this form, no prefix or suffix. It is usually a symlink to the real compiler,
    we will always detect it as !clang (FIXME?).
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

bool pollfd_is_set(const vector<pollfd>& pollfds, int fd, int flags, bool check_errors)
{
    for( size_t i = 0; i < pollfds.size(); ++i )
    {
        if( pollfds[ i ].fd == fd )
        {
            if( pollfds[ i ].revents & flags )
                return true;
            // Unlike with select(), where readfds gets set even on EOF, with poll()
            // POLLIN doesn't imply EOF and we need to check explicitly.
            if( check_errors && ( pollfds[ i ].revents & ( POLLERR | POLLHUP | POLLNVAL )))
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
