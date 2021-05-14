/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/* getifaddrs -- get names and addresses of all network interfaces
   Copyright (C) 1999,2002 Free Software Foundation, Inc.
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

/**
 * 02-12-26, tim@tjansen.de: put in kde_ namespace, C++ fixes,
 *                           included ifreq.h
 *                           removed glibc dependencies
 */

#include "services/getifaddrs.h"
#include "services/logging.h"

#include <netinet/in.h>

bool build_address_for_interface(struct sockaddr_in &myaddr, const std::string &interface, int port)
{
    // Pre-fill the output parameter with the default address (port, INADDR_ANY)
    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons(port);
    myaddr.sin_addr.s_addr = htonl( INADDR_ANY );

    // If no interface was specified, return the default address
    if (interface.empty()) {
        return true;
    }
    // Explicit case for loopback.
    if (interface == "lo") {
        myaddr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        return true;
    }

    // Otherwise, search for the IP address of the given interface
    struct kde_ifaddrs *addrs;

    if (kde_getifaddrs(&addrs) < 0) {
        log_perror("kde_getifaddrs()");
        return false;
    }

    bool found = false;

    for (struct kde_ifaddrs *addr = addrs; addr != nullptr; addr = addr->ifa_next) {
        if (interface != addr->ifa_name) {
            continue;
        }
        if (addr->ifa_addr == nullptr || addr->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        myaddr.sin_addr.s_addr = reinterpret_cast<struct sockaddr_in *>(addr->ifa_addr)->sin_addr.s_addr;
        found = true;
        break;
    }

    kde_freeifaddrs(addrs);

    if (!found) {
        log_error() << "No IP address found for interface \"" << interface << "\"" << std::endl;
        return false;
    }

    return true;
}
