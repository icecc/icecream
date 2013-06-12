/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
 *
 * icecc -- A simple distributed compiler system
 *
 * Copyright (C) 2003, 2004 by the Icecream Authors
 *
 * based on distcc
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


/* 4: The noise of a multitude in the
 * mountains, like as of a great people; a
 * tumultuous noise of the kingdoms of nations
 * gathered together: the LORD of hosts
 * mustereth the host of the battle.
 *      -- Isaiah 13 */



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
#include <limits.h>
#include <sys/time.h>
#include <comm.h>
#include <vector>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif
#include <sys/wait.h>

#include "client.h"

/* Name of this program, for trace.c */
const char *rs_program_name = "icecc";

using namespace std;

static void dcc_show_usage(void)
{
    printf(
        "Usage:\n"
        "   icecc [compiler] [compile options] -o OBJECT -c SOURCE\n"
        "   icecc --build-native [compilertype] [file...]\n"
        "   icecc --help\n"
        "\n"
        "Options:\n"
        "   --help                     explain usage and exit\n"
        "   --version                  show version and exit\n"
        "   --build-native             create icecc environment\n"
        "Environment Variables:\n"
        "   ICECC                      if set to \"no\", just exec the real compiler\n"
        "   ICECC_VERSION              use a specific icecc environment, see icecc-create-env\n"
        "   ICECC_DEBUG                [info | warnings | debug]\n"
        "                              sets verboseness of icecream client.\n"
        "   ICECC_LOGFILE              if set, additional debug information is logged to the specified file\n"
        "   ICECC_REPEAT_RATE          the number of jobs out of 1000 that should be\n"
        "                              compiled on multiple hosts to ensure that they're\n"
        "                              producing the same output.  The default is 0.\n"
        "   ICECC_PREFERRED_HOST       overrides scheduler decisions if set.\n"
        "   ICECC_CC                   set C compiler name (default gcc).\n"
        "   ICECC_CXX                  set C++ compiler name (default g++).\n"
        "   ICECC_CLANG_REMOTE_CPP     set to 1 or 0 to override remote precompiling with clang\n"
        "                              (requires clang -frewrite-includes option).\n"
        "   ICECC_IGNORE_UNVERIFIED    if set, hosts where environment cannot be verified are not used.\n"
        "   ICECC_EXTRAFILES           additional files used in the compilation.\n"
        "\n");
}

static void icerun_show_usage(void)
{
    printf(
        "Usage:\n"
        "   icerun [command]\n"
        "   icerun --help\n"
        "\n"
        "Options:\n"
        "   --help                     explain usage and exit\n"
        "   --version                  show version and exit\n"
        "Environment Variables:\n"
        "   ICECC                      if set to \"no\", just exec the real command\n"
        "   ICECC_DEBUG                [info | warnings | debug]\n"
        "                              sets verboseness of icecream client.\n"
        "   ICECC_LOGFILE              if set, additional debug information is logged to the specified file\n"
        "\n");
}

volatile bool local = false;

static void dcc_client_signalled(int whichsig)
{
    if (!local) {
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

static string read_output(const char *command)
{
    FILE *f = popen(command, "r");
    string output;

    if (!f) {
        log_error() << "no pipe " << strerror(errno) << endl;
        return output;
    }

    char buffer[1024];

    while (!feof(f)) {
        size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, f);
        buffer[bytes] = 0;
        output += buffer;
    }

    pclose(f);
    // get rid of the endline
    return output.substr(0, output.length() - 1);
}

/*
 * @param args Are [clang,gcc] [extra files...]
 */
static int create_native(char **args)
{
    bool is_clang = false;
    char **extrafiles = args;

    // Args[0] maybe a compiler or the first extra file.
    if (args[0] && ((!strcmp(args[0], "clang") && (is_clang = true))
                    || !strcmp(args[0], "gcc"))) {
        extrafiles++;
    }

    vector<char*> argv;
    struct stat st;

    if (lstat(PLIBDIR "/icecc-create-env", &st)) {
        log_error() << PLIBDIR "/icecc-create-env does not exist\n";
        return 1;
    }

    argv.push_back(strdup(PLIBDIR "/icecc-create-env"));

    if (is_clang) {
        string clang = compiler_path_lookup("clang");

        if (clang.empty()) {
            log_error() << "clang compiler not found\n";
            return 1;
        }

        if (lstat(PLIBDIR "/compilerwrapper", &st)) {
            log_error() << PLIBDIR "/compilerwrapper does not exist\n";
            return 1;
        }

        argv.push_back(strdup("--clang"));
        argv.push_back(strdup(clang.c_str()));
        argv.push_back(strdup(PLIBDIR "/compilerwrapper"));
    } else { // "gcc" (default)
        string gcc, gpp;

        // perhaps we're on gentoo
        if (!lstat("/usr/bin/gcc-config", &st)) {
            string gccpath = read_output("/usr/bin/gcc-config -B") + "/";
            gcc = gccpath + "gcc";
            gpp = gccpath + "g++";
        } else {
            gcc = compiler_path_lookup("gcc");
            gpp = compiler_path_lookup("g++");
        }

        // both C and C++ compiler are required
        if (gcc.empty() || gpp.empty()) {
            log_error() << "gcc compiler not found\n";
            return 1;
        }

        argv.push_back(strdup("--gcc"));
        argv.push_back(strdup(gcc.c_str()));
        argv.push_back(strdup(gpp.c_str()));
    }

    for (int extracount = 0; extrafiles[extracount]; extracount++) {
        argv.push_back(strdup("--addfile"));
        argv.push_back(strdup(extrafiles[extracount]));
    }

    argv.push_back(NULL);

    return execv(argv[0], argv.data());
}

int main(int argc, char **argv)
{
    char *env = getenv("ICECC_DEBUG");
    int debug_level = Error;

    if (env) {
        if (!strcasecmp(env, "info"))  {
            debug_level |= Info | Warning;
        } else if (!strcasecmp(env, "warnings")) {
            debug_level |= Warning; // taking out warning
        } else { // any other value
            debug_level |= Info | Debug | Warning;
        }
    }

    std::string logfile;

    if (const char *logfileEnv = getenv("ICECC_LOGFILE")) {
        logfile = logfileEnv;
    }

    setup_debug(debug_level, logfile, "ICECC");

    CompileJob job;
    bool icerun = false;

    string compiler_name = argv[0];
    dcc_client_catch_signals();

    if (find_basename(compiler_name) == rs_program_name) {
        if (argc > 1) {
            string arg = argv[1];

            if (arg == "--help") {
                dcc_show_usage();
                return 0;
            }

            if (arg == "--version") {
                printf("ICECC " VERSION "\n");
                return 0;
            }

            if (arg == "--build-native") {
                return create_native(argv + 2);
            }

            if (arg.size() > 0) {
                job.setCompilerName(arg);
            }
        }
    } else if (find_basename(compiler_name) == "icerun") {
        icerun = true;

        if (argc > 1) {
            string arg = argv[1];

            if (arg == "--help") {
                icerun_show_usage();
                return 0;
            }

            if (arg == "--version") {
                printf("ICERUN " VERSION "\n");
                return 0;
            }

            if (arg.size() > 0) {
                job.setCompilerName(arg);
            }
        }
    } else {
        std::string resolved;

        // check if it's a symlink to icerun
        if (resolve_link(compiler_name, resolved) == 0 && find_basename(resolved) == "icerun") {
            icerun = true;
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

    list<string> extrafiles;
    local |= analyse_argv(argv, job, icerun, &extrafiles);

    /* If ICECC is set to disable, then run job locally, without contacting
       the daemon at all. Because of file-based locking that is used in this
       case, all calls will be serialized and run by one.
       If ICECC is set to no, the job is run locally as well, but it is
       serialized using the daemon, so several may be run at once.
     */
    char *icecc = getenv("ICECC");

    if (icecc && !strcasecmp(icecc, "disable")) {
        return build_local(job, 0);
    }

    if (icecc && !strcasecmp(icecc, "no")) {
        local = true;
    }

    if (const char *extrafilesenv = getenv("ICECC_EXTRAFILES")) {
        for (;;) {
            const char *colon = strchr(extrafilesenv, ':');
            string file;

            if (colon == NULL) {
                file = extrafilesenv;
            } else {
                file = string(extrafilesenv, colon - extrafilesenv);
            }

            struct stat st;

            if (stat(file.c_str(), &st) == 0) {
                extrafiles.push_back(file);
            } else {
                log_warning() << "File in ICECC_EXTRAFILES not found: " << file << endl;
                return build_local(job, 0);
            }

            if (colon == NULL) {
                break;
            }

            extrafilesenv = colon + 1;
        }
    }

    /* try several options to reach the local daemon - 3 sockets, one TCP */
    MsgChannel *local_daemon = Service::createChannel("/var/run/icecc/iceccd.socket");

    if (!local_daemon) {
        local_daemon = Service::createChannel("/var/run/iceccd.socket");
    }

    if (!local_daemon) {
        string path = getenv("HOME");
        path += "/.iceccd.socket";
        local_daemon = Service::createChannel(path);
    }

    if (!local_daemon) {
        local_daemon = Service::createChannel("127.0.0.1", 10245, 0/*timeout*/);
    }

    if (!local_daemon) {
        log_warning() << "no local daemon found\n";
        return build_local(job, 0);
    }

    Environments envs;

    if (!local) {
        if (getenv("ICECC_VERSION")) {     // if set, use it, otherwise take default
            try {
                envs = parse_icecc_version(job.targetPlatform(), find_prefix(job.compilerName()));
            } catch (int x) {
                // we just build locally
            }
        } else if (!extrafiles.empty() && !IS_PROTOCOL_32(local_daemon)) {
            log_warning() << "Local daemon is too old to handle compiler plugins.\n";
            local = true;
        } else {
            if (!local_daemon->send_msg(GetNativeEnvMsg(compiler_is_clang(job)
                                        ? "clang" : "gcc", extrafiles))) {
                log_warning() << "failed to write get native environment\n";
                goto do_local_error;
            }

            // the timeout is high because it creates the native version
            Msg *umsg = local_daemon->get_msg(4 * 60);
            string native;

            if (umsg && umsg->type == M_NATIVE_ENV) {
                native = static_cast<UseNativeEnvMsg*>(umsg)->nativeVersion;
            }

            if (native.empty() || ::access(native.c_str(), R_OK)) {
                log_warning() << "daemon can't determine native environment. "
                              "Set $ICECC_VERSION to an icecc environment.\n";
            } else {
                envs.push_back(make_pair(job.targetPlatform(), native));
                log_info() << "native " << native << endl;
            }

            delete umsg;
        }

        // we set it to local so we tell the local daemon about it - avoiding file locking
        if (envs.size() == 0) {
            local = true;
        }

        for (Environments::const_iterator it = envs.begin(); it != envs.end(); ++it) {
            trace() << "env: " << it->first << " '" << it->second << "'" << endl;

            if (::access(it->second.c_str(), R_OK)) {
                log_error() << "can't read environment " << it->second << endl;
                local = true;
            }
        }
    }

    int ret;

    if (local) {
        log_block b("building_local");
        struct rusage ru;
        Msg *startme = 0L;

        /* Inform the daemon that we like to start a job.  */
        if (local_daemon->send_msg(JobLocalBeginMsg(0, get_absfilename(job.outputFile())))) {
            /* Now wait until the daemon gives us the start signal.  40 minutes
               should be enough for all normal compile or link jobs.  */
            startme = local_daemon->get_msg(40 * 60);
        }

        /* If we can't talk to the daemon anymore we need to fall back
           to lock file locking.  */
        if (!startme || startme->type != M_JOB_LOCAL_BEGIN) {
            goto do_local_error;
        }

        ret = build_local(job, local_daemon, &ru);
    } else {
        try {
            // check if it should be compiled three times
            const char *s = getenv("ICECC_REPEAT_RATE");
            int rate = s ? atoi(s) : 0;
            ret = build_remote(job, local_daemon, envs, rate);

            /* We have to tell the local daemon that everything is fine and
               that the remote daemon will send the scheduler our done msg.
               If we don't, the local daemon will have to assume the job failed
               and tell the scheduler - and that fail message may arrive earlier
               than the remote daemon's success msg. */
            if (ret == 0) {
                local_daemon->send_msg(EndMsg());
            }
        } catch (int error) {
            if (remote_daemon.size()) {
                log_error() << "got exception " << error
                            << " (" << remote_daemon.c_str() << ") " << endl;
            } else {
                log_error() << "got exception " << error << " (this should be an exception!)" <<
                            endl;
            }

            /* currently debugging a client? throw an error then */
            if (debug_level != Error) {
                return error;
            }

            goto do_local_error;
        }
    }

    delete local_daemon;
    return ret;

do_local_error:
    delete local_daemon;
    return build_local(job, 0);
}
