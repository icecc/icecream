/*
    This file is part of Icecream.

    Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
                  2004 Stephan Kulow <coolo@suse.de>

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

#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <job.h>
#include <comm.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "exitcode.h"
#include "logging.h"
#include "util.h"

class MsgChannel;

/* in remote.cpp */
std::string get_absfilename( const std::string &_file );

/* In arg.cpp.  */
bool analyse_argv (const char * const *argv,
                   CompileJob &job);

/* In cpp.cpp.  */
pid_t call_cpp (CompileJob &job, int fdwrite, int fdread = -1);

/* In local.cpp.  */
int build_local (CompileJob& job, struct rusage *used = 0);
std::string find_compiler( const std::string &compiler );
std::string get_compiler_name( const CompileJob &job );

/* In remote.cpp - permill is the probability it will be compiled three times */
int build_remote (CompileJob &job, MsgChannel *scheduler, const Environments &envs, int permill);

/* safeguard.cpp */
void dcc_increment_safeguard(void);
int dcc_recursion_safeguard(void);

Environments parse_icecc_version( const std::string &target );

#endif
