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
pid_t call_cpp(CompileJob &job, int fdwrite, int fdread)
{
    flush_debug();
    pid_t pid = fork();
    if (pid == -1) {
        log_perror("failed to fork:");
        return -1; /* probably */
    } else if (pid != 0) {
	/* Parent.  Close the write fd.  */
	if (fdwrite > -1)
	    close (fdwrite);
        return pid;
    } else {
	/* Child.  Close the read fd, in case we have one.  */
	if (fdread > -1)
	    close (fdread);
        int ret = dcc_ignore_sigpipe(0);
        if (ret)    /* set handler back to default */
            _exit(ret);

	char **argv;
	if ( dcc_is_preprocessed( job.inputFile() ) ) {
            /* already preprocessed, great.
               write the file to the fdwrite (using cat) */
            argv = new char*[2+1];
            argv[0] = strdup( "/bin/cat" );
            argv[1] = strdup( job.inputFile().c_str() );
            argv[2] = 0;
	} else {
	    list<string> flags = job.localFlags();
            /* This has a duplicate meaning. it can either include a file
               for preprocessing or a precompiled header. decide which one.  */
            for (list<string>::iterator it = flags.begin();
                 it != flags.end();) {
                if ((*it) == "-include") {
                    ++it;
                    if (it != flags.end()) {
                        std::string p = (*it);
                        if (access(p.c_str(), R_OK) && !access((p + ".gch").c_str(), R_OK)) {
                            list<string>::iterator o = --it;
                            it++;
                            flags.erase(o);
                            o = it++;
                            flags.erase(o);
                        }
                    }
                }
                else
                    ++it;
            }

	    appendList( flags, job.restFlags() );
	    int argc = flags.size();
	    argc++; // the program
	    argc += 2; // -E file.i
	    argc += 1; // -frewrite-includes
	    argv = new char*[argc + 1];
   	    argv[0] = strdup( find_compiler( job ).c_str() );
	    int i = 1;
	    for ( list<string>::const_iterator it = flags.begin();
		  it != flags.end(); ++it) {
		argv[i++] = strdup( it->c_str() );
	    }
	    argv[i++] = strdup( "-E" );
	    argv[i++] = strdup( job.inputFile().c_str() );
	    if ( compiler_only_rewrite_includes( job ))
	        argv[i++] = strdup( "-frewrite-includes" );
	    argv[i++] = 0;
	}

#if 0
        printf( "forking " );
        for ( int index = 0; argv[index]; index++ )
            printf( "%s ", argv[index] );
        printf( "\n" );
#endif

	if (fdwrite != STDOUT_FILENO) {
            /* Ignore failure */
            close(STDOUT_FILENO);
            dup2(fdwrite, STDOUT_FILENO);
	    close(fdwrite);
	}

        dcc_increment_safeguard();

        _exit(execv(argv[0], argv));
    }
}
