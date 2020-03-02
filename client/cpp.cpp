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

bool dcc_is_preprocessed(const string &sfile)
{
    if (sfile.size() < 3) {
        return false;
    }

    int last = sfile.size() - 1;

    if ((sfile[last - 1] == '.') && (sfile[last] == 'i')) {
        return true;    // .i
    }

    if ((sfile[last - 2] == '.') && (sfile[last - 1] == 'i') && (sfile[last] == 'i')) {
        return true;    // .ii
    }

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
pid_t call_cpp(CompileJob &job, int fdWriteStdout, int fdReadStdout, int fdWriteStderr, int fdReadStderr)
{
    flush_debug();
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork:");
        return -1; /* probably */
    }

    if (pid != 0) {
        /* Parent.  Close the write fd.  */
        if (fdWriteStdout > -1) {
            if ((-1 == close(fdWriteStdout)) && (errno != EBADF)){
                log_perror("close() failed");
            }
        }

        if (fdWriteStderr > -1) {
            if ((-1 == close(fdWriteStderr)) && (errno != EBADF)){
                log_perror("close() failed");
            }
        }

        return pid;
    }

    /* Child.  Close the read fd, in case we have one.  */
    if (fdReadStdout > -1) {
        if ((-1 == close(fdReadStdout)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }

    if (fdReadStderr > -1) {
        if ((-1 == close(fdReadStderr)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }

    int ret = dcc_ignore_sigpipe(0);

    if (ret) {  /* set handler back to default */
        _exit(ret);
    }

    char **argv;

    if (dcc_is_preprocessed(job.inputFile())) {
        /* already preprocessed, great.
           write the file to the fdWriteStdout (using cat) */
        argv = new char*[2 + 1];
        argv[0] = strdup("/bin/cat");
        argv[1] = strdup(job.inputFile().c_str());
        argv[2] = 0;
    } else {
        list<string> flags = job.localFlags();
        appendList(flags, job.restFlags());

        for (list<string>::iterator it = flags.begin(); it != flags.end();) {
            /* This has a duplicate meaning. it can either include a file
               for preprocessing or a precompiled header. decide which one.  */
            if ((*it) == "-include") {
                ++it;

                if (it != flags.end()) {
                    std::string p = (*it);

                    if (access(p.c_str(), R_OK) < 0 && access((p + ".gch").c_str(), R_OK) == 0) {
                        // PCH is useless for preprocessing, ignore the flag.
                        list<string>::iterator o = --it;
                        it++;
                        flags.erase(o);
                        o = it++;
                        flags.erase(o);
                    }
                }
            } else if ((*it) == "-include-pch") {
                list<string>::iterator o = it;
                ++it;
                if (it != flags.end()) {
                    std::string p = (*it);
                    if (access(p.c_str(), R_OK) == 0) {
                        // PCH is useless for preprocessing (and probably slows things down), ignore the flag.
                        flags.erase(o);
                        o = it++;
                        flags.erase(o);
                    }
                }
            } else if ((*it) == "-fpch-preprocess") {
                // This would add #pragma GCC pch_preprocess to the preprocessed output, which would make
                // the remote GCC try to load the PCH directly and fail. Just drop it. This may cause a build
                // failure if the -include check above failed to detect usage of a PCH file (e.g. because
                // it needs to be found in one of the -I paths, which we don't check) and the header file
                // itself doesn't exist.
                flags.erase(it++);
            } else if ((*it) == "-fmodules" || (*it) == "-fcxx-modules" || (*it) == "-fmodules-ts"
                || (*it).find("-fmodules-cache-path=") == 0) {
                // Clang modules, handle like with PCH, remove the flags and compile remotely
                // without them.
                flags.erase(it++);
            } else {
                ++it;
            }
        }

        int argc = flags.size();
        argc++; // the program
        argc += 1; // -E
        argc += 1; // -frewrite-includes / -fdirectives-only
        argc += 3; // clang-cl
        argc += 1; // inputFile
        argv = new char*[argc + 1];
        argv[0] = strdup(find_compiler(job).c_str());
        int i = 1;

        for (list<string>::const_iterator it = flags.begin(); it != flags.end(); ++it) {
            argv[i++] = strdup(it->c_str());
        }

        argv[i++] = strdup("-E");

        if (compiler_only_rewrite_includes(job)) {
            if( compiler_is_clang(job)) {
                argv[i++] = strdup("-frewrite-includes");
            } else { // gcc
                argv[i++] = strdup("-fdirectives-only");
            }
        }

        if ( compiler_is_clang_cl( job ) )
        {
          argv[ i++ ] = strdup( "-Xclang" );
          argv[ i++ ] = strdup( "-fcxx-exceptions" ); // necessary for boost::throw_exception
          argv[ i++ ] = strdup( "--" ); // handle all following arguments as file
        }

        argv[i++] = strdup(job.inputFile().c_str());

        argv[i++] = 0;
    }

    string argstxt = argv[ 0 ];
    for( int i = 1; argv[ i ] != NULL; ++i ) {
        argstxt += ' ';
        argstxt += argv[ i ];
    }
    trace() << "preparing source to send: " << argstxt << endl;

    if (fdWriteStdout != STDOUT_FILENO) {
        /* Ignore failure */
        close(STDOUT_FILENO);
        dup2(fdWriteStdout, STDOUT_FILENO);
        close(fdWriteStdout);
    }

    if (fdWriteStderr > -1)
    {
      dup2( fdWriteStderr, STDERR_FILENO );
    }

    dcc_increment_safeguard(SafeguardStepCompiler);
    execv(argv[0], argv);
    int exitcode = ( errno == ENOENT ? 127 : 126 );
    ostringstream errmsg;
    errmsg << "execv " << argv[0] << " failed";
    log_perror(errmsg.str());
    _exit(exitcode);
}
