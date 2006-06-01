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
        } else if ( !strcasecmp( env, "warnings" ) ) {
            debug_level |= Warning; // taking out warning
        } else // any other value
            debug_level |= Info|Debug|Warning;
    }

    setup_debug(debug_level);

    string compiler_name = argv[0];
    dcc_client_catch_signals();

    if ( find_basename( compiler_name ) == rs_program_name) {
        if ( argc > 1 ) {
            string arg = argv[1];
            if ( arg == "--help" ) {
                dcc_show_usage();
                return 0;
            }
            if ( arg == "--version" ) {
                printf( "ICECREAM " VERSION "\n" );
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

    MsgChannel *local_daemon = Service::createChannel( "127.0.0.1", 10245, 0/*timeout*/);
    if ( ! local_daemon ) {
        log_warning() << "no local daemon found\n";
        return build_local( job, 0 );
    }

    Environments envs;

    if ( !local ) {
        if ( getenv( "ICECC_VERSION" ) ) { // if set, use it, otherwise take default
            try {
                envs = parse_icecc_version( job.targetPlatform() );
            } catch ( int x ) {
                // we just build locally
            }
        } else {
            if ( !local_daemon->send_msg( GetNativeEnvMsg() ) ) {
                log_warning() << "failed to write get native environment\n";
		goto do_local_error;
            }

            // the timeout is high because it creates the native version
            Msg *umsg = local_daemon->get_msg(4 * 60);
            trace() << "got " << (umsg ? ( char )umsg->type : '?') << endl;
            if ( !umsg || umsg->type != M_NATIVE_ENV ) {
		log_warning() << "daemon can't determine native environment. Set $ICECC_VERSION to an icecream environment.\n";
		delete umsg;
                goto do_local_error;
            }
            UseNativeEnvMsg *ucs = dynamic_cast<UseNativeEnvMsg*>( umsg );

            string native = ucs->nativeVersion;
            if ( native.empty() || ::access( native.c_str(), R_OK ) ) {
                log_warning() << "$ICECC_VERSION has to point to an existing file to be installed - as the local daemon didn't know any we try local." << endl;
            } else
                envs.push_back(make_pair( job.targetPlatform(), native ) );

            log_info() << "native " << native << endl;

            delete umsg;
        }

	// we set it to local so we tell the local daemon about it - avoiding file locking
        if ( envs.size() == 0 )
	    local = true;
        for ( Environments::const_iterator it = envs.begin(); it != envs.end(); ++it ) {
            trace() << "env: " << it->first << " '" << it->second << "'" << endl;
            if ( ::access( it->second.c_str(), R_OK ) ) {
                log_error() << "can't read environment " << it->second << endl;
                local = true;
            }
        }
    }

    int ret;
    if ( local ) {
        struct rusage ru;
	/* Inform the daemon that we like to start a job.  */
        local_daemon->send_msg( JobLocalBeginMsg( 0, get_absfilename( job.outputFile() )) );
	/* Now wait until the daemon gives us the start signal.  40 minutes
	   should be enough for all normal compile or link jobs.  */
	Msg *startme = local_daemon->get_msg (40*60);
	/* If we can't talk to the daemon anymore we need to fall back
	   to lock file locking.  */
        if (!startme || startme->type != M_JOB_LOCAL_BEGIN)
	    goto do_local_error;
        ret = build_local( job, local_daemon, &ru );
    } else {
        try {
            // check if it should be compiled three times
            const char *s = getenv( "ICECC_REPEAT_RATE" );
            int rate = s ? atoi( s ) : 0;
            ret = build_remote( job, local_daemon, envs, rate);
        } catch ( int error ) {
            fprintf( stderr, "got exception %d (this should be an exception!)\n", error );
            ret = build_local( job, 0 );
        }
    }
    local_daemon->send_msg (EndMsg());
    delete local_daemon;
    return ret;

do_local_error:
    delete local_daemon;
    return build_local( job, 0 );
}
