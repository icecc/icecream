#ifndef _LOGGING_H
#define _LOGGING_H

#include <string>
#include <iostream>
#include <cassert>

enum DebugLevels { Info = 1, Warning = 2, Error = 4, Debug = 8};
extern std::ostream *logfile_info;
extern std::ostream *logfile_warning;
extern std::ostream *logfile_error;
extern std::ostream *logfile_trace;

void setup_debug(int level, const std::string &logfile = "");

static inline std::ostream& log_info() {
    assert( logfile_info );
    return *logfile_info;
}

static inline std::ostream& log_warning() {
    assert( logfile_warning );
    return *logfile_warning;
}

static inline std::ostream& log_error() {
    assert( logfile_error );
    return *logfile_error;
}

static inline std::ostream& trace() {
    assert( logfile_trace );
    return *logfile_trace;
}

#endif
