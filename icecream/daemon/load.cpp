/*
    Copyright (c) 1999, 2000 Chris Schlaeger <cs@kde.org>
    Copyright (c) 2003 Stephan Kulow <coolo@kde.org>

    This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General Public
    License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "load.h"
#include <unistd.h>
#include <stdio.h>
#include <logging.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

typedef struct
{
  /* A CPU can be loaded with user processes, reniced processes and
   * system processes. Unused processing time is called idle load.
   * These variable store the percentage of each load type. */
  int userLoad;
  int niceLoad;
  int sysLoad;
  int idleLoad;

  /* To calculate the loads we need to remember the tick values for each
   * load type. */
  unsigned long userTicks;
  unsigned long niceTicks;
  unsigned long sysTicks;
  unsigned long idleTicks;
} CPULoadInfo;

static void updateCPULoad( const char* line, CPULoadInfo* load )
{
  unsigned long currUserTicks, currSysTicks, currNiceTicks, currIdleTicks;
  unsigned long totalTicks;

  sscanf( line, "%*s %lu %lu %lu %lu", &currUserTicks, &currNiceTicks,
          &currSysTicks, &currIdleTicks );

  totalTicks = ( currUserTicks - load->userTicks ) +
               ( currSysTicks - load->sysTicks ) +
               ( currNiceTicks - load->niceTicks ) +
               ( currIdleTicks - load->idleTicks );

  if ( totalTicks > 10 ) {
    load->userLoad = ( 1000 * ( currUserTicks - load->userTicks ) ) / totalTicks;
    load->sysLoad = ( 1000 * ( currSysTicks - load->sysTicks ) ) / totalTicks;
    load->niceLoad = ( 1000 * ( currNiceTicks - load->niceTicks ) ) / totalTicks;
    load->idleLoad = ( 1000 - ( load->userLoad + load->sysLoad + load->niceLoad ) );
  } else
    load->userLoad = load->sysLoad = load->niceLoad = load->idleLoad = 0;

  load->userTicks = currUserTicks;
  load->sysTicks = currSysTicks;
  load->niceTicks = currNiceTicks;
  load->idleTicks = currIdleTicks;
}

bool fill_stats( StatsMsg &msg )
{
    static CPULoadInfo load;
    static char StatBuf[ 32 * 1024 ];
    int fd;

    if ( ( fd = open( "/proc/stat", O_RDONLY ) ) < 0 ) {
        log_error() << "Cannot open file \'/proc/stat\'!\n"
            "The kernel needs to be compiled with support\n"
            "forks /proc filesystem enabled!\n" ;
        return false;
    }

    size_t n;
    n = read( fd, StatBuf, sizeof( StatBuf ) -1 );
    close( fd );
    if ( n < 40 ) {
        log_error() << "no enough date in /proc/stat?" << endl;
        return false;
    }
    StatBuf[n] = 0;
    updateCPULoad( StatBuf, &load );

    msg.load = load.niceLoad;
    return true;
}
