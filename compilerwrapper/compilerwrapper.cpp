/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
Copyright (C) 2012 Lubos Lunak <l.lunak@suse.cz>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


/*
Older icecream versions assume the compiler is always GCC. This can
be fixed on the local side, but remote nodes would need icecream upgrade.
As a workaround icecc-create-env includes this wrapper binary in the environment
if clang is to be used as well, that will either call clang or the real gcc.
Which one depends on an extra argument added by icecream.
*/

#include <sstream>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

//#define DEBUG

int main(int argc, char *argv[])
{
    bool iscxx = false;
    int argv0len = strlen(argv[0]);

    if (argv0len > 2 && argv[0][argv0len - 1] == '+' && argv[0][argv0len - 2] == '+') {
        iscxx = true;
    }

#ifdef DEBUG
    fprintf(stderr, "Args1:\n");

    for (int i = 0; i < argc; ++i) {
        fprintf(stderr, "%s\n", argv[i]);
    }

    fprintf(stderr, "\n");
#endif
    bool isclang = argc >= 2 && strcmp(argv[1], "clang") == 0;   // the extra argument from icecream
    bool haveclangarg = isclang;
    // 1 extra for -no-canonical-prefixes
    char **args = new char*[argc + 2];
    args[0] = new char[strlen(argv[0]) + 20];
    strcpy(args[0], argv[0]);
    char *separator = strrchr(args[0], '/');

    const auto resolve_compiler = [&] {
        if (separator == nullptr) {
            args[0][0] = '\0';
        } else {
            separator[1] = '\0';    // after the separator
        }

        if (isclang) {
            strcat(args[0], "clang");
        } else if (iscxx) {
            strcat(args[0], "g++.bin");
        } else {
            strcat(args[0], "gcc.bin");
        }
    };

    resolve_compiler();
    if (!isclang && access(args[0], X_OK)) {  // tarball built with clang and no explicit wrapper (like with icecc --build-native /bin/cc)
        isclang = true;
        resolve_compiler();
    }

    int pos = 1;

    if (isclang) {
        args[pos++] = (char *)"-no-canonical-prefixes";   // otherwise clang tries to access /proc/self/exe
        // clang wants the -x argument early, otherwise it seems to ignore it
        // (and treats the file as already preprocessed)
        int x_arg_pos = -1;

        for (int i = 1 + haveclangarg; i < argc; ++i) {
            if (strcmp(argv[i], "-x") == 0 && i + 1 < argc
                    && (strcmp(argv[i + 1], "c") == 0 || strcmp(argv[i + 1], "c++") == 0)) {
                x_arg_pos = i;
                args[pos++] = (char *)"-x";
                args[pos++] = argv[i + 1];
                break;
            }
        }

        for (int i = 1 + haveclangarg; i < argc; ++i) {
            // strip options that icecream adds but clang doesn't know or need
            if (strcmp(argv[i], "-fpreprocessed") == 0) {
                continue;    // clang doesn't know this (it presumably needs to always preprocess anyway)
            }

            if (strcmp(argv[i], "--param") == 0 && i + 1 < argc) {
                if (strncmp(argv[i + 1], "ggc-min-expand=", strlen("ggc-min-expand=")) == 0
                        || strncmp(argv[i + 1], "ggc-min-heapsize=", strlen("ggc-min-heapsize=")) == 0) {
                    // drop --param and the parameter itself
                    ++i;
                    continue;
                }
            }

            if (i == x_arg_pos) {
                ++i; // skip following
                continue; // and skip this one
            }

            args[pos++] = argv[i];
        }
    } else { // !isclang , just copy the arguments
        for (int i = 1; i < argc; ++i) {
            args[pos++] = argv[i];
        }
    }

    args[pos++] = nullptr;
    assert(pos <= argc + 2);
#ifdef DEBUG
    fprintf(stderr, "Args2:\n");

    for (int i = 0; i < pos; ++i) {
        fprintf(stderr, "%s\n", args[i]);
    }

    fprintf(stderr, "\n");
#endif
    execv(args[0], args);
    std::ostringstream errmsg;
    errmsg << "execv " << args[0] << " failed";
    perror(errmsg.str().c_str());
    exit(1);
}
