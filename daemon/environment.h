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

class MsgChannel;
bool cleanup_cache( const std::string &basedir, uid_t nobody_uid );
size_t setup_env_cache(const std::string &basedir,
                     std::string &native_environment, uid_t nobody_uid);
Environments available_environmnents(const std::string &basename);
size_t install_environment( const std::string &basename,
                            const std::string &target,
                            const std::string &name,
                            MsgChannel *c, uid_t nobody_uid );
size_t remove_environment( const std::string &basedir, const std::string &env, uid_t nobody_uid );

#endif
