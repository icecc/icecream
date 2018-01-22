/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2003 by Martin Pool <mbp@samba.org>
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

/* Thanks to Dimitri PAPADOPOULOS-ORFANOS for researching many of the methods
 * in this file. */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ncpus.h"
#include "exitcode.h"

/**
 * Determine number of processors online.
 *
 * We will in the future use this to gauge how many concurrent tasks
 * should run on this machine.  Obviously this is only very rough: the
 * correct number needs to take into account disk buffers, IO
 * bandwidth, other tasks, etc.
**/

#if defined(__hpux__) || defined(__hpux)

#include <sys/param.h>
#include <sys/pstat.h>

int dcc_ncpus(int *ncpus)
{
    struct pst_dynamic psd;

    if (pstat_getdynamic(&psd, sizeof(psd), 1, 0) != -1) {
        *ncpus = psd.psd_proc_cnt;
        return 0;
    }

    rs_log_error("pstat_getdynamic failed: %s", strerror(errno));
    *ncpus = 1;
    return EXIT_DISTCC_FAILED;
}


#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__) || defined(__bsdi__) || defined(__DragonFly__)

/* http://www.FreeBSD.org/cgi/man.cgi?query=sysctl&sektion=3&manpath=FreeBSD+4.6-stable
   http://www.openbsd.org/cgi-bin/man.cgi?query=sysctl&sektion=3&manpath=OpenBSD+Current
   http://www.tac.eu.org/cgi-bin/man-cgi?sysctl+3+NetBSD-current
*/

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#undef HAVE_RS_LOG_ERROR
#else
#define HAVE_RS_LOG_ERROR
#endif

int dcc_ncpus(int *ncpus)
{
    int mib[2];
    size_t len = sizeof(*ncpus);
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;

    if (sysctl(mib, 2, ncpus, &len, NULL, 0) == 0) {
        return 0;
    }

#ifdef have_rs_log_error
    rs_log_error("sysctl(CTL_HW:HW_NCPU) failed: %s",
                 strerror(errno));
#else
    fprintf(stderr, "sysctl(CTL_HW:HW_NCPU) failed: %s", strerror(errno));
#endif
    *ncpus = 1;
    return EXIT_DISTCC_FAILED;
}

#else /* every other system */

/*
  http://www.opengroup.org/onlinepubs/007904975/functions/sysconf.html
  http://docs.sun.com/?p=/doc/816-0213/6m6ne38dd&a=view
  http://www.tru64unix.compaq.com/docs/base_doc/DOCUMENTATION/V40G_HTML/MAN/MAN3/0629____.HTM
  http://techpubs.sgi.com/library/tpl/cgi-bin/getdoc.cgi?coll=0650&db=man&fname=/usr/share/catman/p_man/cat3c/sysconf.z
*/

int dcc_ncpus(int *ncpus)
{
#if defined(_SC_NPROCESSORS_ONLN)
    /* Linux, Solaris, Tru64, UnixWare 7, and Open UNIX 8  */
    *ncpus = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROC_ONLN)
    /* IRIX */
    *ncpus = sysconf(_SC_NPROC_ONLN);
#else
#warning "Please port this function"
    *ncpus = -1;                /* unknown */
#endif

    if (*ncpus == -1) {
        *ncpus = 1;
        return EXIT_DISTCC_FAILED;
    }

    if (*ncpus == 0) {
        /* if there are no cpus, what are we running on?  But it has
         * apparently been observed to happen on ARM Linux
         */
        *ncpus = 1;
    }

    return 0;
}
#endif
