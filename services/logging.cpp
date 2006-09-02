/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include <config.h>
#include <iostream>
#include "logging.h"
#include <fstream>
#include <signal.h>

using namespace std;

int debug_level = 0;
ostream *logfile_trace = 0;
ostream *logfile_info = 0;
ostream *logfile_warning = 0;
ostream *logfile_error = 0;
string logfile_prefix;

static ofstream logfile_null( "/dev/null" );
static ofstream logfile_file;
static string logfile_filename;

void reset_debug( int );

void setup_debug(int level, const string &filename, const string& prefix)
{
    debug_level = level;
    logfile_prefix = prefix;
    logfile_filename = filename;

    if ( logfile_file.is_open() )
        logfile_file.close();
    ostream *output = 0;
    if ( filename.length() ) {
	logfile_file.clear();
        logfile_file.open( filename.c_str(), fstream::out | fstream::app );
        output = &logfile_file;
    } else
        output = &cerr;

    if ( debug_level & Debug )
        logfile_trace = output;
    else
        logfile_trace = &logfile_null;

    if ( debug_level & Info )
        logfile_info = output;
    else
        logfile_info = &logfile_null;

    if ( debug_level & Warning )
        logfile_warning = output;
    else
        logfile_warning = &logfile_null;

    if ( debug_level & Error )
        logfile_error = output;
    else
        logfile_error = &logfile_null;

    signal( SIGHUP, reset_debug );
}

void reset_debug( int )
{
    setup_debug(debug_level, logfile_filename);
}

void close_debug()
{
    if (logfile_null.is_open())
        logfile_null.close();
    if (logfile_file.is_open())
        logfile_file.close();

    logfile_trace = logfile_info = logfile_warning = logfile_error = 0;
}

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#endif

string get_backtrace() {
    string s;

#ifdef HAVE_BACKTRACE
    void* trace[256];
    int n = backtrace(trace, 256);
    if (!n)
        return s;
    char** strings = backtrace_symbols (trace, n);

    s = "[\n";

    for (int i = 0; i < n; ++i) {
        s += ": ";
        s += strings[i];
        s += "\n";
    }
    s += "]\n";
    if (strings)
        free (strings);
#endif

    return s;
}

unsigned log_block::nesting;
