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

// Required by strsignal() on some systems.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <sys/types.h>
#include <sys/wait.h>

#include "client.h"

/* Name of this program, for trace.c */
const char * rs_program_name = "icecc";

using namespace std;

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
"Environment Variables:\n"
"   ICECC_VERSION              use a specific icecc environment, see create-env\n"
"   ICECC_REPEAT_RATE          the number of jobs out of 1000 that should be\n"
"                              compiled on multiple hosts to ensure that they're\n"
"                              producing the same output.  The default is 10.\n"
"\n");
}


volatile bool local = false;

static void dcc_client_signalled (int whichsig)
{
    if ( !local ) {
#ifdef HAVE_STRSIGNAL
        log_info() << rs_program_name << ": " << strsignal(whichsig) << endl;
#else
        log_info() << "terminated by signal " << whichsig << endl;
#endif

        //    dcc_cleanup_tempfiles();
    }

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
    char *env = getenv( "ICECC_DEBUG" );
    int debug_level = Error;
    if ( env ) {
        if ( !strcasecmp( env, "info" ) )  {
            debug_level |= Info|Warning;
        } else if ( !strcasecmp( env, "debug" ) ||
                    !strcasecmp( env, "trace" ) )  {
            debug_level |= Info|Debug|Warning;
        } else if ( !strcasecmp( env, "warnings" ) ) {
            debug_level |= Warning; // taking out warning
        }
    }
    setup_debug(debug_level);
    string compiler_name = argv[0];
    dcc_client_catch_signals();

    if ( find_basename( compiler_name ) == rs_program_name) {
        trace() << argc << endl;
        if ( argc > 1 ) {
            string arg = argv[1];
            if ( arg == "--help" ) {
                dcc_show_usage();
                return 0;
            }
            if ( arg == "--version" ) {
                printf( "ICECREAM 0.1\n" );
                return 0;
            }
        }
    }

    int sg_level = dcc_recursion_safeguard();

    if (sg_level > 0) {
        log_error() << "icecream seems to have invoked itself recursively!" << endl;
        return EXIT_RECURSION;
    }

    /* Ignore SIGPIPE; we consistently check error codes and will
     * see the EPIPE. */
    dcc_ignore_sigpipe(1);

    CompileJob job;

    /* if ICECC is set to no, then run job locally */
    if ( getenv( "ICECC" ) ) {
	string icecc;
	icecc = getenv( "ICECC" );
        if ( icecc == "no" ) {
	    local = true;
        }
    }

    local |= analyse_argv( argv, job );

    pid_t pid = 0;

    /* for local jobs, we fork off a child that tells the scheduler that we got something
       to do and kill that child when we're done. This way we can start right away with the
       action without any round trip delays and the scheduler can tell the monitors anyway
    */
    if ( local ) {
        pid = fork();
        if ( pid ) { // do your job and kill the rest
            struct rusage ru;
            int ret = build_local( job, &ru );
            int status;
            if ( waitpid( pid, &status, WNOHANG ) != pid && errno != ECHILD)
                kill( pid, SIGTERM );
            return ret; // exit the program
        }
    }

    MsgChannel *local_daemon = Service::createChannel( "127.0.0.1", 10245, 0); // 0 == no timeout
    if ( ! local_daemon ) {
        log_warning() << "no local daemon found\n";
        delete local_daemon;
        return local || build_local( job );
    }
    if ( !local_daemon->send_msg( GetSchedulerMsg( getenv( "ICECC_VERSION" ) == 0 && !local) ) ) {
        log_warning() << "failed to write get scheduler\n";
        delete local_daemon;
        return local || build_local( job );
    }

    // the timeout is high because it creates the native version
    Msg *umsg = local_daemon->get_msg(4 * 60);
    if ( !umsg || umsg->type != M_USE_SCHEDULER ) {
        delete local_daemon;
        return local || build_local( job );
    }
    UseSchedulerMsg *ucs = dynamic_cast<UseSchedulerMsg*>( umsg );
    Environments envs;

    if ( !local ) {
        if ( getenv( "ICECC_VERSION" ) ) { // if set, use it, otherwise take default
            try {
                envs = parse_icecc_version( job.targetPlatform() );
            } catch ( int x ) {
                // we just build locally
            }
        } else {
            string native = ucs->nativeVersion;
            if ( native.empty() || ::access( native.c_str(), R_OK ) ) {
                log_warning() << "$ICECC_VERSION has to point to an existing file to be installed - as the local daemon didn't know any we try local." << endl;
            } else
                envs.push_back(make_pair( job.targetPlatform(), native ) );
        }
    }

    bool error = ( envs.size() == 0 );
    for ( Environments::const_iterator it = envs.begin(); it != envs.end(); ++it ) {
        trace() << "env: " << it->first << " '" << it->second << "'" << endl;
        if ( ::access( it->second.c_str(), R_OK ) ) {
            log_error() << "can't read environment " << it->second << endl;
            error = true;
            break;
        }
    }

    if ( error ) {
        delete local_daemon;
        delete ucs;
        return local || build_local( job );
    }

    trace() << "contacting scheduler " << ucs->hostname << ":" << ucs->port << endl;

    delete local_daemon;

    MsgChannel *scheduler = Service::createChannel( ucs->hostname, ucs->port, 0 ); // 0 == no time out
    if ( ! scheduler ) {
        log_warning() << "no scheduler found at " << ucs->hostname << ":" << ucs->port << endl;
        delete scheduler;
	delete ucs;
        return local || build_local( job );
    }

    delete ucs;

    if ( local ) {
        scheduler->send_msg( JobLocalBeginMsg( get_absfilename( job.outputFile() )) );
        sleep( 30 * 60 ); // wait for the kill by parent - without killing the scheduler connection ;/
        delete scheduler;
        return 0;
    }

    int ret;
    try {
        // by default every 100th is compiled three times
        const char *s = getenv( "ICECC_REPEAT_RATE" );
        int rate = s ? atoi( s ) : 0;
        ret = build_remote( job, scheduler, envs, rate);
    } catch ( int error ) {
        delete scheduler;
        return build_local( job );
    }
    scheduler->send_msg (EndMsg());
    delete scheduler;
    return ret;
}
