/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <string>

/* util.c */
extern int set_cloexec_flag (int desc, int value);
extern int dcc_ignore_sigpipe (int val);

extern std::string find_basename(const std::string &sfile);
extern void colorify_output(const std::string &s_ccout);
extern bool colorify_wanted(CompileJob::Language lang);
extern bool colorify_possible();

extern bool dcc_unlock(int lock_fd);
extern bool dcc_lock_host(int &lock_fd);
