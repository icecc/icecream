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

using namespace std;

struct CPULoadInfo
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

    CPULoadInfo() {
        userTicks = 0;
        niceTicks = 0;
        sysTicks = 0;
        idleTicks = 0;
    }
};

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
    load->idleLoad = ( 1000 - ( load->userLoad + load->sysLoad + load->niceLoad) );
    if ( load->idleLoad < 0 )
        load->idleLoad = 0;
  } else
    load->userLoad = load->sysLoad = load->niceLoad = load->idleLoad = 0;

  load->userTicks = currUserTicks;
  load->sysTicks = currSysTicks;
  load->niceTicks = currNiceTicks;
  load->idleTicks = currIdleTicks;
}

static unsigned long int scan_one( const char* buff, const char *key )
{
  char *b = strstr( buff, key );
  if ( !b )
      return 0;
  unsigned long int val = 0;
  if ( sscanf( b + strlen( key ), ": %lu", &val ) != 1 )
      return 0;
  return val;
}

static unsigned int calculateMemLoad( const char* MemInfoBuf, unsigned long int &MemFree )
{
    MemFree = scan_one( MemInfoBuf, "MemFree" );
    unsigned long int Buffers = scan_one( MemInfoBuf, "Buffers" );
    unsigned long int Cached = scan_one( MemInfoBuf, "Cached" );

    if ( Buffers > 50 * 1024 )
        Buffers -= 50 * 1024;
    else
        Buffers /= 2;

    if ( Cached > 50 * 1024 )
        Cached -= 50 * 1024;
    else
        Cached /= 2;

    unsigned long int NetMemFree = MemFree + Cached + Buffers;
    if ( NetMemFree > 128 * 1014 )
        return 0;
    else
        return 1000 - ( NetMemFree * 1000 / ( 128 * 1024 ) );
}

bool fill_stats( StatsMsg &msg )
{
    static CPULoadInfo load;
    static char StatBuf[ 32 * 1024 ];
    int fd;

    if ( ( fd = open( "/proc/stat", O_RDONLY ) ) < 0 ) {
        log_error() << "Cannot open file \'/proc/stat\'!\n"
            "The kernel needs to be compiled with support\n"
            "forks /proc filesystem enabled!" << endl;
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

    if ( ( fd = open( "/proc/meminfo", O_RDONLY ) ) < 0 ) {
        log_error() << "Cannot open file \'/proc/meminfo\'!\n"
            "The kernel needs to be compiled with support\n"
            "forks /proc filesystem enabled!" << endl;
        return false;
    }

    n = read( fd, StatBuf, sizeof( StatBuf ) -1 );
    close( fd );
    if ( n < 40 ) {
        log_error() << "no enough date in /proc/meminfo?" << endl;
        return false;
    }
    StatBuf[n] = 0;
    unsigned long int MemFree = 0;
    unsigned int memory_fillgrade = calculateMemLoad( StatBuf, MemFree );

    unsigned int realLoad = 1000 - load.idleLoad - load.niceLoad;
    msg.load = ( 700 * realLoad + 300 * memory_fillgrade ) / 1000;
    if ( memory_fillgrade > 600 )
        msg.load = 1000;
    if ( realLoad > 950 )
        msg.load = 1000;
    msg.niceLoad = load.niceLoad;
    msg.sysLoad = load.sysLoad;
    msg.userLoad = load.userLoad;
    msg.idleLoad = load.idleLoad;

    double avg[3];
    getloadavg( avg, 3 );
    msg.loadAvg1 = ( unsigned int )avg[0] * 1000;
    msg.loadAvg5 = ( unsigned int )avg[1] * 1000;
    msg.loadAvg10 = ( unsigned int )avg[2] * 1000;

    msg.freeMem = ( unsigned int )( MemFree / 1024.0 + 0.5 );
//    trace() << "load cpu=" << netLoad << " mem=" << memory_fillgrade << " total=" << msg.load << endl;
    return true;
}
