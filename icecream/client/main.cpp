/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
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


			/* 4: The noise of a multitude in the
			 * mountains, like as of a great people; a
			 * tumultuous noise of the kingdoms of nations
			 * gathered together: the LORD of hosts
			 * mustereth the host of the battle.
			 *		-- Isaiah 13 */



#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <cassert>

#include "logging.h"

#include "clinet.h"
#include "implicit.h"
#include "strip.h"
#include "cpp.h"
#include "client_comm.h"
#include "exec.h"
#include "arg.h"
#include "util.h"
#include "exitcode.h"
#include "filename.h"
#include "distcc.h"

/* Name of this program, for trace.c */
const char *const rs_program_name = "icecc";

using namespace std;

/**
 * @file
 *
 * Entry point for the distcc client.
 *
 * There are three methods of use for distcc: explicit (distcc gcc -c
 * foo.c), implicit (distcc -c foo.c) and masqueraded (gcc -c foo.c,
 * where gcc is really a link to distcc).
 *
 * Detecting these is relatively easy by examining the first one or
 * two words of the command.  We also need to make sure that when we
 * go to run the compiler, we run the one intended by the user.
 *
 * In particular, for masqueraded mode, we want to make sure that we
 * don't invoke distcc recursively.
 **/

static void dcc_show_usage(void)
{
    printf(
"Usage:\n"
"   distcc [COMPILER] [compile options] -o OBJECT -c SOURCE\n"
"   distcc --help\n"
"\n"
"Options:\n"
"   COMPILER                   defaults to \"cc\"\n"
"   --help                     explain usage and exit\n"
"   --version                  show version and exit\n"
"\n");
}


static void dcc_client_signalled (int whichsig)
{
#ifdef HAVE_STRSIGNAL
    log_info() << strsignal(whichsig) << endl;
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
 * This is called with a lock on localhost already held.
 **/
static int dcc_compile_local(char *argv[])
{
    pid_t pid;
    int ret;
    int status;
#warning TODO
#if 0
    /* We don't do any redirection of file descriptors when running locally,
     * so if for example cpp is being used in a pipeline we should be fine. */
    if ((ret = dcc_spawn_child(argv, &pid, NULL, NULL, NULL)) != 0)
        return ret;

    if ((ret = dcc_collect_child("cc", pid, &status)))
        return ret;
#endif
    return 0;
}

static int write_argv( int fd, char *argv[])
{
    size_t argc = 0;
    int ret;

    /* calculate total length */
    while (argv[argc]) argc++;

    printf("argc %d\n", argc);

    if ( ( ret = client_write_message( fd, C_ARGC, argc) ) != 0)
        return ret;

    for (argc = 0; argv[argc]; argc++) {
        ssize_t len = strlen(argv[argc]);
        if ( ( ret = client_write_message( fd, C_ARGV, len) ) != 0)
            return ret;
        if (write(fd, argv[argc], len) != len)
            return 1;
    }
    return 0;
}

/**
 * Execute the commands in argv remotely or locally as appropriate.
 *
 * We may need to run cpp locally; we can do that in the background
 * while trying to open a remote connection.
 *
 * This function is slightly inefficient when it falls back to running
 * gcc locally, because cpp may be run twice.  Perhaps we could adjust
 * the command line to pass in the .i file.  On the other hand, if
 * something has gone wrong, we should probably take the most
 * conservative course and run the command unaltered.  It should not
 * be a big performance problem because this should occur only rarely.
 *
 * @param argv Command to execute.  Does not include 0='distcc'.
 * Treated as read-only, because it is a pointer to the program's real
 * argv.
 *
 * @param status On return, contains the waitstatus of the compiler or
 * preprocessor.  This function can succeed (in running the compiler) even if
 * the compiler itself fails.  If either the compiler or preprocessor fails,
 * @p status is guaranteed to hold a failure value.
 **/
static int dcc_build_somewhere(char *argv[],
                               int sg_level,
                               int *status)
{
    char *input_fname = NULL, *output_fname;
    char **argv_stripped;
    pid_t cpp_pid = 0;
    int ret;
    int sockets[2];
    int out_fd = -1;
    char buffer[4096];
    unsigned char version = ICECC_PROTO_VERSION;
    struct Client_Message m;

    if (sg_level)
        goto run_local;

    /* TODO: Perhaps tidy up these gotos. */

    if (dcc_scan_args(argv, &input_fname, &output_fname, &argv) != 0) {
        /* we need to scan the arguments even if we already know it's
         * local, so that we can pick up distcc client options. */
        goto run_local;
    }

    if ((ret = dcc_strip_local_args(argv, &argv_stripped)))
        goto run_local;

    if ((ret = dcc_connect_by_name("localhost", 7000, &out_fd)) != 0)
        goto run_local;
    log_info() << "got connection: fd=" << out_fd << endl;

    if ((ret = client_write_message( out_fd, C_VERSION, version)) != 0) {
        log_info() << "write of version failed" << endl;
        goto run_local;
    }

    if ((ret = write_argv( out_fd, argv_stripped)) != 0) {
        log_info() << "write of arguments failed" << endl;
        goto run_local;
    }

    if (pipe(sockets)) {
        /* for all possible cases, this is something severe */
        exit(errno);
    }

    if ((ret = dcc_cpp_maybe(argv, input_fname, sockets[1], &cpp_pid)) != 0)
        goto run_local;
    close(sockets[1]);

    do {
        ssize_t bytes = read(sockets[0], buffer, sizeof(buffer));
        if (!bytes)
            break;
        log_info() << "read " << bytes << " bytes" << endl;
        if ((ret = client_write_message( out_fd, C_PREPROC, bytes)) != 0) {
            log_info() << "failed to write preproc header" << endl;
            goto run_local;
        }
        if (write(out_fd, buffer, bytes) != bytes) {
            log_info() << "write failed" << endl;
            close(sockets[0]);
            close(out_fd);
            goto run_local;
        }
    } while (1);

    if ((ret = client_write_message( out_fd, C_DONE, 0)) != 0)
        goto run_local; // very sad :/


    client_read_message( out_fd, &m );
    if ( m.type != C_STATUS) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }
    *status = m.length;

    client_read_message( out_fd, &m );
    if ( m.type != C_STDOUT) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }

    assert(m.length < 4096); // for now :/
    read(out_fd, buffer, m.length);
    buffer[m.length] = 0;
    fprintf(stdout, "%s", buffer);

    client_read_message( out_fd, &m );
    if ( m.type != C_STDERR) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }

    assert(m.length < 4096); // for now :/
    read(out_fd, buffer, m.length);
    buffer[m.length] = 0;
    fprintf(stderr, "%s", buffer);

    return 0;

  run_local:
    log_info() << "going to run local" << endl;
    return dcc_compile_local(argv);
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
    int status, sg_level, tweaked_path = 0;
    char **compiler_args;
    const char *compiler_name;
    int ret;

    dcc_client_catch_signals();

    compiler_name = dcc_find_basename(argv[0]);

    /* Ignore SIGPIPE; we consistently check error codes and will
     * see the EPIPE. */
    dcc_ignore_sigpipe(1);

    sg_level = dcc_recursion_safeguard();

    if (strncmp(compiler_name, rs_program_name, strlen(rs_program_name)) == 0) {
        /* Either "icecc(++) -c hello.c" or "distcc gcc|g++ -c hello.c" */
        if (argc <= 1 || !strcmp(argv[1], "--help")) {
            dcc_show_usage();
            ret = 0;
            goto out;
        }
        if (!strcmp(argv[1], "--version")) {
            //            dcc_show_version("distcc");
            ret = 0;
            goto out;
        }
        dcc_find_compiler(argv, &compiler_args);
        if ((ret = dcc_trim_path(compiler_name)) != 0)
            goto out;
    } else {
        /* Invoked as "cc -c hello.c", with masqueraded path */
        if ((ret = dcc_support_masquerade(argv, compiler_name,
                                          &tweaked_path)) != 0)
            goto out;

        // TODO dcc_shallowcopy_argv(argv, &compiler_args, 0);
        compiler_args[0] = strdup(compiler_name);
    }

    if (sg_level - tweaked_path > 0) {
        log_crit() << "distcc seems to have invoked itself recursively!" << endl;
        ret = EXIT_RECURSION;
        goto out;
    }

    ret = dcc_build_somewhere(compiler_args, sg_level, &status);
    log_info() << "got status: " << status << endl;

    out:
    exit(ret);
}
