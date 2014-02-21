/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
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

#include <string>

class CompileJob;

/* util.c */
extern int set_cloexec_flag(int desc, int value);
extern int dcc_ignore_sigpipe(int val);

extern std::string find_basename(const std::string &sfile);
extern std::string find_prefix(const std::string &basename);
extern void colorify_output(const std::string &s_ccout);
extern bool colorify_wanted(const CompileJob &job);
extern bool compiler_has_color_output(const CompileJob &job);
extern bool output_needs_workaround(const CompileJob &job);
extern bool ignore_unverified();
extern int resolve_link(const std::string &file, std::string &resolved);

extern bool dcc_unlock(int lock_fd);
extern bool dcc_lock_host(int &lock_fd);
