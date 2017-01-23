/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>
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

#ifndef _WORKIT_H
#define _WORKIT_H

#include <job.h>
#include <sys/types.h>
#include <string>

#include <exception>

class MsgChannel;
class CompileResultMsg;

// No icecream ;(
class myexception : public std::exception
{
    int code;
public:
    myexception( int _exitcode ) : exception(), code( _exitcode ) {}
    int exitcode() const { return code; }
};

namespace JobStatistics {
    enum job_stat_fields { in_compressed, in_uncompressed, out_uncompressed, exit_code,
                           real_msec, user_msec, sys_msec, sys_pfaults  };
}

extern int work_it( CompileJob &j, unsigned int job_stats[], MsgChannel* client,
             CompileResultMsg& msg, const std::string &outfilename,
             unsigned long int mem_limit, int client_fd, int job_in_fd );

#endif
