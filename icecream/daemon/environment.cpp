/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

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

#include <config.h>
#include "environment.h"
#include <logging.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "comm.h"

using namespace std;

static string read_fromFILE( FILE *f )
{
    string output;
    if ( !f ) {
        log_error() << "no pipe " << strerror( errno ) << endl;
        return output;
    }
    char buffer[100];
    while ( !feof( f ) ) {
        size_t bytes = fread( buffer, 1, 99, f );
        buffer[bytes] = 0;
        output += buffer;
    }
    pclose( f );
    return output;
}

static bool extract_version( string &version )
{
    string::size_type pos = version.find_last_of( '\n' );
    if ( pos == string::npos )
        return false;

    while ( pos + 1 == version.size() ) {
        version.resize( version.size() - 1 );
        pos = version.find_last_of( '\n' );
        if ( pos == string::npos )
            return false;
    }

    version = version.substr( pos + 1);
    return true;
}

list<string> available_environmnents(const string &basedir, string &native_environments)
{
    list<string> envs;
    string gcc_version = read_fromFILE( popen( "/usr/bin/gcc -v 2>&1", "r" ) );
    if ( extract_version( gcc_version ) )
        envs.push_back( "gcc " + gcc_version );
    string gpp_version = read_fromFILE( popen( "/usr/bin/g++ -v 2>&1", "r" ) );
    if ( extract_version( gpp_version ) )
        envs.push_back( "g++ " + gpp_version );

    DIR *envdir = opendir( basedir.c_str() );
    if ( !envdir ) {
        log_info() << "can't open envs dir " << strerror( errno ) << endl;
    } else {
        struct dirent *ent = readdir(envdir);
        while ( ent ) {
            string dirname = ent->d_name;
            if ( dirname.at( 0 ) != '.' )
            {
                if ( !access( string( basedir + "/" + dirname + "/usr/bin/gcc" ).c_str(), X_OK ) )
                     envs.push_back( dirname );
            }
            ent = readdir( envdir );
        }
        closedir( envdir );
    }

    return envs;
}


bool install_environment( const std::string &basename, const std::string &name, MsgChannel *c )
{
    if ( !name.size() || name[0] == '.' ) {
        log_error() << "illegal name for environment " << name << endl;
        return false;
    }

    for ( string::size_type i = 0; i < name.size(); ++i ) {
        if ( isascii( name[i] ) && !isspace( name[i]) && name[i] != '/' && isprint( name[i] ) )
            continue;
        log_error() << "illegal char '" << name[i] << "' - rejecting environment " << name << endl;
        return false;
    }

    if ( mkdir( basename.c_str(), 0755 ) && errno != EEXIST ) {
        perror( "mkdir" );
        return false;
    }

    string dirname = basename + "/" + name;
    if ( mkdir( dirname.c_str(), 0755 ) ) {
        perror( "mkdir" );
        return false;
    }

    char pwd[PATH_MAX];
    getcwd(pwd, PATH_MAX);
    if ( chdir( dirname.c_str() ) ) {
        perror( "chdir" );
        return false;
    }

    trace() << "set it up, opening tar\n";

    FILE *pipe = popen( "tar xjf -", "w" );
    if ( !pipe ) {
        perror( "popen tar" );
        return false; // TODO: rm?
    }

    bool error = false;
    Msg *msg = 0;
    do {
        delete msg;
        msg = c->get_msg();
        if ( msg->type == M_END ) {
            trace() << "end\n";
            break;
        }
        FileChunkMsg *fmsg = dynamic_cast<FileChunkMsg*>( msg );
        if ( !fmsg ) {
            log_error() << "Expected another file chunk\n";
            error = true;
            break;
        }
        trace() << "got env share: " << fmsg->len << endl;
        int ret = fwrite( fmsg->buffer, fmsg->len, 1, pipe );
        if ( ret != 1 ) {
            log_error() << "wrote " << ret << " bytes\n";
            error = true;
            break;
        }
    } while ( true );

    delete msg;
    chdir( pwd );
    pclose( pipe );
    if ( error ) {
        char buffer[PATH_MAX];
        sprintf( buffer, "rm -rf '/%s'", dirname.c_str() );
        system( buffer );
        return false;
    } else {
        mkdir( ( dirname + "/var/tmp" ).c_str(), 0755 );
        mkdir( ( dirname + "/tmp" ).c_str(), 0755 );
    }
    return true;
}
