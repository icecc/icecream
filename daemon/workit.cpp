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
#include "workit.h"
#include "tempfile.h"
#include "assert.h"
#include "exitcode.h"
#include "logging.h"
#include "pipes.h"
#include <sys/select.h>
#include <algorithm>

#ifdef __FreeBSD__
#include <sys/param.h>
#endif

/* According to earlier standards */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_USER_H && !defined(__DragonFly__)
#  include <sys/user.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__APPLE__)
#ifndef RUSAGE_SELF
#define   RUSAGE_SELF     (0)
#endif
#ifndef RUSAGE_CHILDREN
#define   RUSAGE_CHILDREN     (-1)
#endif
#endif

#include <stdio.h>
#include <errno.h>
#include <string>

#include "comm.h"
#include "platform.h"
#include "util.h"

using namespace std;

static int death_pipe[2];

extern "C" {

    static void theSigCHLDHandler(int)
    {
        char foo = 0;
        ignore_result(write(death_pipe[1], &foo, 1));
    }

}

static void
error_client(MsgChannel *client, string error)
{
    if (IS_PROTOCOL_VERSION(23, client)) {
        client->send_msg(StatusTextMsg(error));
    }
}

/*
 * This is all happening in a forked child.
 * That means that we can block and be lazy about closing fds
 * (in the error cases which exit quickly).
 */

int work_it(CompileJob &j, unsigned int job_stat[], MsgChannel *client, CompileResultMsg &rmsg,
            const std::string &tmp_root, const std::string &build_path, const std::string &file_name,
            unsigned long int mem_limit, int client_fd)
{
    rmsg.out.erase(rmsg.out.begin(), rmsg.out.end());
    rmsg.out.erase(rmsg.out.begin(), rmsg.out.end());

    std::list<string> list = j.nonLocalFlags();

    if (!IS_PROTOCOL_VERSION(41, client) && j.dwarfFissionEnabled()) {
        list.push_back("-gsplit-dwarf");
    }

    trace() << "remote compile for file " << j.inputFile() << endl;

    string argstxt;
    for (std::list<string>::const_iterator it = list.begin();
         it != list.end(); ++it) {
        argstxt += ' ';
        argstxt += *it;
    }
    trace() << "remote compile arguments:" << argstxt << endl;

    int sock_err[2];
    int sock_out[2];
    int sock_in[2];
    int main_sock[2];
    char buffer[4096];

    if (pipe(sock_err)) {
        return EXIT_DISTCC_FAILED;
    }

    if (pipe(sock_out)) {
        return EXIT_DISTCC_FAILED;
    }

    if (pipe(main_sock)) {
        return EXIT_DISTCC_FAILED;
    }

    if (pipe(death_pipe)) {
        return EXIT_DISTCC_FAILED;
    }

    if (create_large_pipe(sock_in)) {
        return EXIT_DISTCC_FAILED;
    }

    if (fcntl(sock_in[1], F_SETFL, O_NONBLOCK)) {
        return EXIT_DISTCC_FAILED;
    }

    /* Testing */
    struct sigaction act;
    sigemptyset(&act.sa_mask);

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigaction(SIGPIPE, &act, nullptr);

    act.sa_handler = theSigCHLDHandler;
    act.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &act, nullptr);

    sigaddset(&act.sa_mask, SIGCHLD);
    // Make sure we don't block this signal. gdb tends to do that :-(
    sigprocmask(SIG_UNBLOCK, &act.sa_mask, nullptr);

    flush_debug();
    pid_t pid = fork();

    if (pid == -1) {
        return EXIT_OUT_OF_MEMORY;
    } else if (pid == 0) {

        setenv("PATH", "/usr/bin", 1);

        // Safety check
        if (getuid() == 0 || getgid() == 0) {
            error_client(client, "UID is 0 - aborting.");
            _exit(142);
        }

#ifdef RLIMIT_AS

// Sanitizers use huge amounts of virtual memory and the setrlimit() call below
// may lead to the process getting killed at any moment without any warning
// or message. Both gcc's and clang's macros are unreliable (no way to detect -fsanitize=leak,
// for example), but hopefully with the configure check this is good enough.
#ifndef SANITIZER_USED
#ifdef __SANITIZE_ADDRESS__
#define SANITIZER_USED
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SANITIZER_USED
#endif
#endif
#endif

#ifndef SANITIZER_USED
        struct rlimit rlim;

        rlim_t lim = mem_limit * 1024 * 1024;
        rlim.rlim_cur = lim;
        rlim.rlim_max = lim;

        if (setrlimit(RLIMIT_AS, &rlim)) {
            error_client(client, "setrlimit failed.");
            log_perror("setrlimit");
        } else {
            log_info() << "Compile job memory limit set to " << mem_limit << " megabytes" << endl;
        }
#endif
#endif

        int argc = list.size();
        argc++; // the program
        argc += 6; // -x c - -o file.o -fpreprocessed
        argc += 4; // gpc parameters
        argc += 9; // clang extra flags
        char **argv = new char*[argc + 1];
        int i = 0;
        bool clang = false;

        if (IS_PROTOCOL_VERSION(30, client)) {
            assert(!j.compilerName().empty());
            clang = (j.compilerName().find("clang") != string::npos);
            argv[i++] = strdup(("/usr/bin/" + j.compilerName()).c_str());
        } else {
            if (j.language() == CompileJob::Lang_C) {
                argv[i++] = strdup("/usr/bin/gcc");
            } else if (j.language() == CompileJob::Lang_CXX) {
                argv[i++] = strdup("/usr/bin/g++");
            } else {
                assert(0);
            }
        }

        argv[i++] = strdup("-x");
        if (j.language() == CompileJob::Lang_C) {
          argv[i++] = strdup("c");
        } else if (j.language() == CompileJob::Lang_CXX) {
          argv[i++] = strdup("c++");
        } else if (j.language() == CompileJob::Lang_OBJC) {
          argv[i++] = strdup("objective-c");
        } else if (j.language() == CompileJob::Lang_OBJCXX) {
          argv[i++] = strdup("objective-c++");
        } else {
            error_client(client, "language not supported");
            log_perror("language not supported");
        }

        if( clang ) {
            // gcc seems to handle setting main file name and working directory fine
            // (it gets it from the preprocessed info), but clang needs help
            if( !j.inputFile().empty()) {
                argv[i++] = strdup("-Xclang");
                argv[i++] = strdup("-main-file-name");
                argv[i++] = strdup("-Xclang");
                argv[i++] = strdup(j.inputFile().c_str());
            }
            if( !j.workingDirectory().empty()) {
                argv[i++] = strdup("-Xclang");
                argv[i++] = strdup("-fdebug-compilation-dir");
                argv[i++] = strdup("-Xclang");
                argv[i++] = strdup(j.workingDirectory().c_str());
            }
        }

        // HACK: If in / , Clang records DW_AT_name with / prepended .
        if (chdir((tmp_root + build_path).c_str()) != 0) {
            error_client(client, "/tmp dir missing?");
        }

        for (std::list<string>::const_iterator it = list.begin();
                it != list.end(); ++it) {
            argv[i++] = strdup(it->c_str());
        }

        if (!clang) {
            argv[i++] = strdup("-fpreprocessed");
        }

        argv[i++] = strdup("-");
        argv[i++] = strdup("-o");
        argv[i++] = strdup(file_name.c_str());

        if (!clang) {
            argv[i++] = strdup("--param");
            sprintf(buffer, "ggc-min-expand=%d", ggc_min_expand_heuristic(mem_limit));
            argv[i++] = strdup(buffer);
            argv[i++] = strdup("--param");
            sprintf(buffer, "ggc-min-heapsize=%d", ggc_min_heapsize_heuristic(mem_limit));
            argv[i++] = strdup(buffer);
        }

        if (clang) {
            argv[i++] = strdup("-no-canonical-prefixes");    // otherwise clang tries to access /proc/self/exe
        }

        if (!clang && j.dwarfFissionEnabled()) {
            sprintf(buffer, "-fdebug-prefix-map=%s/=/", tmp_root.c_str());
            argv[i++] = strdup(buffer);
        }

        // before you add new args, check above for argc
        argv[i] = nullptr;
        assert(i <= argc);

        argstxt.clear();
        for (int pos = 1;
             pos < i;
             ++pos ) {
            argstxt += ' ';
            argstxt += argv[pos];
        }
        trace() << "final arguments:" << argstxt << endl;

        close_debug();

        if ((-1 == close(sock_out[0])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if (-1 == dup2(sock_out[1], STDOUT_FILENO)){
            log_perror("dup2 failed");
        }

        if ((-1 == close(sock_out[1])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if ((-1 == close(sock_err[0])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if (-1 == dup2(sock_err[1], STDERR_FILENO)){
            log_perror("dup2 failed");
        }

        if ((-1 == close(sock_err[1])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if ((-1 == close(sock_in[1])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if (-1 == dup2(sock_in[0], STDIN_FILENO)){
            log_perror("dup2 failed");
        }

        if ((-1 == close(sock_in[0])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if ((-1 == close(main_sock[0])) && (errno != EBADF)){
            log_perror("close failed");
        }
        fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);

        if ((-1 == close(death_pipe[0])) && (errno != EBADF)){
            log_perror("close failed");
        }

        if ((-1 == close(death_pipe[1])) && (errno != EBADF)){
            log_perror("close failed");
        }

#ifdef ICECC_DEBUG

        for (int f = STDERR_FILENO + 1; f < 4096; ++f) {
            long flags = fcntl(f, F_GETFD, 0);
            assert(flags < 0 || (flags & FD_CLOEXEC));
        }

#endif

        execv(argv[0], const_cast<char * const*>(argv));    // no return
        perror("ICECC: execv");

        char resultByte = 1;
        ignore_result(write(main_sock[1], &resultByte, 1));
        _exit(-1);
    }

    if ((-1 == close(sock_in[0])) && (errno != EBADF)){
        log_perror("close failed");
    }

    if ((-1 == close(sock_out[1])) && (errno != EBADF)){
        log_perror("close failed");
    }

    if ((-1 == close(sock_err[1])) && (errno != EBADF)){
        log_perror("close failed");
    }

    // idea borrowed from kprocess.
    // check whether the compiler could be run at all.
    if ((-1 == close(main_sock[1])) && (errno != EBADF)){
        log_perror("close failed");
    }

    for (;;) {
        char resultByte;
        ssize_t n = ::read(main_sock[0], &resultByte, 1);

        if (n == -1 && errno == EINTR) {
            continue;    // Ignore
        }

        if (n == 1) {
            rmsg.status = resultByte;

            log_error() << "compiler did not start" << endl;
            error_client(client, "compiler did not start");
            return EXIT_COMPILER_MISSING;
        }

        break; // != EINTR
    }

    if ((-1 == close(main_sock[0])) && (errno != EBADF)){
        log_perror("close failed");
    }

    struct timeval starttv;
    gettimeofday(&starttv, nullptr);

    int return_value = 0;
    // Got EOF for preprocessed input. stdout send may be still pending.
    bool input_complete = false;
    // Pending data to send to stdin
    FileChunkMsg *fcmsg = nullptr;
    size_t off = 0;

    log_block parent_wait("parent, waiting");

    for (;;) {
        if (client_fd >= 0 && !fcmsg) {
            if (Msg *msg = client->get_msg(0, true)) {
                if (input_complete) {
                    rmsg.err.append("client cancelled\n");
                    return_value = EXIT_CLIENT_KILLED;
                    client_fd = -1;
                    kill(pid, SIGTERM);
                    delete fcmsg;
                    fcmsg = nullptr;
                    delete msg;
                } else {
                    if (*msg == Msg::END) {
                        input_complete = true;

                        if (!fcmsg && sock_in[1] != -1) {
                            if (-1 == close(sock_in[1])){
                                log_perror("close failed");
                            }
                            sock_in[1] = -1;
                        }

                        delete msg;
                    } else if (*msg == Msg::FILE_CHUNK) {
                        fcmsg = static_cast<FileChunkMsg*>(msg);
                        off = 0;

                        job_stat[JobStatistics::in_uncompressed] += fcmsg->len;
                        job_stat[JobStatistics::in_compressed] += fcmsg->compressed;
                    } else {
                        log_error() << "protocol error while reading preprocessed file" << endl;
                        input_complete = true;
                        return_value = EXIT_IO_ERROR;
                        client_fd = -1;
                        kill(pid, SIGTERM);
                        delete fcmsg;
                        fcmsg = nullptr;
                        delete msg;
                    }
                }
            } else if (client->at_eof()) {
                log_warning() << "unexpected EOF while reading preprocessed file" << endl;
                input_complete = true;
                return_value = EXIT_IO_ERROR;
                client_fd = -1;
                kill(pid, SIGTERM);
                delete fcmsg;
                fcmsg = nullptr;
            }
        }

        vector< pollfd > pollfds;
        pollfd pfd; // tmp variable

        if (sock_out[0] >= 0) {
            pfd.fd = sock_out[0];
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }

        if (sock_err[0] >= 0) {
            pfd.fd = sock_err[0];
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }

        if (sock_in[1] == -1 && fcmsg) {
            // This state can occur when the compiler has terminated before
            // all file input is received from the client.  The daemon must continue
            // reading all file input from the client because the client expects it to.
            // Deleting the file chunk message here tricks the poll() below to continue
            // listening for more file data from the client even though it is being
            // thrown away.
            delete fcmsg;
            fcmsg = nullptr;
        }
        if (client_fd >= 0 && !fcmsg) {
            pfd.fd = client_fd;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
            // Note that we don't actually query the status of this fd -
            // we poll it in every iteration.
        }

        // If all file data has been received from the client then start
        // listening on the death_pipe to know when the compiler has
        // terminated.  The daemon can't start listening for the death of
        // the compiler sooner or else it might close the client socket before the
        // client had time to write all of the file data and wait for a response.
        // The client isn't coded to properly handle the closing of the socket while
        // sending all file data to the daemon.
        if (input_complete) {
            pfd.fd = death_pipe[0];
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }

        // Don't try to write to sock_in it if was already closed because
        // the compile terminated before reading all of the file data.
        if (fcmsg && sock_in[1] != -1) {
            pfd.fd = sock_in[1];
            pfd.events = POLLOUT;
            pollfds.push_back(pfd);
        }

        int timeout = input_complete ? -1 : 60 * 1000;

        switch (poll(pollfds.data(), pollfds.size(), timeout)) {
        case 0:

            if (!input_complete) {
                log_warning() << "timeout while reading preprocessed file" << endl;
                kill(pid, SIGTERM); // Won't need it any more ...
                return_value = EXIT_IO_ERROR;
                client_fd = -1;
                input_complete = true;
                delete fcmsg;
                fcmsg = nullptr;
                continue;
            }

            // this should never happen
            assert(false);
            return EXIT_DISTCC_FAILED;
        case -1:

            if (errno == EINTR) {
                continue;
            }

            // this should never happen
            assert(false);
            return EXIT_DISTCC_FAILED;
        default:

            if (fcmsg && pollfd_is_set(pollfds, sock_in[1], POLLOUT)) {
                ssize_t bytes = write(sock_in[1], fcmsg->buffer + off, fcmsg->len - off);

                if (bytes < 0) {
                    if (errno == EINTR) {
                        continue;
                    }

                    kill(pid, SIGTERM); // Most likely crashed anyway ...
                    if (input_complete) {
                        return_value = EXIT_COMPILER_CRASHED;
                    }
                    delete fcmsg;
                    fcmsg = nullptr;
                    if (-1 == close(sock_in[1])){
                        log_perror("close failed");
                    }
                    sock_in[1] = -1;
                    continue;
                }

                off += bytes;

                if (off == fcmsg->len) {
                    delete fcmsg;
                    fcmsg = nullptr;

                    if (input_complete) {
                        if (-1 == close(sock_in[1])){
                            log_perror("close failed");
                        }
                        sock_in[1] = -1;
                    }
                }
            }

            if (sock_out[0] >= 0 && pollfd_is_set(pollfds, sock_out[0], POLLIN)) {
                ssize_t bytes = read(sock_out[0], buffer, sizeof(buffer) - 1);

                if (bytes > 0) {
                    buffer[bytes] = 0;
                    rmsg.out.append(buffer);
                } else if (bytes == 0) {
                    if (-1 == close(sock_out[0])){
                        log_perror("close failed");
                    }
                    sock_out[0] = -1;
                }
            }

            if (sock_err[0] >= 0 && pollfd_is_set(pollfds, sock_err[0], POLLIN)) {
                ssize_t bytes = read(sock_err[0], buffer, sizeof(buffer) - 1);

                if (bytes > 0) {
                    buffer[bytes] = 0;
                    rmsg.err.append(buffer);
                } else if (bytes == 0) {
                    if (-1 == close(sock_err[0])){
                        log_perror("close failed");
                    }
                    sock_err[0] = -1;
                }
            }

            if (pollfd_is_set(pollfds, death_pipe[0], POLLIN)) {
                // Note that we have already read any remaining stdout/stderr:
                // the sigpipe is delivered after everything was written,
                // and the notification is multiplexed into the select above.

                struct rusage ru;
                int status;

                if (wait4(pid, &status, 0, &ru) != pid) {
                    // this should never happen
                    assert(false);
                    return EXIT_DISTCC_FAILED;
                }

                if (shell_exit_status(status) != 0) {
                    if( !rmsg.out.empty())
                        trace() << "compiler produced stdout output:\n" << rmsg.out;
                    if( !rmsg.err.empty())
                        trace() << "compiler produced stderr output:\n" << rmsg.err;
                    unsigned long int mem_used = ((ru.ru_minflt + ru.ru_majflt) * getpagesize()) / 1024;
                    rmsg.status = EXIT_OUT_OF_MEMORY;

                    if (((mem_used * 100) > (85 * mem_limit * 1024))
                            || (rmsg.err.find("memory exhausted") != string::npos)
                            || (rmsg.err.find("out of memory") != string::npos)
                            || (rmsg.err.find("annot allocate memory") != string::npos)
                            || (rmsg.err.find("failed to map segment from shared object") != string::npos)
                            || (rmsg.err.find("Assertion `NewElts && \"Out of memory\"' failed") != string::npos)
                            || (rmsg.err.find("terminate called after throwing an instance of 'std::bad_alloc'") != string::npos)
                            || (rmsg.err.find("llvm::MallocSlabAllocator::Allocate") != string::npos)) {
                        // the relation between ulimit and memory used is pretty thin ;(
                        log_warning() << "Remote compilation failed, presumably because of running out of memory (exit code "
                            << shell_exit_status(status) << ")" << endl;
                        return EXIT_OUT_OF_MEMORY;
                    }

#ifdef HAVE_SYS_VFS_H
                    struct statfs buf;
                    int ret = statfs( "/", &buf);
                    // If there's less than 10MiB of disk space free, we're probably running out of disk space.
                    if ((ret == 0 && long(buf.f_bavail) < ((10 * 1024 * 1024) / buf.f_bsize))
                            || rmsg.err.find("o space left on device") != string::npos) {
                        log_warning() << "Remote compilation failed, presumably because of running out of disk space (exit code "
                            << shell_exit_status(status) << ")" << endl;
                        return EXIT_IO_ERROR;
                    }
#endif

                }

                if (WIFEXITED(status)) {
                    struct timeval endtv;
                    gettimeofday(&endtv, nullptr);
                    rmsg.status = shell_exit_status(status);
                    job_stat[JobStatistics::exit_code] = shell_exit_status(status);
                    job_stat[JobStatistics::real_msec] = ((endtv.tv_sec - starttv.tv_sec) * 1000)
                                                         + ((long(endtv.tv_usec) - long(starttv.tv_usec)) / 1000);
                    job_stat[JobStatistics::user_msec] = (ru.ru_utime.tv_sec * 1000)
                                                         + (ru.ru_utime.tv_usec / 1000);
                    job_stat[JobStatistics::sys_msec] = (ru.ru_stime.tv_sec * 1000)
                                                        + (ru.ru_stime.tv_usec / 1000);
                    job_stat[JobStatistics::sys_pfaults] = ru.ru_majflt + ru.ru_nswap + ru.ru_minflt;
                    if(rmsg.status != 0) {
                        log_warning() << "Remote compilation exited with exit code " << shell_exit_status(status) << endl;
                    } else {
                        log_info() << "Remote compilation completed with exit code " << shell_exit_status(status) << endl;
                    }
                } else {
                    log_warning() << "Remote compilation aborted with exit code " << shell_exit_status(status) << endl;
                }

                return return_value;
            }
        }
    }
}
