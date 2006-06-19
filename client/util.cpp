/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78; -*-
 *
 * distcc -- A simple distributed compiler system
 *
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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include <sys/types.h>
#include <pwd.h>

#include <sys/stat.h>
#include <sys/file.h>

#include "exitcode.h"
#include "logging.h"
#include "util.h"

using namespace std;

/**
 * Set the `FD_CLOEXEC' flag of DESC if VALUE is nonzero,
 * or clear the flag if VALUE is 0.
 *
 * From the GNU C Library examples.
 *
 * @returns 0 on success, or -1 on error with `errno' set.
 **/
int set_cloexec_flag (int desc, int value)
{
    int oldflags = fcntl (desc, F_GETFD, 0);
    /* If reading the flags failed, return error indication now. */
    if (oldflags < 0)
        return oldflags;
    /* Set just the flag we want to set. */
    if (value != 0)
        oldflags |= FD_CLOEXEC;
    else
        oldflags &= ~FD_CLOEXEC;
    /* Store modified flag word in the descriptor. */
    return fcntl (desc, F_SETFD, oldflags);
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
        log_warning() << "signal(SIGPIPE, " << ( val ? "ignore" : "default" ) << ") failed: "
                      << strerror(errno) << endl;
        return EXIT_DISTCC_FAILED;
    }
    return 0;
}

/**
 * Return a pointer to the basename of the file (everything after the
 * last slash.)  If there is no slash, return the whole filename,
 * which is presumably in the current directory.
 **/
string find_basename(const string &sfile)
{
    size_t index = sfile.find_last_of( '/' );
    if ( index == string::npos )
        return sfile;
    return sfile.substr( index + 1);
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


bool dcc_unlock(int lock_fd)
{
    /* All our current locks can just be closed */
    if (close(lock_fd)) {
        log_perror("close failed:");
        return false;
    }
    return true;
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
    plockfd = open(fname.c_str(), O_WRONLY|O_CREAT, 0666);
    if (plockfd == -1 && errno != EEXIST) {
        log_error() << "failed to creat " << fname << ": " << strerror(errno) << endl;
        return false;
    }

    return true;
}

bool dcc_lock_host(int &lock_fd)
{
    string fname = "/tmp/.icecream-";
    struct passwd *pwd = getpwuid( getuid() );
    if ( pwd )
        fname += pwd->pw_name;
    else {
        char buffer[10];
        sprintf( buffer, "%ld", ( long )getuid() );
        fname += buffer;
    }

    if ( mkdir( fname.c_str(), 0700 ) && errno != EEXIST ) {
        log_perror( "mkdir" );
        return false;
    }

    fname += "/local_lock";

    lock_fd = 0;
    if (!dcc_open_lockfile(fname, lock_fd) )
        return false;

    if (sys_lock(lock_fd, true) == 0) {
        return true;
    } else {
        switch (errno) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EAGAIN:
        case EACCES: /* HP-UX and Cygwin give this for exclusion */
            trace() << fname << " is busy" << endl;
            break;
        default:
            log_error() << "lock " << fname << " failed: " << strerror(errno) << endl;
            break;
        }

        ::close(lock_fd);
        return false;
    }
}

bool colorify_wanted()
{
  const char* term_env = getenv("TERM");

  return isatty(2) && !getenv("EMACS") && term_env && strcmp(term_env, "DUMB");
}

void colorify_output(const string& _s_ccout)
{
    string s_ccout(_s_ccout);
    string::size_type end;

    while ( (end = s_ccout.find('\n')) !=  string::npos ) {

        string cline = s_ccout.substr(string::size_type(0), end );
        s_ccout = s_ccout.substr(end+1);

        if (cline.find(": error:") != string::npos)
            fprintf(stderr, "\x1b[1;31m%s\x1b[0m\n", cline.c_str());
        else if (cline.find(": warning:") != string::npos)
            fprintf(stderr, "\x1b[34m%s\x1b[0m\n", cline.c_str());
        else
            fprintf(stderr, "%s\n", cline.c_str());
    }
    fprintf(stderr, s_ccout.c_str());
}


