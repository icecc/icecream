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
pid_t call_cpp(CompileJob &job, int fd)
{
	printf("call_cpp\n");
#if 0
    if ( dcc_is_preprocessed( job.inputFile() ) ) {
        /* TODO: Perhaps also consider the option that says not to use cpp.
         * Would anyone do that? */
        trace() << "input is already preprocessed" << endl;

        /* already preprocessed, great. */
        /* TODO: write the file to the fd (fork cat?) */
        assert(0);
        return 0;
    }
#endif

    /* COOLO    if ((ret = dcc_make_tmpnam("distcc", output_exten, cpp_fname)))
        return ret; */

    pid_t pid = fork();
    if (pid == -1) {
        log_error() << "failed to fork: " << strerror(errno) << endl;
        return -1; /* probably */
    } else if (pid == 0) {
        int ret = dcc_ignore_sigpipe(0);
        if (ret)    /* set handler back to default */
            exit(ret);

        CompileJob::ArgumentsList list = job.localFlags();
        appendList( list, job.restFlags() );
        int argc = list.size();
        argc++; // the program
        argc += 2; // -E file.i
        char **argv = new char*[argc + 1];
        if (job.language() == CompileJob::Lang_C)
            argv[0] = strdup( "/opt/teambuilder/bin/gcc" );
        else if (job.language() == CompileJob::Lang_CXX)
            argv[0] = strdup( "/opt/teambuilder/bin/g++" );
        else
            assert(0);
        int i = 1;
        for ( CompileJob::ArgumentsList::const_iterator it = list.begin();
              it != list.end(); ++it) {
            argv[i++] = strdup( it->c_str() );
        }
        argv[i++] = strdup( "-E" );
        argv[i++] = strdup( job.inputFile().c_str() );
        argv[i++] = 0;

        printf( "forking " );
        for ( int index = 0; argv[index]; index++ )
            printf( "%s ", argv[index] );
        printf( "\n" );

        /* Ignore failure */
        close(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);

        execvp(argv[0], argv);
        /* !! NEVER RETURN FROM HERE !! */
        return 0;
    } else {
        trace() << "child started as pid" << (int) pid << endl;
        return pid;
    }

}


