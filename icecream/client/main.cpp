/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
 *
 * icecc -- A simple distributed compiler system
 *
 * Copyright (C) 2003, 2004 by the Icecream Authors
 *
 * based on distcc
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */


			/* 4: The noise of a multitude in the
			 * mountains, like as of a great people; a
			 * tumultuous noise of the kingdoms of nations
			 * gathered together: the LORD of hosts
			 * mustereth the host of the battle.
			 *		-- Isaiah 13 */



#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <cassert>
#include <sys/time.h>
#include <comm.h>

#include "client.h"

/* Name of this program, for trace.c */
const char * rs_program_name = "icecc";

using namespace std;

// possibly platform specific
string split_out_file( const string &line )
{
    assert( line.find( '\n' ) == string::npos );

    string::size_type pos = line.find( "=>" );
    if ( pos == string::npos )
        return "";
    pos += 2;
    while ( isspace( line[pos] ) )
        pos++;
    string raw = line.substr( pos );
    pos = raw.find( ' ' );
    if ( pos != string::npos )
        raw = raw.substr( 0, pos );
    if ( raw[0] != '/' )
        return "";
    return raw;
}

list<string> split_ldd_output( string &out )
{
    list<string> res;
    string::size_type pos = 0;
    do {
        string::size_type opos = pos;
        pos = out.find( '\n', pos + 1);
        if ( pos == string::npos ) {
            res.push_back( split_out_file( out.substr( opos + 1) ) );
            break;
        }
        res.push_back( split_out_file( out.substr( opos + 1,
                                                   pos - opos - 1 ) ) );
    } while ( true );
    return res;
}

string read_prog_output( const char *command )
{
    FILE *pipe = popen( command, "r" );
    if ( !pipe ) {
        perror( "popen" );
        return "";
    }

    string output;
    char buffer[1000];
    do {
        size_t res = fread( buffer, 1,  999, pipe );
        buffer[res] = 0;
        output += buffer;
        if ( res < PATH_MAX - 1 )
            break;
    } while ( true );
    pclose(pipe);
    return output;
}

bool collect_file( const string &dirname, const string &file )
{
    if ( !::access( ( dirname + "/" + file ).c_str(), R_OK ) ) // already in there
        return true;

    trace() << "package " << file << endl;
    // even if the client runs on a higher version, we will not risk something
    setenv( "LD_ASSUME_KERNEL", "2.4.0", 1 );
    char buffer[PATH_MAX];
    sprintf( buffer, "/usr/bin/ldd '%s' 2>&1", file.c_str() );
    string output = read_prog_output( buffer );

    list<string> files = split_ldd_output( output );
    for ( list<string>::const_iterator it = files.begin();
          it != files.end(); ++it ) {
        if ( !collect_file( dirname, *it ) )
            return false;
    }
    string fdir = file.substr( 0, file.rfind( '/' ) + 1 );
    for ( string::size_type pos = 0; pos != string::npos; pos = fdir.find( '/', pos + 1 ) ) {
        string dir = dirname + "/" + fdir.substr( 0, pos );
        if ( ::access( dir.c_str(), R_OK ) ) {
            sprintf( buffer, "mkdir '%s'", dir.c_str() );
            system( buffer );
        }
    }
    sprintf( buffer,  "cp -p '%s' '%s/%s'", file.c_str(), dirname.c_str(), file.c_str() );
    system( buffer );

    struct timeval ts;
    ts.tv_sec = 0;
    ts.tv_usec = 0;
    sprintf( buffer, "%s/%s", dirname.c_str(), fdir.c_str() );
    utimes( buffer, &ts );

    utimes( buffer, &ts );
    fdir = file.substr( 0, file.rfind( '/' ) + 1 );
    for ( string::size_type pos = 0; pos != string::npos; pos = fdir.find( '/', pos + 1 ) ) {
        string dir = dirname + "/" + fdir.substr( 0, pos );
        utimes( dir.c_str(), &ts );
    }
    return true;
}

bool collect_env( )
{
    char dirname[] = "/var/tmp/icecream_env_XXXXXX";
    if ( !mkdtemp( dirname ) ) {
        perror( "mkdtemp /var/tmp/icecream_env_XXXXXX" );
        return -1;
    }

    // collect_file( dirname, "/bin/bash" ); // for testing chroot
    if ( !collect_file( dirname, "/usr/bin/g++" ) )
        return false;
    if ( !collect_file( dirname, "/usr/bin/gcc" ) )
        return false;
    if ( !collect_file( dirname, "/usr/bin/as" ) )
        return false;
    string output = read_prog_output( "/usr/bin/gcc -print-prog-name=cc1" );
    if ( output.size() < 5 )
        return false;
    while ( output[output.size() - 1] == '\n' )
        output = output.substr( 0, output.size() - 1 );
    trace() << "output " << output << " " <<  output.substr( output.size() - 4, 4 ) << endl;
    if ( output.size() < 5 || output.substr( output.size() - 4, 4 ) != "/cc1" )
        return false;
    if ( !collect_file( dirname, output ) )
        return false;
    output = output.substr( 0, output.size() - 3 );
    if ( !collect_file( dirname, output + "/cc1plus") )
        return false;
    collect_file( dirname, output + "/specs");

    char buffer[] = "/var/tmp/icecream_env_XXXXXX";
    if ( !mkstemp( buffer ) ) {
        perror( "mkstemp" );
        return false;
    }

    char system_buf[PATH_MAX * 2];
    sprintf( system_buf, "cd '%s' && tar cjpf '%s' .", dirname, buffer );

    if ( system( system_buf ) ) {
        printf( "called: %s\n", system_buf );
        return false;
    }
    sprintf( system_buf, "rm -r '%s'", dirname );
    if ( system( system_buf ) ) {
        printf( "called: %s\n", system_buf );
        return false;
    }
    sprintf( system_buf, "md5sum '%s'", buffer);
    FILE *pipe = popen( system_buf, "r" );
    size_t res = fread( system_buf, 1, PATH_MAX, pipe );
    pclose( pipe );
    system_buf[res] = 0;
    string md5sum = system_buf;
    md5sum = md5sum.substr( 0, md5sum.find( ' ' ) );
    sprintf( system_buf, "mv '%s' '%s.tar.bz2'", buffer, md5sum.c_str() );
    if ( system( system_buf ) )
        return false;
    printf( "created %s.tar.bz2\n", md5sum.c_str() );
    return true;
}

static void dcc_show_usage(void)
{
    printf(
"Usage:\n"
"   icecc [compile options] -o OBJECT -c SOURCE\n"
"   icecc --help\n"
"\n"
"Options:\n"
"   --help                     explain usage and exit\n"
"   --version                  show version and exit\n"
"   --create-env               will create an environment tar ball\n"
"\n");
}


static void dcc_client_signalled (int whichsig)
{
#ifdef HAVE_STRSIGNAL
    log_info() << rs_program_name << ": " << strsignal(whichsig) << endl;
#else
    log_info() << "terminated by signal " << whichsig << endl;
#endif

    //    dcc_cleanup_tempfiles();

    signal(whichsig, SIG_DFL);
    raise(whichsig);

}

static void dcc_client_catch_signals(void)
{
    signal(SIGTERM, &dcc_client_signalled);
    signal(SIGINT, &dcc_client_signalled);
    signal(SIGHUP, &dcc_client_signalled);
}


/**
 * distcc client entry point.
 *
 * This is typically called by make in place of the real compiler.
 *
 * Performs basic setup and checks for distcc arguments, and then kicks of
 * dcc_build_somewhere().
 **/
int main(int argc, char **argv)
{
    dcc_client_catch_signals();
    string compiler_name = argv[0];

    if ( argc == 1 && find_basename( compiler_name ) == rs_program_name) {
        dcc_show_usage();
        return 0;
    }

    if ( argc == 2 ) { // check for known arguments
        string arg = argv[1];
        if ( arg == "--help" ) {
            dcc_show_usage();
            return 0;
        }
        if ( arg == "--version" ) {
            printf( "ICECREAM 0.1\n" );
            return 0;
        }
        if ( arg == "--create-env" ) {

            if ( collect_env( ) )
                return 0;
            else
                return -1;
        }
    }

    /* Ignore SIGPIPE; we consistently check error codes and will
     * see the EPIPE. */
    dcc_ignore_sigpipe(1);

    CompileJob job;
    bool local = analyse_argv( argv, job );

    Service *serv = new Service ("localhost", 10245);
    MsgChannel *local_daemon = serv->channel();
    if ( ! local_daemon ) {
        log_error() << "no local daemon found\n";
        return build_local( job, 0 );
    }
    if ( !local_daemon->send_msg( GetSchedulerMsg() ) ) {
        log_error() << "failed to write get scheduler\n";
        return build_local( job, 0 );
    }

    Msg *umsg = local_daemon->get_msg();
    if ( !umsg || umsg->type != M_USE_SCHEDULER ) {
        log_error() << "umsg != scheduler\n";
        delete serv;
        return build_local( job, 0 );
    }
    UseSchedulerMsg *ucs = dynamic_cast<UseSchedulerMsg*>( umsg );
    delete serv;

    // log_info() << "contacting scheduler " << ucs->hostname << ":" << ucs->port << endl;

    serv = new Service( ucs->hostname, ucs->port );
    MsgChannel *scheduler = serv->channel();
    if ( ! scheduler ) {
        log_error() << "no scheduler found at " << ucs->hostname << ":" << ucs->port << endl;
        delete serv;
        return build_local( job, 0 );
    }
    delete ucs;

    int ret;
    if ( local )
        ret = build_local( job, scheduler );
    else
        ret = build_remote( job, scheduler );

    scheduler->send_msg (EndMsg());
    delete scheduler;
    return ret;
}
