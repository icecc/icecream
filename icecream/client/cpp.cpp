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
#include <errno.h>
#include <assert.h>

#include "client.h"

using namespace std;

bool dcc_is_preprocessed(const string& sfile)
{
    if( sfile.size() < 3 )
        return false;
    int last = sfile.size() - 1;
    if( sfile[last-1] == '.' && sfile[last] == 'i' )
        return true; // .i
    if( sfile[last-2] == '.' && sfile[last-1] == 'i' && sfile[last] == 'i' )
        return true; // .ii
    return false;
}

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
    if ( dcc_is_preprocessed( job.inputFile() ) ) {
        /* TODO: Perhaps also consider the option that says not to use cpp.
         * Would anyone do that? */
#if 0
        trace() << "input is already preprocessed" << endl;
#endif
        /* already preprocessed, great. */
        /* write the file to the fd (fork cat) */
        pid_t pid = fork();
        if (pid == -1) {
            log_error() << "failed to fork: " << strerror(errno) << endl;
            return -1; /* probably */
        } else if (pid == 0) {
            int ret = dcc_ignore_sigpipe(0);
            if (ret)    /* set handler back to default */
                exit(ret);
    
            char **argv = new char*[2+1];
            argv[0] = strdup( "/bin/cat" );
            argv[1] = strdup( job.inputFile().c_str() );
            argv[2] = 0;

            /* Ignore failure */
            close(STDOUT_FILENO);
            dup2(fd, STDOUT_FILENO);

            execvp(argv[0], argv);
            /* !! NEVER RETURN FROM HERE !! */
            return 0;
        } else
            return pid;
    }

    pid_t pid = fork();
    if (pid == -1) {
        log_error() << "failed to fork: " << strerror(errno) << endl;
        return -1; /* probably */
    } else if (pid == 0) {
        int ret = dcc_ignore_sigpipe(0);
        if (ret)    /* set handler back to default */
            exit(ret);

        list<string> flags = job.localFlags();
        appendList( flags, job.restFlags() );
        int argc = flags.size();
        argc++; // the program
        argc += 2; // -E file.i
        char **argv = new char*[argc + 1];
        if (job.language() == CompileJob::Lang_C)
            argv[0] = strdup( "/usr/bin/gcc" );
        else if (job.language() == CompileJob::Lang_CXX)
            argv[0] = strdup( "/usr/bin/g++" );
        else
            assert(0);
        int i = 1;
        for ( list<string>::const_iterator it = flags.begin();
              it != flags.end(); ++it) {
            argv[i++] = strdup( it->c_str() );
        }
        argv[i++] = strdup( "-E" );
        argv[i++] = strdup( job.inputFile().c_str() );
        argv[i++] = 0;

#if 0
        printf( "forking " );
        for ( int index = 0; argv[index]; index++ )
            printf( "%s ", argv[index] );
        printf( "\n" );
#endif

        /* Ignore failure */
        close(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);

        execvp(argv[0], argv);
        /* !! NEVER RETURN FROM HERE !! */
        return 0;
    } else
        return pid;
}
