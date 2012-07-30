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
#ifdef HAVE_SIGNAL_H
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

static void list_target_dirs( const string &current_target, const string &targetdir, Environments &envs )
{
    DIR *envdir = opendir( targetdir.c_str() );
    if ( !envdir )
        return;

    for ( struct dirent *ent = readdir(envdir); ent; ent = readdir( envdir ) )
    {
        string dirname = ent->d_name;
        if ( !access( string( targetdir + "/" + dirname + "/usr/bin/as" ).c_str(), X_OK ) )
            envs.push_back( make_pair( current_target, dirname ) );
    }
    closedir( envdir );
}

/* Returns true if the child exited with success */
static bool exec_and_wait( const char *const argv[] )
{
    pid_t pid = fork();
    if ( pid == -1 ) {
        log_perror("fork");
        return false;
    }
    if ( pid ) {
        // parent
        int status;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
            ;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    // child
    _exit(execv(argv[0], const_cast<char *const *>(argv)));
}

bool cleanup_cache( const string &basedir )
{
    flush_debug();

    // make sure it ends with '/' to not fall into symlink traps
    string bdir = basedir + '/';
    const char *const argv[] = {
        "/bin/rm", "-rf", "--", bdir.c_str(), NULL
    };

    bool ret = exec_and_wait( argv );

    if ( mkdir( basedir.c_str(), 0755 ) && errno != EEXIST ) {
        if ( errno == EPERM )
            log_error() << "permission denied on mkdir " << basedir << endl;
        else
            log_perror( "mkdir in cleanup_cache() failed" );
        return false;
    }

    return ret;
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

// Timestamps for compiler binaries, if they have changed since the time
// native env was built, it needs to be rebuilt.
static time_t gcc_bin_timestamp = 0;
static time_t gpp_bin_timestamp = 0;
static time_t clang_bin_timestamp = 0;

static void save_native_env_timestamp()
{
    struct stat st;
    if( stat( "/usr/bin/gcc", &st ) == 0 )
        gcc_bin_timestamp = st.st_mtime;
    else
        gcc_bin_timestamp = 0;
    if( stat( "/usr/bin/g++", &st ) == 0 )
        gpp_bin_timestamp = st.st_mtime;
    else
        gpp_bin_timestamp = 0;
    if( stat( "/usr/bin/clang", &st ) == 0 )
        clang_bin_timestamp = st.st_mtime;
    else
        clang_bin_timestamp = 0;
}

bool native_env_uptodate()
{
    struct stat st;
    if( stat( "/usr/bin/gcc", &st ) == 0 ) {
        if( st.st_mtime != gcc_bin_timestamp )
            return false;
    } else {
        if( gcc_bin_timestamp != 0 )
            return false;
    }
    if( stat( "/usr/bin/g++", &st ) == 0 ) {
        if( st.st_mtime != gpp_bin_timestamp )
            return false;
    } else {
        if( gpp_bin_timestamp != 0 )
            return false;
    }
    if( stat( "/usr/bin/clang", &st ) == 0 ) {
        if( st.st_mtime != clang_bin_timestamp )
            return false;
    } else {
        if( clang_bin_timestamp != 0 )
            return false;
    }
    return true;
}

size_t setup_env_cache(const string &basedir, string &native_environment, uid_t nobody_uid, gid_t nobody_gid)
{
    native_environment = "";
    string nativedir = basedir + "/native/";

    // Either both gcc and g++ are needed, and/or clang.
    bool ok = false;
    if ( ::access( "/usr/bin/gcc", X_OK ) == 0 && ::access( "/usr/bin/g++", X_OK ) == 0 ) 
        ok = true;
    if ( ::access( "/usr/bin/clang", X_OK ) == 0 )
        ok = true;
    if ( !ok )
	return 0;

    if ( mkdir( nativedir.c_str(), 0775 ) && errno != EEXIST )
   	return 0; 

    if ( chown( nativedir.c_str(), 0, nobody_gid ) ||
         chmod( nativedir.c_str(), 0775 ) ) {
	rmdir( nativedir.c_str() );
	return 0;
    }

    flush_debug();
    int pipes[ 2 ];
    pipe( pipes );
    pid_t pid = fork();
    if ( pid ) {
        close( pipes[ 1 ] );
        int status = 1;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
           ;
        trace() << "waitpid " << status << endl;
        if ( !status ) {
            char buf[ 1024 ];
            buf[ 0 ] = '\0';
            while ( read( pipes[ 0 ], buf, 1023 ) < 0 && errno == EINTR )
                ;
            if ( char *nl = strchr( buf, '\n' ))
                *nl = '\0';
            native_environment = nativedir + buf;
        }
        close( pipes[ 0 ] );
        trace() << "native_environment " << native_environment << endl;
        struct stat st;
        if ( !status && !native_environment.empty()
            && stat( native_environment.c_str(), &st ) == 0 ) {
            save_native_env_timestamp();
            return st.st_size;
        }
        rmdir( nativedir.c_str() );
        return 0;
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

    close( pipes[ 0 ] );
    dup2( pipes[ 1 ], 5 ); // icecc-create-env will write the hash there
    close( pipes[ 1 ] );

    const char *const argv[] = {
        BINDIR "/icecc", "--build-native", NULL
    };
    if ( !exec_and_wait( argv ) ) {
        log_error() << BINDIR "/icecc --build-native failed\n";
        _exit(1);
    }
    _exit( 0 );
}


pid_t start_install_environment( const std::string &basename, const std::string &target,
                          const std::string &name, MsgChannel *c,
                          int& pipe_to_stdin, FileChunkMsg*& fmsg,
                          uid_t nobody_uid, gid_t nobody_gid )
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

    fmsg = dynamic_cast<FileChunkMsg*>( msg );
    enum { BZip2, Gzip, None} compression = None;
    if ( fmsg->len > 2 )
    {
        if ( fmsg->buffer[0] == 037 && fmsg->buffer[1] == 0213 )
            compression = Gzip;
        else if ( fmsg->buffer[0] == 'B' && fmsg->buffer[1] == 'Z' )
            compression = BZip2;
    }

    if ( mkdir( dirname.c_str(), 0770 ) && errno != EEXIST ) {
        log_perror( "mkdir target" );
        return 0;
    }

    if ( chown( dirname.c_str(), 0, nobody_gid ) ||
         chmod( dirname.c_str(), 0770 ) ) {
        log_perror( "chown,chmod target" );
        return 0;
    }

    dirname = dirname + "/" + name;
    if ( mkdir( dirname.c_str(), 0770 ) ) {
        log_perror( "mkdir name" );
        return 0;
    }

    if ( chown( dirname.c_str(), 0, nobody_gid ) ||
         chmod( dirname.c_str(), 0770 ) ) {
        log_perror( "chown,chmod name" );
        return 0;
    }

    int fds[2];
    if ( pipe( fds ) )
        return 0;

    flush_debug();
    pid_t pid = fork();
    if ( pid )
    {
        trace() << "pid " << pid << endl;
        close( fds[0] );
        pipe_to_stdin = fds[1];

        return pid;
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
    argv[0] = strdup( TAR );
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
    string dirname = basename + "/target=" + env;

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

size_t remove_native_environment( const string &basedir, const string &env )
{
    if ( env.empty() )
        return 0;
    string nativedir = basedir + "/native/";
    size_t size = sumup_dir( nativedir );
    unlink( env.c_str());
    return size;
}

static void
error_client( MsgChannel *client, string error )
{
    if ( IS_PROTOCOL_23( client ) )
        client->send_msg( StatusTextMsg( error ) );
}

void chdir_to_environment( MsgChannel *client, const string &dirname, uid_t nobody_uid, gid_t nobody_gid )
{
    if ( getuid() == 0 ) {
        // without the chdir, the chroot will escape the
        // jail right away
        if ( chdir( dirname.c_str() ) < 0 ) {
            error_client( client, string( "chdir to " ) + dirname + "failed" );
            log_perror("chdir() failed" );
            _exit(145);
        }
        if ( chroot( dirname.c_str() ) < 0 ) {
            error_client( client, string( "chroot " ) + dirname + "failed" );
            log_perror("chroot() failed" );
            _exit(144);
        }
        if ( setgid( nobody_gid ) < 0 ) {
            error_client( client, string( "setgid failed" ));
            log_perror("setgid() failed" );
            _exit(143);
        }
        if ( setuid( nobody_uid ) < 0) {
            error_client( client, string( "setuid failed" ));
            log_perror("setuid() failed" );
            _exit(142);
        }
    }
    else {
        if ( chdir( dirname.c_str() ) ) {
            log_perror( "chdir" );
        } else {
            trace() << "chdir to " << dirname << endl;
        }
    }
}

// Verify that the environment works by simply running the bundled bin/true.
bool verify_env( MsgChannel *client, const string &basedir, const string& target, const string &env,
                 uid_t nobody_uid, gid_t nobody_gid )
{
    if ( target.empty() || env.empty())
        return false;

    string dirname = basedir + "/target=" + target + "/" + env;
    if ( ::access( string( dirname + "/bin/true" ).c_str(), X_OK ) ) {
        error_client( client, dirname + "/bin/true is not executable" );
        log_error() << "I don't have environment " << env << "(" << target << ") to verify." << endl;
        return false;
    }

    flush_debug();
    pid_t pid = fork();
    assert(pid >= 0);
    if ( pid > 0) { // parent
        int status;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
            ;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    // child
    reset_debug(0);
    chdir_to_environment( client, dirname, nobody_uid, nobody_gid );
    _exit(execl("bin/true", "bin/true", (void*)NULL));
}
