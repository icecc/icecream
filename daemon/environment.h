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

#ifndef _ENVIRONMENT_H
#define _ENVIRONMENT_H

#include <comm.h>
#include <list>
#include <string>
#include <unistd.h>

class MsgChannel;
extern bool cleanup_cache( const std::string &basedir );
extern size_t setup_env_cache(const std::string &basedir,
                     std::string &native_environment, uid_t nobody_uid, gid_t nobody_gid);
Environments available_environmnents(const std::string &basename);
extern bool native_env_uptodate();
extern pid_t start_install_environment( const std::string &basename,
                            const std::string &target,
                            const std::string &name,
                            MsgChannel *c, int& pipe_to_child,
                            FileChunkMsg*& fmsg,
                            uid_t nobody_uid, gid_t nobody_gid );
extern size_t finalize_install_environment( const std::string &basename, const std::string& target,
        pid_t pid, gid_t nobody_gid );
extern size_t remove_environment( const std::string &basedir, const std::string &env);
extern size_t remove_native_environment( const std::string &basedir, const std::string &env );
extern void chdir_to_environment( MsgChannel *c, const std::string &dirname, uid_t nobody_uid, gid_t nobody_gid );
extern bool verify_env( MsgChannel *c, const std::string &basedir, const std::string& target,
                        const std::string &env, uid_t nobody_uid, gid_t nobody_gid );

#endif
