/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
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

#include "logging.h"

using namespace std;

/**
 * @file
 * @brief Protect against unbounded recursion.
 *
 * It would be fairly easy for somebody to get confused in masquerade mode and
 * try to get distcc to invoke itself in a loop.  We can't always work out the
 * right thing to do but we can at least flag an error.
 *
 * This environment variable is set to guard against distcc accidentally
 * recursively invoking itself, thinking it's the real compiler.
 **/

static const char dcc_safeguard_name[] = "_ICECREAM_SAFEGUARD";
static char dcc_safeguard_set[] = "_ICECREAM_SAFEGUARD=1";
static int dcc_safeguard_level;

int dcc_recursion_safeguard(void)
{
    char *env = getenv(dcc_safeguard_name);

    if (env) {
        //trace() << "safeguard: " << env << endl;
        if (!(dcc_safeguard_level = atoi(env)))
            dcc_safeguard_level = 1;
    }
    else
        dcc_safeguard_level = 0;
    //trace() << "safeguard level=" << dcc_safeguard_level << endl;

    return dcc_safeguard_level;
}


void dcc_increment_safeguard(void)
{
    if (dcc_safeguard_level > 0)
        dcc_safeguard_set[sizeof dcc_safeguard_set-2] = dcc_safeguard_level+'1';
    //trace() << "setting safeguard: " << dcc_safeguard_set << endl;
    if ((putenv(strdup(dcc_safeguard_set)) == -1)) {
        log_error() << "putenv failed" << endl;
    }
}
