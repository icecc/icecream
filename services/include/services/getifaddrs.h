/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/* ifaddrs.h -- declarations for getting network interface addresses
   Copyright (C) 2002 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef GETIFADDRS_H
#define GETIFADDRS_H

/**
 * 02-12-26, tim@tjansen.de: added kde_ prefix, fallback-code,
 *                           removed glibs dependencies
 */


#include <string>

#include <sys/types.h>

#include <sys/socket.h>
#include <net/if.h>

#ifndef IFF_POINTOPOINT
#   define IFF_POINTOPOINT 0x10
#endif

#include <ifaddrs.h>

#define kde_getifaddrs(a) getifaddrs(a)
#define kde_freeifaddrs(a) freeifaddrs(a)
#define kde_ifaddrs ifaddrs



/**
 * Constructs an IPv4 socket address for a given port and network interface.
 *
 * The address is suitable for use by a subsequent call to bind().
 * If the interface argument is an empty string, the socket will listen on all interfaces.
 */
bool build_address_for_interface(struct sockaddr_in &myaddr, const std::string &interface, int port);

#endif
