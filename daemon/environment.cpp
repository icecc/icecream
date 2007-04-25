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
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <signal.h>
#endif

#include "comm.h"

using namespace std;

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

size_t sumup_dir( const string &dir )
{
    size_t res = 0;
    DIR *envdir = opendir( dir.c_str() );
    if ( !envdir )
        return res;

    struct stat st;
    string tdir = dir + "/";

    for ( struct dirent *ent = readdir(envdir); ent; ent = readdir( envdir ) )
    {
        if ( !strcmp( ent->d_name, "." ) || !strcmp( ent->d_name, ".." ) )
            continue;

        if ( lstat( ( tdir + ent->d_name ).c_str(), &st ) ) {
            perror( "stat" );
            continue;
        }

        if ( S_ISDIR( st.st_mode ) )
            res += sumup_dir( tdir + ent->d_name );
        else if ( S_ISREG( st.st_mode ) )
            res += st.st_size;
        // else ignore
    }
    closedir( envdir );
    return res;
}

static string list_native_environment( const string &nativedir )
{
    assert( nativedir.at( nativedir.length() - 1 ) == '/' );

    string native_environment;

    DIR *tdir = opendir( nativedir.c_str() );
    if ( tdir ) {
        string suff = ".tar.gz";
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
    flush_debug();
    pid_t pid = fork();
    if ( pid )
    {
        int status = 0;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
            ;

        if ( mkdir( basedir.c_str(), 0755 ) && errno != EEXIST ) {
            if ( errno == EPERM )
                log_error() << "permission denied on mkdir " << basedir << endl;
            else
                log_perror( "mkdir in cleanup_cache() failed" );
            return false;
        }
        return WIFEXITED(status);
    }
    // else
    char **argv;
    argv = new char*[5];
    argv[0] = strdup( "/bin/rm" );
    argv[1] = strdup( "-rf" );
    argv[2] = strdup( "--" );
    // make sure it ends with '/' to not fall into symlink traps
    string bdir = basedir + '/';
    argv[3] = strdup( bdir.c_str()  );
    argv[4] = NULL;

    _exit(execv(argv[0], argv));
}

Environments available_environmnents(const string &basedir)
{
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

size_t setup_env_cache(const string &basedir, string &native_environment, uid_t nobody_uid, gid_t nobody_gid)
{
    native_environment = "";
    string nativedir = basedir + "/native/";

    if ( ::access( "/usr/bin/gcc", X_OK ) || ::access( "/usr/bin/g++", X_OK ) )
	return 0;

    if ( mkdir( nativedir.c_str(), 0755 ) )
   	return 0;

    if ( chown( nativedir.c_str(), nobody_uid, nobody_gid) ) {
	rmdir( nativedir.c_str() );
	return 0;
    }

    flush_debug();
    pid_t pid = fork();
    if ( pid ) {
        int status = 1;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
           ;
        trace() << "waitpid " << status << endl;
        if ( !status ) {
            trace() << "opendir " << nativedir << endl;
            native_environment = list_native_environment( nativedir );
            if ( native_environment.empty() )
                status = 1;
        }
        trace() << "native_environment " << native_environment << endl;
        if ( status ) {
            rmdir( nativedir.c_str() );
            return 0;
        }
        else {
            return sumup_dir( nativedir );
        }
    }
    // else
    if ( setgid( nobody_gid ) < 0) {
      log_perror("setgid failed");
      _exit(143);
    }
    if (!geteuid() && setuid( nobody_uid ) < 0) {
      log_perror("setuid failed");
      _exit (142);
    }

    if ( chdir( nativedir.c_str() ) ) {
         log_perror( "chdir" );
         _exit(1);
    }

    if ( system( BINDIR "/icecc --build-native" ) ) {
        log_error() << BINDIR "/icecc --build-native failed\n";
        _exit(1);
    }
    _exit( 0 );
}

size_t install_environment( const std::string &basename, const std::string &target,
                          const std::string &name, MsgChannel *c, uid_t nobody_uid, gid_t nobody_gid )
{
    if ( !name.size() || name[0] == '.' ) {
        log_error() << "illegal name for environment " << name << endl;
        return 0;
    }

    for ( string::size_type i = 0; i < name.size(); ++i ) {
        if ( isascii( name[i] ) && !isspace( name[i]) && name[i] != '/' && isprint( name[i] ) )
            continue;
        log_error() << "illegal char '" << name[i] << "' - rejecting environment " << name << endl;
        return 0;
    }

    string dirname = basename + "/target=" + target;
    Msg *msg = c->get_msg(30);
    if ( !msg || msg->type != M_FILE_CHUNK )
    {
        trace() << "Expected first file chunk\n";
        return 0;
    }

    FileChunkMsg *fmsg = dynamic_cast<FileChunkMsg*>( msg );
    enum { BZip2, Gzip, None} compression = None;
    if ( fmsg->len > 2 )
    {
        if ( fmsg->buffer[0] == 037 && fmsg->buffer[1] == 0213 )
            compression = Gzip;
        else if ( fmsg->buffer[0] == 'B' && fmsg->buffer[1] == 'Z' )
            compression = BZip2;
    }

    if( ::access( basename.c_str(), W_OK ) ) {
       log_error() << "access for basename " <<  basename.c_str() << " gives " << strerror(errno) << endl;
       return 0;
    }

    if ( mkdir( dirname.c_str(), 0755 ) && errno != EEXIST ) {
        log_perror( "mkdir target" );
        return 0;
    }

    dirname = dirname + "/" + name;
    if ( mkdir( dirname.c_str(), 0700 ) ) {
        log_perror( "mkdir name" );
        return 0;
    }

    chown( dirname.c_str(), 0, nobody_gid );
    chmod( dirname.c_str(), 0770 );

    int fds[2];
    if ( pipe( fds ) )
        return 0;

    flush_debug();
    pid_t pid = fork();
    if ( pid )
    {
        trace() << "pid " << pid << endl;
        close( fds[0] );
        FILE *fpipe = fdopen( fds[1], "w" );

        bool error = false;
        do {
            int ret = fwrite( fmsg->buffer, fmsg->len, 1, fpipe );
            if ( ret != 1 ) {
                log_error() << "wrote " << ret << " bytes\n";
                error = true;
                break;
            }
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
            fmsg = dynamic_cast<FileChunkMsg*>( msg );

            if ( !fmsg ) {
                log_error() << "Expected another file chunk\n";
                error = true;
                break;
            }
        } while ( true );

        delete msg;

        fclose( fpipe );
        close( fds[1] );

        int status = 1;
        while ( waitpid( pid, &status, 0) < 0 && errno == EINTR)
            ;

        error |= WEXITSTATUS(status);

        if ( error ) {
            kill( pid, SIGTERM );
            char buffer[PATH_MAX];
            snprintf( buffer, PATH_MAX, "rm -rf '/%s'", dirname.c_str() );
            system( buffer );
            return 0;
        } else {
            mkdir( ( dirname + "/tmp" ).c_str(), 01775 );
            chown( ( dirname + "/tmp" ).c_str(), 0, nobody_gid );
            chmod( ( dirname + "/tmp" ).c_str(), 01775 );
            return sumup_dir( dirname );
        }
    }
    // else
    if ( setgid( nobody_gid ) < 0) {
      log_perror("setgid fails");
      _exit(143);
    }
    if (!geteuid() && setuid( nobody_uid ) < 0) {
      log_perror("setuid fails");
      _exit (142);
    }

    // reset SIGPIPE and SIGCHILD handler so that tar
    // isn't confused when gzip/bzip2 aborts
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    close( 0 );
    close( fds[1] );
    dup2( fds[0], 0 );

    char **argv;
    argv = new char*[6];
    argv[0] = strdup( "/bin/tar" );
    argv[1] = strdup ("-C");
    argv[2] = strdup ( dirname.c_str() );
    if ( compression == BZip2 )
        argv[3] = strdup( "-xjf" );
    else if ( compression == Gzip )
        argv[3] = strdup( "-xzf" );
    else if ( compression == None )
        argv[3] = strdup( "-xf" );
    argv[4] = strdup( "-" );
    argv[5] = 0;
    _exit( execv( argv[0], argv ) );
}

size_t finalize_install_environment( const std::string &basename, const std::string &target,
                                     pid_t pid, gid_t nobody_gid)
{
    int status = 1;
    while ( waitpid( pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        log_error() << "exit code: " << WEXITSTATUS(status) << endl;
        remove_environment(basename, target);
        return 0;
    }

    string dirname = basename + "/target=" + target;
    mkdir( ( dirname + "/tmp" ).c_str(), 01775 );
    chown( ( dirname + "/tmp" ).c_str(), 0, nobody_gid );
    chmod( ( dirname + "/tmp" ).c_str(), 01775 );

    return sumup_dir (dirname);
}

size_t remove_environment( const string &basename, const string &env )
{
    string::size_type pos = env.find_first_of( '/' );
    if ( pos == string::npos ) // nonsense
        return 0;

    string target = env.substr( 0, pos );
    string name = env.substr( pos + 1 );
    string dirname = basename + "/target=" + target + "/" + name;

    size_t res = sumup_dir( dirname );

    flush_debug();
    pid_t pid = fork();
    if ( pid )
    {
        int status = 0;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
            ;
         if ( WIFEXITED (status) )
             return res;
        // something went wrong. assume no disk space was free'd.
        return 0;
    }
    // else

    char **argv;
    argv = new char*[5];
    argv[0] = strdup( "/bin/rm" );
    argv[1] = strdup( "-rf" );
    argv[2] = strdup("--");
    argv[3] = strdup( dirname.c_str() );
    argv[4] = NULL;

    _exit(execv(argv[0], argv));
}
