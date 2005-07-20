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


                /* "More computing sins are committed in the name of
                 * efficiency (without necessarily achieving it) than
                 * for any other single reason - including blind
                 * stupidity."  -- W.A. Wulf
                 */



#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include "tempfile.h"
#include "exitcode.h"

#ifndef _PATH_TMP
#define _PATH_TMP "/tmp"
#endif



/**
 * Create a file inside the temporary directory and register it for
 * later cleanup, and return its name.
 *
 * The file will be reopened later, possibly in a child.  But we know
 * that it exists with appropriately tight permissions.
 **/
int dcc_make_tmpnam(const char *prefix,
                    const char *suffix,
                    char *name_ret, int relative)
{
    unsigned long random_bits;
    unsigned long tries = 0;
    int fd;

    random_bits = (unsigned long) getpid() << 16;

# if HAVE_GETTIMEOFDAY
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        random_bits ^= tv.tv_usec << 16;
        random_bits ^= tv.tv_sec;
    }
# else
    random_bits ^= time(NULL);
# endif

#if 0
    random_bits = 0;            /* FOR TESTING */
#endif

    do {
        if (snprintf(name_ret, PATH_MAX, "%s/%s_%08lx%s",
                     _PATH_TMP + ( relative ? 1 : 0),
                      prefix,
                      random_bits & 0xffffffffUL,
                      suffix) == -1)
            return EXIT_OUT_OF_MEMORY;

        /* Note that if the name already exists as a symlink, this
         * open call will fail.
         *
         * The permissions are tight because nobody but this process
         * and our children should do anything with it. */
        fd = open(name_ret, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd == -1) {
	    /* Don't try getting a file too often.  Safety net against
	       endless loops. Probably just paranoia.  */
	    if (++tries > 1000000)
	      return EXIT_IO_ERROR;
	    /* Some errors won't change by changing the filename,
	       e.g. ENOENT means that the directory where we try to create
	       the file was removed from under us.  Don't endlessly loop
	       in that case.  */
            switch (errno) {
	      case EACCES: case EEXIST: case EISDIR: case ELOOP: 
		/* try again */
		random_bits += 7777; /* fairly prime */
		continue;
	    }
	    return EXIT_IO_ERROR;
        }
        
        if (close(fd) == -1) {  /* huh? */
            return EXIT_IO_ERROR;
        }
        
        break;
    } while (1);

    return 0;
}


