/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//#define ICECC_DEBUG 1
#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

#include <signal.h>
#include <sys/types.h>

#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#endif

#include <deque>
#include <algorithm>
#include <set>
#include <fstream>
#include <string>
#include <pwd.h>
#include <sys/stat.h>

#include "services/ncpus.h"
#include "services/exitcode.h"
#include "workit.h"
#include "services/logging.h"
#include "load.h"
#include "environment.h"
#include "services/util.h"
#include "services/version.h"
#include "serve.h"

static std::string pidFilePath;
static volatile sig_atomic_t exit_main_loop = 0;

#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include "daemon.hpp"

static int set_new_pgrp()
{
    /* If we're a session group leader, then we are not able to call
     * setpgid().  However, setsid will implicitly have put us into a new
     * process group, so we don't have to do anything. */

    /* Does everyone have getpgrp()?  It's in POSIX.1.  We used to call
     * getpgid(0), but that is not available on BSD/OS. */
    int pgrp_id = getpgrp();

    if (-1 == pgrp_id){
        log_perror("Failed to get process group ID");
        return EXIT_DISTCC_FAILED;
    }

    if (pgrp_id == getpid()) {
        trace() << "already a process group leader\n";
        return 0;
    }

    if (setpgid(0, 0) == 0) {
        trace() << "entered process group\n";
        return 0;
    }

    trace() << "setpgid(0, 0) failed: " << strerror(errno) << std::endl;
    return EXIT_DISTCC_FAILED;
}

static void dcc_daemon_terminate(int);

/**
 * Catch all relevant termination signals.  Set up in parent and also
 * applies to children.
 **/
void dcc_daemon_catch_signals()
{
    /* SIGALRM is caught to allow for built-in timeouts when running test
     * cases. */

    signal(SIGTERM, &dcc_daemon_terminate);
    signal(SIGINT, &dcc_daemon_terminate);
    signal(SIGALRM, &dcc_daemon_terminate);
}

pid_t dcc_master_pid;

/**
 * Called when a daemon gets a fatal signal.
 *
 * Some cleanup is done only if we're the master/parent daemon.
 **/
static void dcc_daemon_terminate(int whichsig)
{
    /**
     * This is a signal handler. don't do stupid stuff.
     * Don't call printf. and especially don't call the log_*() functions.
     */

    if (exit_main_loop > 1) {
        // The > 1 is because we get one more signal from the kill(0,...) below.
        // hmm, we got killed already twice. try better
        static const char msg[] = "forced exit.\n";
        ignore_result(write(STDERR_FILENO, msg, strlen( msg )));
        _exit(1);
    }

    // make BSD happy
    signal(whichsig, dcc_daemon_terminate);

    bool am_parent = (getpid() == dcc_master_pid);

    if (am_parent && exit_main_loop == 0) {
        /* kill whole group */
        kill(0, whichsig);

        /* Remove pid file */
        unlink(pidFilePath.c_str());
    }

    ++exit_main_loop;
}

void usage(const char *reason = nullptr)
{
    if (reason) {
        std::cerr << reason << std::endl;
    }

    std::cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [--no-remote] [-d|--daemonize] [-l logfile] [-s <schedulerhost[:port]>]"
                 " [-v[v[v]]] [-u|--user-uid <user_uid>] [-b <env-basedir>] [--cache-limit <MB>] [-N <node_name>] [-i|--interface <net_interface>] [-p|--port <port>]" << std::endl;
    exit(1);
}

// Initial rlimit for a compile job, measured in megabytes.  Will vary with
// the amount of available memory.
int mem_limit = 100;

// Minimum rlimit for a compile job, measured in megabytes.

unsigned int max_kids = 0;

int main(int argc, char **argv)
{
    int max_processes = -1;
    srand(time(nullptr) + getpid());

    Daemon d{mem_limit, max_kids, exit_main_loop};

    int debug_level = Error;
    std::string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h

    while (true) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, nullptr, 'n' },
            { "max-processes", 1, nullptr, 'm' },
            { "help", 0, nullptr, 'h' },
            { "daemonize", 0, nullptr, 'd'},
            { "log-file", 1, nullptr, 'l'},
            { "nice", 1, nullptr, 0},
            { "name", 1, nullptr, 'N'},
            { "scheduler-host", 1, nullptr, 's' },
            { "env-basedir", 1, nullptr, 'b' },
            { "user-uid", 1, nullptr, 'u'},
            { "cache-limit", 1, nullptr, 0},
            { "no-remote", 0, nullptr, 0},
            { "interface", 1, nullptr, 'i'},
            { "port", 1, nullptr, 'p'},
            { nullptr, 0, nullptr, 0 }
        };

        const int c = getopt_long(argc, argv, "N:n:m:l:s:hvdb:u:i:p:", long_options, &option_index);

        if (c == -1) {
            break;    // eoo
        }

        switch (c) {
        case 0: {
            std::string optname = long_options[option_index].name;

            if (optname == "nice") {
                if (optarg && *optarg) {
                    errno = 0;
                    int tnice = atoi(optarg);

                    if (!errno) {
                        nice_level = tnice;
                    }
                } else {
                    usage("Error: --nice requires argument");
                }
            } else if (optname == "name") {
                if (optarg && *optarg) {
                    d.nodename = optarg;
                } else {
                    usage("Error: --name requires argument");
                }
            } else if (optname == "cache-limit") {
                if (optarg && *optarg) {
                    errno = 0;
                    int mb = atoi(optarg);

                    if (!errno) {
                        d.cache_size_limit = mb * 1024 * 1024;
                    }
                } else {
                    usage("Error: --cache-limit requires argument");
                }
            } else if (optname == "no-remote") {
                d.noremote = true;
            }

        }
        break;
        case 'd':
            detach = true;
            break;
        case 'N':

            if (optarg && *optarg) {
                d.nodename = optarg;
            } else {
                usage("Error: -N requires argument");
            }

            break;
        case 'l':

            if (optarg && *optarg) {
                logfile = optarg;
            } else {
                usage("Error: -l requires argument");
            }

            break;
        case 'v':

            if (debug_level < MaxVerboseLevel) {
                debug_level++;
            }

            break;
        case 'n':

            if (optarg && *optarg) {
                d.netname = optarg;
            } else {
                usage("Error: -n requires argument");
            }

            break;
        case 'm':

            if (optarg && *optarg) {
                max_processes = atoi(optarg);
            } else {
                usage("Error: -m requires argument");
            }

            break;
        case 's':

            if (optarg && *optarg) {
                std::string scheduler = optarg;
                size_t colon = scheduler.rfind( ':' );
                if( colon == std::string::npos ) {
                    d.schedname = scheduler;
                } else {
                    d.schedname = scheduler.substr(0, colon);
                    d.scheduler_port = atoi( scheduler.substr( colon + 1 ).c_str());
                    if( d.scheduler_port == 0 ) {
                        usage("Error: -s requires valid port if hostname includes colon");
                    }
                }
            } else {
                usage("Error: -s requires hostname argument");
            }

            break;
        case 'b':

            if (optarg && *optarg) {
                d.envbasedir = optarg;
            }

            break;
        case 'u':

            if (optarg && *optarg) {
                struct passwd *pw = getpwnam(optarg);

                if (!pw) {
                    usage("Error: -u requires a valid username");
                } else {
                    d.user_uid = pw->pw_uid;
                    d.user_gid = pw->pw_gid;
                    d.warn_icecc_user_errno = 0;

                    if (!d.user_gid || !d.user_uid) {
                        usage("Error: -u <username> must not be root");
                    }
                }
            } else {
                usage("Error: -u requires a valid username");
            }

            break;
        case 'i':

            if (optarg && *optarg) {
                std::string daemon_interface = optarg;
                if (daemon_interface.empty()) {
                    usage("Error: Invalid network interface specified");
                }

                d.daemon_interface = daemon_interface;
            } else {
                usage("Error: -i requires argument");
            }

            break;
        case 'p':

            if (optarg && *optarg) {
                d.daemon_port = atoi(optarg);

                if (0 == d.daemon_port) {
                    usage("Error: Invalid port specified");
                }
            } else {
                usage("Error: -p requires argument");
            }

            break;
        default:
            usage();
        }
    }

    if (d.warn_icecc_user_errno != 0) {
        log_errno("No icecc user on system. Falling back to nobody.", d.warn_icecc_user_errno);
    }

    umask(022);

    bool remote_disabled = false;
    if (getuid() == 0) {
        if (!logfile.length() && detach) {
            mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            ignore_result(chown("/var/log/icecc", d.user_uid, d.user_gid));
            logfile = "/var/log/icecc/iceccd.log";
        }

        mkdir("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        chmod("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        ignore_result(chown("/var/run/icecc", d.user_uid, d.user_gid));

#ifdef HAVE_LIBCAP_NG
        capng_clear(CAPNG_SELECT_BOTH);
        capng_update(CAPNG_ADD, (capng_type_t)(CAPNG_EFFECTIVE | CAPNG_PERMITTED), CAP_SYS_CHROOT);
        int r = capng_change_id(d.user_uid, d.user_gid,
                                (capng_flags_t)(CAPNG_DROP_SUPP_GRP | CAPNG_CLEAR_BOUNDING));
        if (r) {
            log_error() << "Error: capng_change_id failed: " << r << std::endl;
            exit(EXIT_SETUID_FAILED);
        }
#endif
    } else {
#ifdef HAVE_LIBCAP_NG
        // It's possible to have the capability even without being root.
        if (!capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        {
#endif
            d.noremote = true;
            remote_disabled = true;
        }
    }

    setup_debug(debug_level, logfile);

    log_info() << "ICECREAM daemon " VERSION " starting up (nice level "
               << nice_level << ") " << std::endl;
    if (remote_disabled)
        log_warning() << "Cannot use chroot, no remote jobs accepted." << std::endl;
    if (d.noremote)
        d.daemon_port = 0;

    d.determine_system();

    if (chdir("/") != 0) {
        log_error() << "failed to switch to root directory: "
                    << strerror(errno) << std::endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (detach)
        if (daemon(0, 0)) {
            log_perror("Failed to run as a daemon.");
            exit(EXIT_DISTCC_FAILED);
        }

    if (dcc_ncpus(&d.num_cpus) == 0) {
        log_info() << d.num_cpus << " CPU(s) online on this server" << std::endl;
    }

    if (max_processes < 0) {
        max_kids = d.num_cpus;
    } else {
        max_kids = max_processes;
    }

    log_info() << "allowing up to " << max_kids << " active jobs" << std::endl;

    d.determine_supported_features();
    log_info() << "supported features: " << supported_features_to_string(d.supported_features) << std::endl;

    int ret;

    /* Still create a new process group, even if not detached */
    trace() << "not detaching\n";

    if ((ret = set_new_pgrp()) != 0) {
        return ret;
    }

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << std::endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGCHLD) failed: " << strerror(errno) << std::endl;
        exit(EXIT_DISTCC_FAILED);
    }

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    std::ofstream pidFile;
    std::string progName = argv[0];
    progName = find_basename(progName);
    pidFilePath = std::string(RUNDIR) + std::string("/") + progName + std::string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << dcc_master_pid << std::endl;
    pidFile.close();

    if (!cleanup_cache(d.envbasedir, d.user_uid, d.user_gid)) {
        return 1;
    }

    std::list<std::string> nl = get_netnames(200, d.scheduler_port);
    trace() << "Netnames:" << std::endl;

    for (std::list<std::string>::const_iterator it = nl.begin(); it != nl.end(); ++it) {
        trace() << *it << std::endl;
    }

    if (!d.setup_listen_fds()) { // error
        return 1;
    }

    return d.working_loop();
}
