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
    dcc_client_catch_signals();
    string compiler_name = argv[0];

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
    bool local = analyse_argv( argv, job );

    Service *serv = new Service ("localhost", 10245);
    MsgChannel *local_daemon = serv->channel();
    if ( ! local_daemon || !local_daemon->protocol ) {
        log_warning() << "no local daemon found\n";
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

    Environments envs;
    if ( getenv( "ICECC_VERSION" ) ) // if set, use it, otherwise take default
        envs = parse_icecc_version( job.targetPlatform() );
    else {
        string native = ucs->nativeVersion;
        if ( native.empty() ) {
            log_warning() << "$ICECC_VERSION has to point to an existing file to be installed - as the local daemon didn't know any we try local." << endl
                          << "Hint: you need /usr/bin/gcc _and_ /usr/bin/g++." << endl;
            delete ucs;
            return build_local( job, 0 );
        }
        envs.push_back(make_pair( job.targetPlatform(), native ) );
    }

    for ( Environments::const_iterator it = envs.begin(); it != envs.end(); ++it )
        trace() << "env: " << it->first << " '" << it->second << "'" << endl;

    trace() << "contacting scheduler " << ucs->hostname << ":" << ucs->port << endl;

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
    else {
        try {
            // by default every 100th is compiled three times
            const char *s = getenv( "ICECC_REPEAT_RATE" );
            int rate = s ? atoi( s ) : 10;
            ret = build_remote( job, scheduler, envs, rate );
        } catch ( int error ) {
            delete scheduler;
            return build_local( job, 0 );
        }
    }
    scheduler->send_msg (EndMsg());
    delete scheduler;
    return ret;
}
