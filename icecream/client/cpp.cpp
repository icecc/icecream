/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
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

/**
 * @file
 *
 * Run the preprocessor.  Client-side only.
 **/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "distcc.h"
#include "logging.h"
#include "exitcode.h"
#include "util.h"
#include "implicit.h"
#include "exec.h"
#include "tempfile.h"
#include "cpp.h"
#include "filename.h"
#include "arg.h"

using namespace std;

/**
 * If the input filename is a plain source file rather than a
 * preprocessed source file, then preprocess it to a temporary file
 * and return the name in @p cpp_fname.
 *
 * The preprocessor may still be running when we return; you have to
 * wait for @p cpp_fid to exit before the output is complete.  This
 * allows us to overlap opening the TCP socket, which probably doesn't
 * use many cycles, with running the preprocessor.
 **/
int dcc_cpp_maybe(char **argv, char *input_fname, int fd, pid_t *cpp_pid)
{
#warning TODO
#if 0
    char **cpp_argv;
    int ret;
    char *input_exten;
    const char *output_exten;
    pid_t pid;

    *cpp_pid = 0;

    if (dcc_is_preprocessed(input_fname)) {
        /* TODO: Perhaps also consider the option that says not to use cpp.
         * Would anyone do that? */
        trace() << "input is already preprocessed" << endl;

        /* already preprocessed, great. */
        /* TODO: write the file to the fd (fork cat?) */
        assert(0);
        return 0;
    }

    input_exten = dcc_find_extension(input_fname);
    output_exten = dcc_preproc_exten(input_exten);

    /* COOLO    if ((ret = dcc_make_tmpnam("distcc", output_exten, cpp_fname)))
        return ret; */

    /* We strip the -o option and allow cpp to write to stdout, which is
     * caught in a file.  Sun cc doesn't understand -E -o, and gcc screws up
     * -MD -E -o.
     *
     * There is still a problem here with -MD -E -o, gcc writes dependencies
     * to a file determined by the source filename.  We could fix it by
     * generating a -MF option, but that would break compilation with older
     * versions of gcc.  This is only a problem for people who have the source
     * and objects in different directories, and who don't specify -MF.  They
     * can fix it by specifying -MF.  */

    if ((ret = dcc_strip_dasho(argv, &cpp_argv))
        || (ret = dcc_set_action_opt(cpp_argv, "-E")))
        return ret;

    pid = fork();
    if (pid == -1) {
        log_error() << "failed to fork: " << strerror(errno) << endl;
        return EXIT_OUT_OF_MEMORY; /* probably */
    } else if (pid == 0) {
        if ((ret = dcc_ignore_sigpipe(0)))    /* set handler back to default */
            exit(ret);

        /* Ignore failure */
        dcc_increment_safeguard();
        close(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);

        dcc_execvp(cpp_argv);
        /* !! NEVER RETURN FROM HERE !! */
        return 0;
    } else {
        *cpp_pid = pid;
        trace() << "child started as pid" << (int) pid << endl;
        return 0;
    }
#endif
}


