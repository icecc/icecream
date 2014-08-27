/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

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

#include <config.h>
#include <iostream>
#include "logging.h"
#include <fstream>
#include <signal.h>
#ifdef __linux__
#include <dlfcn.h>
#endif

using namespace std;

int debug_level = 0;
ostream *logfile_trace = 0;
ostream *logfile_info = 0;
ostream *logfile_warning = 0;
ostream *logfile_error = 0;
string logfile_prefix;

static ofstream logfile_null("/dev/null");
static ofstream logfile_file;
static string logfile_filename;

void reset_debug(int);

void setup_debug(int level, const string &filename, const string &prefix)
{
    string fname = filename;
    debug_level = level;
    logfile_prefix = prefix;
    logfile_filename = filename;

    if (logfile_file.is_open()) {
        logfile_file.close();
    }

    ostream *output = 0;

    if (filename.length()) {
        logfile_file.clear();
        logfile_file.open(filename.c_str(), fstream::out | fstream::app);
#ifdef __linux__

        if (fname[0] != '/') {
            char buf[FILENAME_MAX];

            if (getcwd(buf, sizeof(buf))) {
                fname.insert(0, "/");
                fname.insert(0, buf);
            }
        }

        setenv("SEGFAULT_OUTPUT_NAME", fname.c_str(), false);
#endif
        output = &logfile_file;
    } else {
        output = &cerr;
    }

#ifdef __linux__
    (void) dlopen("libSegFault.so", RTLD_NOW | RTLD_LOCAL);
#endif

    if (debug_level & Debug) {
        logfile_trace = output;
    } else {
        logfile_trace = &logfile_null;
    }

    if (debug_level & Info) {
        logfile_info = output;
    } else {
        logfile_info = &logfile_null;
    }

    if (debug_level & Warning) {
        logfile_warning = output;
    } else {
        logfile_warning = &logfile_null;
    }

    if (debug_level & Error) {
        logfile_error = output;
    } else {
        logfile_error = &logfile_null;
    }

    signal(SIGHUP, reset_debug);
}

void reset_debug(int)
{
    setup_debug(debug_level, logfile_filename);
}

void close_debug()
{
    if (logfile_null.is_open()) {
        logfile_null.close();
    }

    if (logfile_file.is_open()) {
        logfile_file.close();
    }

    logfile_trace = logfile_info = logfile_warning = logfile_error = 0;
}

/* Flushes all ostreams used for debug messages.  You need to call
   this before forking.  */
void flush_debug()
{
    if (logfile_null.is_open()) {
        logfile_null.flush();
    }

    if (logfile_file.is_open()) {
        logfile_file.flush();
    }
}

unsigned log_block::nesting;
