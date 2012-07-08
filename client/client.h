/*
    This file is part of icecc.

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

extern std::string remote_daemon;

/* in remote.cpp */
extern std::string get_absfilename( const std::string &_file );

/* In arg.cpp.  */
extern bool analyse_argv (const char * const *argv, CompileJob &job, bool icerun);

/* In cpp.cpp.  */
extern pid_t call_cpp (CompileJob &job, int fdwrite, int fdread = -1);

/* In local.cpp.  */
extern int build_local (CompileJob& job, MsgChannel *daemon, struct rusage *usage =0);
extern std::string find_compiler( const CompileJob& job );
extern bool compiler_is_clang( const CompileJob& job );
extern bool compiler_only_rewrite_includes( const CompileJob& job );
extern std::string compiler_path_lookup(const std::string& compiler);

/* In remote.cpp - permill is the probability it will be compiled three times */
extern int build_remote (CompileJob &job, MsgChannel *scheduler, const Environments &envs, int permill);

/* safeguard.cpp */
extern void dcc_increment_safeguard(void);
extern int dcc_recursion_safeguard(void);

extern Environments parse_icecc_version( const std::string &target );

#endif
