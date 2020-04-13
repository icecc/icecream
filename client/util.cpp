/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <limits.h>

#include <sys/types.h>
#include <pwd.h>

#include <sys/stat.h>
#include <sys/file.h>

#include <vector>

#include "client.h"
#include "exitcode.h"
#include "job.h"
#include "logging.h"
#include "ncpus.h"
#include "util.h"

using namespace std;

extern bool explicit_color_diagnostics;
extern bool explicit_no_show_caret;

/**
 * Set the `FD_CLOEXEC' flag of DESC if VALUE is nonzero,
 * or clear the flag if VALUE is 0.
 *
 * From the GNU C Library examples.
 *
 * @returns 0 on success, or -1 on error with `errno' set.
 **/
int set_cloexec_flag(int desc, int value)
{
    int oldflags = fcntl(desc, F_GETFD, 0);

    /* If reading the flags failed, return error indication now. */
    if (oldflags < 0) {
        return oldflags;
    }

    /* Set just the flag we want to set. */
    if (value != 0) {
        oldflags |= FD_CLOEXEC;
    } else {
        oldflags &= ~FD_CLOEXEC;
    }

    /* Store modified flag word in the descriptor. */
    return fcntl(desc, F_SETFD, oldflags);
}

/**
 * Ignore or unignore SIGPIPE.
 *
 * The server and child ignore it, because distcc code wants to see
 * EPIPE errors if something goes wrong.  However, for invoked
 * children it is set back to the default value, because they may not
 * handle the error properly.
 **/
int dcc_ignore_sigpipe(int val)
{
    if (signal(SIGPIPE, val ? SIG_IGN : SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, " << (val ? "ignore" : "default") << ") failed: "
                      << strerror(errno) << endl;
        return EXIT_DISTCC_FAILED;
    }

    return 0;
}

/**
 * Get an exclusive, non-blocking lock on a file using whatever method
 * is available on this system.
 *
 * @retval 0 if we got the lock
 * @retval -1 with errno set if the file is already locked.
 **/
static int sys_lock(int fd, bool block)
{
#if defined(F_SETLK)
    struct flock lockparam;

    lockparam.l_type = F_WRLCK;
    lockparam.l_whence = SEEK_SET;
    lockparam.l_start = 0;
    lockparam.l_len = 0;        /* whole file */

    return fcntl(fd, block ? F_SETLKW : F_SETLK, &lockparam);
#elif defined(HAVE_FLOCK)
    return flock(fd, LOCK_EX | (block ? 0 : LOCK_NB));
#elif defined(HAVE_LOCKF)
    return lockf(fd, block ? F_LOCK : F_TLOCK, 0);
#else
#  error "No supported lock method.  Please port this code."
#endif
}


static volatile int lock_fd = -1;

void dcc_unlock()
{
    // This must be safe to use from a signal handler.
    if (lock_fd != -1)
        close(lock_fd); // All our current locks can just be closed.
    lock_fd = -1;
}


/**
 * Open a lockfile, creating if it does not exist.
 **/
static bool dcc_open_lockfile(const string &fname, int &plockfd)
{
    /* Create if it doesn't exist.  We don't actually do anything with
     * the file except lock it.
     *
     * The file is created with the loosest permissions allowed by the user's
     * umask, to give the best chance of avoiding problems if they should
     * happen to use a shared lock dir. */
    plockfd = open(fname.c_str(), O_WRONLY | O_CREAT, 0666);

    if (plockfd == -1 && errno != EEXIST) {
        log_error() << "failed to create " << fname << ": " << strerror(errno) << endl;
        return false;
    }

    set_cloexec_flag(plockfd, true);

    return true;
}

static bool dcc_lock_host_slot(string fname, int lock, bool block);

bool dcc_lock_host()
{
    assert(lock_fd == -1);

    string fname = "/tmp/.icecream-";
    struct passwd *pwd = getpwuid(getuid());

    if (pwd) {
        fname += pwd->pw_name;
    } else {
        char buffer[12];
        sprintf(buffer, "%ld", (long)getuid());
        fname += buffer;
    }

    if (mkdir(fname.c_str(), 0700) && errno != EEXIST) {
        log_perror("mkdir") << "\t" << fname << endl;
        return false;
    }

    fname += "/local_lock";
    lock_fd = 0;
    int max_cpu = 1;
    dcc_ncpus(&max_cpu);
    // To ensure better distribution, select a "random" starting slot.
    int lock_offset = getpid();
    // First try if any slot is free.
    for( int lock = 0; lock < max_cpu; ++lock ) {
        if( dcc_lock_host_slot( fname, ( lock + lock_offset ) % max_cpu, false ))
            return true;
    }
    // If not, block on the first selected one.
    return dcc_lock_host_slot( fname, lock_offset % max_cpu, true );
}

bool dcc_lock_host_slot(string fname, int lock, bool block)
{
    if( lock > 0 ) { // 1st keep without the 0 for backwards compatibility
        char num[ 20 ];
        sprintf( num, "%d", lock );
        fname += num;
    }

    int fd = 0;
    if (!dcc_open_lockfile(fname, fd)) {
        return false;
    }

    if (sys_lock(fd, block) == 0) {
        lock_fd = fd;
        return true;
    }

    switch (errno) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EAGAIN:
    case EACCES: /* HP-UX and Cygwin give this for exclusion */
        if( block )
            trace() << fname << " is busy" << endl;
        break;
    default:
        log_error() << "lock " << fname << " failed: " << strerror(errno) << endl;
        break;
    }

    if ((-1 == ::close(fd)) && (errno != EBADF)){
        log_perror("close failed");
    }
    return false;
}

bool color_output_possible()
{
    const char* term_env = getenv("TERM");

    return isatty(2) && term_env && strcasecmp(term_env, "DUMB");
}

bool compiler_has_color_output(const CompileJob &job)
{
    if (!color_output_possible())
        return false;

    // Clang has coloring.
    if (compiler_is_clang(job)) {
        return true;
    }
    if (const char* icecc_color_diagnostics = getenv("ICECC_COLOR_DIAGNOSTICS")) {
        return *icecc_color_diagnostics == '1';
    }
#ifdef HAVE_GCC_COLOR_DIAGNOSTICS
    return true;
#endif
    // GCC has it since 4.9, but that'd require detecting what GCC
    // version is used for the actual compile. However it requires
    // also GCC_COLORS to be set (and not empty), so use that
    // for detecting if GCC would use colors.
    if (const char *gcc_colors = getenv("GCC_COLORS")) {
        return (*gcc_colors != '\0');
    }
    return false;
}

// Whether icecream should add colors to the compiler output.
bool colorify_wanted(const CompileJob &job)
{
    if (compiler_has_color_output(job)) {
        return false; // -fcolor-diagnostics handling lets the compiler do it itself
    }
    if (explicit_color_diagnostics) { // colors explicitly enabled/disabled by an option
        return false;
    }
    if (getenv("ICECC_COLOR_DIAGNOSTICS") != nullptr)
        return false; // if set explicitly, assume icecream's colorify is not wanted

    if (getenv("EMACS")) {
        return false;
    }

    return color_output_possible();
}

void colorify_output(const string &_s_ccout)
{
    string s_ccout(_s_ccout);
    string::size_type end;

    while ((end = s_ccout.find('\n')) !=  string::npos) {

        string cline = s_ccout.substr(string::size_type(0), end);
        s_ccout = s_ccout.substr(end + 1);

        if (cline.find(": error:") != string::npos) {
            fprintf(stderr, "\x1b[1;31m%s\x1b[0m\n", cline.c_str());
        } else if (cline.find(": warning:") != string::npos) {
            fprintf(stderr, "\x1b[36m%s\x1b[0m\n", cline.c_str());
        } else {
            fprintf(stderr, "%s\n", cline.c_str());
        }
    }

    fprintf(stderr, "%s", s_ccout.c_str());
}


bool ignore_unverified()
{
    return getenv("ICECC_IGNORE_UNVERIFIED");
}

// GCC4.8+ has -fdiagnostics-show-caret, but when it prints the source code,
// it tries to find the source file on the disk, rather than printing the input
// it got like Clang does. This means that when compiling remotely, it of course
// won't find the source file in the remote chroot, and will disable the caret
// silently. As a workaround, make it possible to recompile locally if there's
// any stdout/stderr.
// Another way of handling this might be to send all the headers to the remote
// host, but this has been already tried in the sendheaders branch (for
// preprocessing remotely too) and performance-wise it just doesn't seem to
// be worth it.
bool output_needs_workaround(const CompileJob &job)
{
    if (compiler_is_clang(job))
        return false;
    if (explicit_no_show_caret)
        return false;
    if (const char* caret_workaround = getenv("ICECC_CARET_WORKAROUND"))
        return *caret_workaround == '1';
#ifdef HAVE_GCC_SHOW_CARET
    return true;
#endif
    return false;
}

int resolve_link(const std::string &file, std::string &resolved)
{
    char buf[PATH_MAX];
    buf[PATH_MAX - 1] = '\0';
    const int ret = readlink(file.c_str(), buf, sizeof(buf) - 1);
    const int errno_save = errno;

    if (ret <= 0) {
        return errno_save;
    }

    buf[ret] = 0;
    resolved = std::string(buf);
    return 0;
}

std::string get_cwd()
{
    static std::vector<char> buffer(1024);

    errno = 0;
    while (getcwd(&buffer[0], buffer.size() - 1) == nullptr && errno == ERANGE) {
        buffer.resize(buffer.size() + 1024);
        errno = 0;
    }
    if (errno != 0)
        return std::string();

    return string(&buffer[0]);
}
