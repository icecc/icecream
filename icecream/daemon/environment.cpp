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

extern bool maybe_stats();

#if 0
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
#endif

static string list_native_environment( const string &nativedir )
{
    assert( nativedir.at( nativedir.length() - 1 ) == '/' );

    string native_environment;

    DIR *tdir = opendir( nativedir.c_str() );
    if ( tdir ) {
        string suff = ".tar.bz2";
        do {
            struct dirent *myenv = readdir(tdir);
            if ( !myenv )
                break;
            string versfile = myenv->d_name;
            if ( versfile.size() > suff.size() && versfile.substr( versfile.size() - suff.size() ) == suff ) {
                native_environment = nativedir + versfile;
                break;
            }
        } while ( true );
        closedir( tdir );
    }
    return native_environment;
}

static void list_target_dirs( const string &current_target, const string &targetdir, Environments &envs )
{
    DIR *envdir = opendir( targetdir.c_str() );
    if ( !envdir )
        return;

    for ( struct dirent *ent = readdir(envdir); ent; ent = readdir( envdir ) )
    {
        string dirname = ent->d_name;
        if ( !access( string( targetdir + "/" + dirname + "/usr/bin/gcc" ).c_str(), X_OK ) )
            envs.push_back( make_pair( current_target, dirname ) );
    }
    closedir( envdir );
}

bool cleanup_cache( const string &basedir )
{
    if ( !::access( basedir.c_str(), W_OK ) ) { // it exists - removing
        if ( ( basedir.substr( 0, 5 ) != "/tmp/" && basedir.substr( 0, 9 ) != "/var/tmp/" ) ||
             basedir.find_first_of( "'\"" ) != string::npos )
        {
            log_error() << "the cache directory for icecream isn't below /tmp or /var/tmp and already exists - I won't remove it!\n";
            return false;
        }
        char buffer[PATH_MAX];
        snprintf( buffer, PATH_MAX, "rm -rf '%s'", basedir.c_str() );
        if ( system( buffer ) ) {
            perror( "rm -rf failed" );
            return false;
        }
    }

    if ( mkdir( basedir.c_str(), 0755 ) ) {
        if ( errno == EPERM )
            log_error() << "cache directory can't be generated: " << basedir << endl;
        else if ( errno == EEXIST )
            log_error() << "cache directory already exists and can't be removed: " << basedir << endl;
        else
            perror( "Failed " );
        return false;
    }

    return true;
}

Environments available_environmnents(const string &basedir)
{
    assert( !::access( basedir.c_str(), W_OK ) );

    Environments envs;

    DIR *envdir = opendir( basedir.c_str() );
    if ( !envdir ) {
        log_info() << "can't open envs dir " << strerror( errno ) << endl;
    } else {
        for ( struct dirent *target_ent = readdir(envdir); target_ent; target_ent = readdir( envdir ) )
        {
            string dirname = target_ent->d_name;
            if ( dirname.at( 0 ) == '.' )
                continue;
            if ( dirname.substr( 0, 7 ) == "target=" )
            {
                string current_target = dirname.substr( 7, dirname.length() - 7 );
                list_target_dirs( current_target, basedir + "/" + dirname, envs );
            }
        }
        closedir( envdir );
    }

    return envs;
}

bool setup_env_cache(const string &basedir, string &native_environment)
{
    native_environment = "";

    if ( !::access( "/usr/bin/gcc", X_OK ) && !::access( "/usr/bin/g++", X_OK ) ) {
        string nativedir = basedir + "/native/";
        if ( !mkdir ( nativedir.c_str(), 0755 ) )
        {
            char pwd[PATH_MAX];
            getcwd(pwd, PATH_MAX);
            if ( chdir( nativedir.c_str() ) ) {
                perror( "chdir" );
                return false;
            }

            if ( system( "create-env" ) ) {
                log_error() << "create-env failed\n";
            } else {
                native_environment = list_native_environment( nativedir );
            }

            if ( native_environment.empty() )
                if ( !rmdir( nativedir.c_str() ) ) {
                    perror( "rmdir nativedir" );
                }

            chdir( pwd );
        }
    }

    return true;
}

bool install_environment( const std::string &basename, const std::string &target, const std::string &name, MsgChannel *c )
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

    assert( !::access( basename.c_str(), W_OK ) );

    string dirname = basename + "/target=" + target;
    if ( mkdir( dirname.c_str(), 0755 ) && errno != EEXIST ) {
        perror( "mkdir target" );
        return false;
    }

    dirname = dirname + "/" + name;
    if ( mkdir( dirname.c_str(), 0755 ) ) {
        perror( "mkdir name" );
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
        msg = c->get_msg(30);
	if (!msg) {
            error = true;
	    break;
	}

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
        maybe_stats();
        trace() << "got env share: " << fmsg->len << endl;
        int ret = fwrite( fmsg->buffer, fmsg->len, 1, pipe );
        if ( ret != 1 ) {
            log_error() << "wrote " << ret << " bytes\n";
            error = true;
            break;
        }
    } while ( true );

    maybe_stats();

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
