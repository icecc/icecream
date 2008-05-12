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

#ifndef _LOGGING_H
#define _LOGGING_H

#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <iostream>
#include <cassert>
#include <cstring>

enum DebugLevels { Info = 1, Warning = 2, Error = 4, Debug = 8};
extern std::ostream *logfile_info;
extern std::ostream *logfile_warning;
extern std::ostream *logfile_error;
extern std::ostream *logfile_trace;
extern std::string logfile_prefix;

void setup_debug(int level, const std::string &logfile = "", const std::string& prefix="");
void reset_debug(int);
void close_debug();
void flush_debug();

static inline std::ostream & output_date( std::ostream &os )
{
    time_t t = time( 0 );
    struct tm* tmp = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%T: ", tmp);
    if (logfile_prefix.size())
        os << logfile_prefix << "[" << getpid() << "] ";

    os << buf;
    return os;
}

static inline std::ostream& log_info() {
    if(!logfile_info) return std::cerr;
    return output_date( *logfile_info);
}

static inline std::ostream& log_warning() {
    if(!logfile_warning) return std::cerr;
    return output_date( *logfile_warning );
}


static inline std::ostream& log_error() {
    if(!logfile_error) return std::cerr;
    return output_date( *logfile_error );
}

static inline std::ostream& trace() {
    if(!logfile_trace) return std::cerr;
    return output_date( *logfile_trace);
}

std::string get_backtrace();
static inline void log_perror(const char *prefix) {
    int tmp_errno = errno;
    log_error() << prefix << " " << strerror( tmp_errno ) << std::endl;
}

class log_block 
{
    static unsigned nesting;
    timeval m_start;
    char* m_label;
public:
    log_block(const char* label =0)
    {
#ifndef NDEBUG
        for (unsigned i = 0; i < nesting; ++i) 
            log_info() << "  "; 

        log_info() << "<" << (label ? label : "") << ">\n";

        m_label = strdup(label ? label : "");
        ++nesting;
        gettimeofday( &m_start, 0);
#endif
    }

    ~log_block()
    {
#ifndef NDEBUG
        timeval end;
        gettimeofday (&end, 0);

        --nesting;
        for (unsigned i = 0; i < nesting; ++i) 
            log_info() << "  "; 
        log_info() << "</" << m_label << ": "
            << (end.tv_sec - m_start.tv_sec ) * 1000 + ( end.tv_usec - m_start.tv_usec ) / 1000 << "ms>\n";

        free(m_label);
#endif
    }
};

#include <sstream>
#include <iostream>

template<class T>
std::string toString(const T& val)
{
    std::ostringstream os;
    os << val;
    return os.str();
}

#endif
