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

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <vector>
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#include <comm.h>
#include "client.h"

using namespace std;

/* Name of this program, for trace.c */
const char *rs_program_name = "icecc";

#define CLIENT_DEBUG 0

static string compiler_path_lookup_helper(const string &compiler, const string &compiler_path)
{
    if (compiler_path.find_first_of('/') != string::npos) {
        return compiler_path;
    }

    string path = ::getenv("PATH");
    string::size_type begin = 0;
    string::size_type end = 0;
    struct stat s;
    bool after_selflink = false;
    string best_match;

    while (end != string::npos) {
        end = path.find_first_of(':', begin);
        string part;

        if (end == string::npos) {
            part = path.substr(begin);
        } else {
            part = path.substr(begin, end - begin);
        }

        begin = end + 1;

        part = part + '/' + compiler;

        if (!lstat(part.c_str(), &s)) {
            if (S_ISLNK(s.st_mode)) {
                std::string buffer;
                const int ret = resolve_link(part, buffer);

                if (ret != 0) {
                    log_error() << "resolve_link failed " << strerror(ret) << endl;
                    continue;
                }

                string target = find_basename(buffer);

                if (target == rs_program_name
                        || (after_selflink
                            && (target == "tbcompiler" || target == "distcc"
                                || target == "colorgcc"))) {
                    // this is a link pointing to us, ignore it
                    after_selflink = true;
                    continue;
                }
            } else if (!S_ISREG(s.st_mode)) {
                // It's not a link and not a file, so just ignore it. We don't
                // want to accidentially attempt to execute directories.
                continue;
            }

            best_match = part;

            if (after_selflink) {
                return part;
            }
        }
    }

    if (best_match.empty()) {
        log_error() << "couldn't find any " << compiler << endl;
    }

    return best_match;
}

string compiler_path_lookup(const string& compiler)
{
    return compiler_path_lookup_helper(compiler, compiler);
}

/*
 * Get the name of the compiler depedant on the
 * language of the job and the environment
 * variable set. This is useful for native cross-compilers.
 * (arm-linux-gcc for example)
 */
string find_compiler(const CompileJob &job)
{
    if (job.language() == CompileJob::Lang_C) {
        if (const char *env = getenv("ICECC_CC")) {
            return env;
        }
    }

    if (job.language() == CompileJob::Lang_CXX) {
        if (const char *env = getenv("ICECC_CXX")) {
            return env;
        }
    }

    return compiler_path_lookup_helper(job.compilerName(), job.compilerPathname());
}

bool compiler_is_clang(const CompileJob &job)
{
    if (job.language() == CompileJob::Lang_Custom) {
        return false;
    }

    assert(job.compilerName().find('/') == string::npos);
    return job.compilerName().find("clang") != string::npos;
}

/*
Clang works suboptimally when handling an already preprocessed source file,
for example error messages quote (already preprocessed) parts of the source.
Therefore it is better to only locally merge all #include files into the source
file and do the actual preprocessing remotely together with compiling.
There exists a Clang patch to implement option -frewrite-includes that does
such #include rewritting, and it's been only recently merged upstream.
*/
bool compiler_only_rewrite_includes(const CompileJob &job)
{
    if (compiler_is_clang(job)) {
        if (const char *rewrite_includes = getenv("ICECC_CLANG_REMOTE_CPP")) {
            return (*rewrite_includes != '\0') && (*rewrite_includes != '0');
        }

#ifdef HAVE_CLANG_REWRITE_INCLUDES
        // Assume that we use the same clang (as least as far as capabilities go)
        // as was available when icecream was built. ICECC_CLANG_REMOTE_CPP above
        // allows override, and the only case when this should realistically break
        // is if somebody downgrades their clang.
        return true;
#endif
    }

    return false;
}

static volatile int lock_fd = 0;
static volatile int user_break_signal = 0;
static volatile pid_t child_pid;

static void handle_user_break(int sig)
{
    if (lock_fd) {
        dcc_unlock(lock_fd);
    }

    lock_fd = 0;
    user_break_signal = sig;

    if (child_pid != 0) {
        kill(child_pid, sig);
    }

    signal(sig, handle_user_break);
}

/**
 * Invoke a compiler locally.  This is, obviously, the alternative to
 * dcc_compile_remote().
 *
 * The server does basically the same thing, but it doesn't call this
 * routine because it wants to overlap execution of the compiler with
 * copying the input from the network.
 *
 * This routine used to exec() the compiler in place of distcc.  That
 * is slightly more efficient, because it avoids the need to create,
 * schedule, etc another process.  The problem is that in that case we
 * can't clean up our temporary files, and (not so important) we can't
 * log our resource usage.
 *
 **/
int build_local(CompileJob &job, MsgChannel *local_daemon, struct rusage *used)
{
    list<string> arguments;

    string compiler_name = find_compiler(job);

    trace() << "invoking: " << compiler_name << endl;

    if (compiler_name.empty()) {
        log_error() << "could not find " << job.compilerName() << " in PATH." << endl;
        return EXIT_NO_SUCH_FILE;
    }

    arguments.push_back(compiler_name);
    appendList(arguments, job.allFlags());

    if (job.dwarfFissionEnabled()) {
        arguments.push_back("-gsplit-dwarf");
    }

    if (!job.inputFile().empty()) {
        arguments.push_back(job.inputFile());
    }

    if (!job.outputFile().empty()) {
        arguments.push_back("-o");
        arguments.push_back(job.outputFile());
    }

    vector<char*> argv; 

    for (list<string>::const_iterator it = arguments.begin(); it != arguments.end(); ++it) {
        argv.push_back(strdup(it->c_str()));
    }

    argv.push_back(0);
#if CLIENT_DEBUG
    trace() << "execing ";

    for (int i = 0; argv.at(i); i++) {
        trace() << argv.at(i) << " ";
    }

    trace() << endl;
#endif

    if (!local_daemon) {
        int fd;

        if (!dcc_lock_host(fd)) {
            log_error() << "can't lock for local job" << endl;
            return EXIT_DISTCC_FAILED;
        }

        lock_fd = fd;
    }

    bool color_output = job.language() != CompileJob::Lang_Custom
                        && colorify_wanted(job);
    int pf[2];

    if (color_output && pipe(pf)) {
        color_output = false;
    }

    if (used || color_output) {
        flush_debug();
        child_pid = fork();
    }

    if (child_pid == -1){
        log_perror("fork failed");
    }

    if (!child_pid) {
        dcc_increment_safeguard();

        if (color_output) {
            if ((-1 == close(pf[0])) && (errno != EBADF)){
                log_perror("close failed");
            }
            if ((-1 == close(2)) && (errno != EBADF)){
                log_perror("close failed");
            }
            if (-1 == dup2(pf[1], 2)){
                log_perror("dup2 failed");
            }
        }

        execv(argv[0], &argv[0]);
        log_perror("execv failed");

        if (lock_fd) {
            dcc_unlock(lock_fd);
        }

        {
            char buf[256];
            snprintf(buf, sizeof(buf), "ICECC[%d]: %s:", getpid(), argv[0]);
            log_perror(buf);
        }

        _exit(-1);
    }
    for(vector<char*>::const_iterator i = argv.begin(); i != argv.end(); ++i){
        free(*i);
    }
    argv.clear();

    if (color_output) {
        if ((-1 == close(pf[1])) && (errno != EBADF)){
            log_perror("close failed");
        }
    }

    // setup interrupt signals, so that the JobLocalBeginMsg will
    // have a matching JobLocalDoneMsg
    void (*old_sigint)(int) = signal(SIGINT, handle_user_break);
    void (*old_sigterm)(int) = signal(SIGTERM, handle_user_break);
    void (*old_sigquit)(int) = signal(SIGQUIT, handle_user_break);
    void (*old_sighup)(int) = signal(SIGHUP, handle_user_break);

    if (color_output) {
        string s_ccout;
        char buf[250];
        int r;

        for (;;) {
            while ((r = read(pf[0], buf, sizeof(buf) - 1)) > 0) {
                buf[r] = '\0';
                s_ccout.append(buf);
            }

            if (r == 0) {
                break;
            }

            if (r < 0 && errno != EINTR) {
                break;
            }
        }

        colorify_output(s_ccout);
    }

    int status = 1;

    while (wait4(child_pid, &status, 0, used) < 0 && errno == EINTR) {}

    status = shell_exit_status(status);

    signal(SIGINT, old_sigint);
    signal(SIGTERM, old_sigterm);
    signal(SIGQUIT, old_sigquit);
    signal(SIGHUP, old_sighup);

    if (user_break_signal) {
        raise(user_break_signal);
    }

    if (lock_fd) {
        dcc_unlock(lock_fd);
    }

    return status;
}
