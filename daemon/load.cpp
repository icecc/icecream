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

#if defined( __FreeBSD__ ) || defined( MACOS )
#define USE_SYSCTL
#endif

#ifdef USE_SYSCTL
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <devstat.h>
#endif

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
    unsigned long waitTicks;

    CPULoadInfo() {
        userTicks = 0;
        niceTicks = 0;
        sysTicks = 0;
        idleTicks = 0;
        waitTicks = 0;
    }
};

static void updateCPULoad( CPULoadInfo* load )
{
  unsigned long totalTicks;
  unsigned long currUserTicks, currSysTicks, currNiceTicks, currIdleTicks, currWaitTicks;

#ifdef USE_SYSCTL
  static int mibs[4] = { 0,0,0,0 };
  static size_t mibsize = 4;
  unsigned long ticks[CPUSTATES];
  size_t mibdatasize = sizeof(ticks);

  if (mibs[0]==0) {
      if (sysctlnametomib("kern.cp_time",mibs,&mibsize) < 0) {
          load->userTicks = load->sysTicks = load->niceTicks = load->idleTicks = 0;
          load->userLoad = load->sysLoad = load->niceLoad = load->idleLoad = 0;
          mibs[0]=0;
          return;
      }
  }
  if (sysctl(mibs,mibsize,&ticks,&mibdatasize,NULL,0) < 0) {
      load->userTicks = load->sysTicks = load->niceTicks = load->idleTicks = 0;
      load->userLoad = load->sysLoad = load->niceLoad = load->idleLoad = 0;
      return;
  } else {
      currUserTicks = ticks[CP_USER];
      currNiceTicks = ticks[CP_NICE];
      currSysTicks = ticks[CP_SYS];
      currIdleTicks = ticks[CP_IDLE];
  }

#else
    char buf[ 256 ];
    static int fd = -1;

    if ( fd < 0 ) {
        if (( fd = open( "/proc/stat", O_RDONLY ) ) < 0 ) {
            log_error() << "Cannot open file \'/proc/stat\'!\n"
                "The kernel needs to be compiled with support\n"
                "for /proc filesystem enabled!" << endl;
            return;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    lseek(fd, 0, SEEK_SET);
    ssize_t n;

    while ( (n = read( fd, buf, sizeof(buf) -1 )) < 0 && errno == EINTR)
        ;

    if ( n < 20 ) {
        log_error() << "no enough data in /proc/stat?" << endl;
        return;
    }
    buf[n] = 0;

    /* wait ticks only exist with Linux >= 2.6.0. treat as 0 otherwise */
    currWaitTicks = 0;
    sscanf( buf, "%*s %lu %lu %lu %lu %lu", &currUserTicks, &currNiceTicks,
            &currSysTicks, &currIdleTicks, &currWaitTicks );
#endif

  totalTicks = ( currUserTicks - load->userTicks ) +
               ( currSysTicks - load->sysTicks ) +
               ( currNiceTicks - load->niceTicks ) +
               ( currIdleTicks - load->idleTicks ) +
               ( currWaitTicks - load->waitTicks );

  if ( totalTicks > 10 ) {
    load->userLoad = ( 1000 * ( currUserTicks - load->userTicks ) ) / totalTicks;
    load->sysLoad = ( 1000 * ( currSysTicks - load->sysTicks ) ) / totalTicks;
    load->niceLoad = ( 1000 * ( currNiceTicks - load->niceTicks ) ) / totalTicks;
    load->idleLoad = ( 1000 - ( load->userLoad + load->sysLoad + load->niceLoad) );
    if ( load->idleLoad < 0 )
        load->idleLoad = 0;
  } else {
    load->userLoad = load->sysLoad = load->niceLoad = 0; 
    load->idleLoad = 1000;
  }

  load->userTicks = currUserTicks;
  load->sysTicks = currSysTicks;
  load->niceTicks = currNiceTicks;
  load->idleTicks = currIdleTicks;
  load->waitTicks = currWaitTicks;
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

static unsigned int calculateMemLoad( unsigned long int &NetMemFree )
{
    unsigned long int MemFree;

#ifdef USE_SYSCTL
    size_t len = sizeof (MemFree);
    if ((sysctlbyname("vm.stats.vm.v_free_count", &MemFree, &len, NULL, 0) == -1) || !len)
        MemFree = 0; /* Doesn't work under FreeBSD v2.2.x */

    unsigned long int Buffers;
    len = sizeof (Buffers);
    if ((sysctlbyname("vfs.bufspace", &Buffers, &len, NULL, 0) == -1) || !len)
        Buffers = 0; /* Doesn't work under FreeBSD v2.2.x */

    unsigned long int Cached;
    len = sizeof (Cached);
    if ((sysctlbyname("vm.stats.vm.v_cache_count", &Cached, &len, NULL, 0) == -1) || !len)
            Cached = 0; /* Doesn't work under FreeBSD v2.2.x */
#else
    /* The interesting information is definitely within the first 256 bytes */
    char buf[256];
    static int fd = -1;

    if ( fd < 0 ) {
        if ( ( fd = open( "/proc/meminfo", O_RDONLY ) ) < 0 ) {
            log_error() << "Cannot open file \'/proc/meminfo\'!\n"
                "The kernel needs to be compiled with support\n"
                "for /proc filesystem enabled!" << endl;
            return 0;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    lseek (fd, 0, SEEK_SET);
    ssize_t n;
    while ((n = read( fd, buf, sizeof( buf ) -1 )) < 0 && errno == EINTR)
        ;
    if (n < 20)
        return 0;

    buf[n] = '\0';
    MemFree = scan_one( buf, "MemFree" );
    unsigned long int Buffers = scan_one( buf, "Buffers" );
    unsigned long int Cached = scan_one( buf, "Cached" );
#endif

    if ( Buffers > 50 * 1024 )
        Buffers -= 50 * 1024;
    else
        Buffers /= 2;

    if ( Cached > 50 * 1024 )
        Cached -= 50 * 1024;
    else
        Cached /= 2;

    NetMemFree = MemFree + Cached + Buffers;
    if ( NetMemFree > 128 * 1024 )
        return 0;
    else
        return 1000 - ( NetMemFree * 1000 / ( 128 * 1024 ) );
}

bool fill_stats( unsigned long &myidleload, unsigned int &memory_fillgrade, StatsMsg *msg )
{
    static CPULoadInfo load;

    updateCPULoad( &load );

    myidleload = load.idleLoad;

    if ( msg ) {
        unsigned long int MemFree = 0;

        memory_fillgrade = calculateMemLoad( MemFree );

        double avg[3];
        getloadavg( avg, 3 );
        msg->loadAvg1 = ( unsigned int )( avg[0] * 1000 );
        msg->loadAvg5 = ( unsigned int )( avg[1] * 1000 );
        msg->loadAvg10 = ( unsigned int )( avg[2] * 1000 );

        msg->freeMem = ( unsigned int )( MemFree / 1024.0 + 0.5 );

    }
    return true;
}
