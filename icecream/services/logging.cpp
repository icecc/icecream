#include <iostream>
#include "logging.h"
#include <fstream>

using namespace std;

int debug_level = 0;
ostream *logfile_trace = 0;
ostream *logfile_info = 0;
ostream *logfile_warning = 0;
ostream *logfile_error = 0;
ofstream logfile_null( "/dev/null" );
ofstream logfile_file;

void setup_debug(int level, const string &filename)
{
    debug_level = level;

    ostream *output = 0;
    if ( filename.length() ) {
        logfile_file.open( filename.c_str() );
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
}
