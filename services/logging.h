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
#include <errno.h>
#include <string>
#include <iostream>
#include <cassert>
#include <sys/time.h>

enum DebugLevels { Info = 1, Warning = 2, Error = 4, Debug = 8};
extern std::ostream *logfile_info;
extern std::ostream *logfile_warning;
extern std::ostream *logfile_error;
extern std::ostream *logfile_trace;

void setup_debug(int level, const std::string &logfile = "");
void reset_debug(int);

static inline std::ostream & output_date( std::ostream &os )
{
    time_t t = time( 0 );
    char *buf = ctime( &t );
    buf[strlen( buf )-1] = 0;
    os << "[" << buf << "] ";
    return os;
}

inline std::ostream & output_mdate( std::ostream &os )
{
   static struct timeval tps;
   static bool inited = false;
   if (!inited) 
   {
	gettimeofday(&tps, 0);
	inited = true;
   }	
   
   struct timeval tp;
   gettimeofday(&tp, 0);
   os << "[" << tp.tv_sec << ":" << tp.tv_usec << "(" << (tp.tv_sec - tps.tv_sec) * 1000 + (tp.tv_usec - tps.tv_usec  + 500 ) / 1000 << ")] ";
   tps = tp;
   return os;
}

static inline std::ostream& log_info() {
    assert( logfile_info );
    return *logfile_info;
}

static inline std::ostream& log_warning() {
    assert( logfile_warning );
    return output_date( *logfile_warning );
}

static inline std::ostream& log_error() {
    assert( logfile_error );
    return output_date( *logfile_error );
}

static inline std::ostream& trace() {
    assert( logfile_trace );
    return *logfile_trace;
}

std::string get_backtrace();
static inline void log_perror(const char *prefix) {
    int tmp_errno = errno;
    log_error() << prefix << " " << strerror( tmp_errno ) << std::endl;
}

class log_block 
{
    static uint nesting;
    timeval m_start;
    char* m_label;
public:
    log_block(const char* label =0)
    {
#ifndef NDEBUG
        for (unsigned i = 0; i < nesting; ++i) 
            log_info() << "  "; 

        log_info() << "  <" << (label ? label : "") << ">\n";

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
        log_info() << "  </" << m_label << ": "
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
