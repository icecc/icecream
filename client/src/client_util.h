/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*-
 */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _CLIENT_UTIL_H_
#define _CLIENT_UTIL_H_

#include "services_util.h"

#include <string>

class CompileJob;

int
set_cloexec_flag(int desc, int value);
int
dcc_ignore_sigpipe(int val);

void
colorify_output(const std::string & s_ccout);
bool
colorify_wanted(const CompileJob & job);
bool
compiler_has_color_output(const CompileJob & job);
bool
output_needs_workaround(const CompileJob & job);
bool
ignore_unverified();
int
resolve_link(const std::string & file, std::string & resolved);

std::string
get_cwd();
std::string
get_absfilename(const std::string & _file);

bool
dcc_lock_host();
void
dcc_unlock();
int
dcc_locked_fd();

class HostUnlock {
public:
    ~HostUnlock()
    {
        dcc_unlock();
    }
};

#endif
