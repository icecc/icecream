/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    Copyright (c) 1999, 2000 Chris Schlaeger <cs@kde.org>
    Copyright (c) 2003 Stephan Kulow <coolo@kde.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "config.h"
#include "load.h"
#include <unistd.h>
#include <stdio.h>
#include <math.h>
#include <logging.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_MACH_HOST_INFO_H
#define USE_MACH 1
#elif !defined( __linux__ ) && !defined(__CYGWIN__)
#define USE_SYSCTL
#endif

#ifdef USE_MACH
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#endif

#ifdef HAVE_KINFO_H
#include <kinfo.h>
#endif

#ifdef HAVE_DEVSTAT_H
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <devstat.h>
#endif

using namespace std;

// what the kernel puts as ticks in /proc/stat
typedef unsigned long long load_t;


struct CPULoadInfo {
    /* A CPU can be loaded with user processes, reniced processes and
     * system processes. Unused processing time is called idle load.
     * These variable store the percentage of each load type. */
    int userLoad;
    int niceLoad;
    int sysLoad;
    int idleLoad;

    /* To calculate the loads we need to remember the tick values for each
     * load type. */
    load_t userTicks;
    load_t niceTicks;
    load_t sysTicks;
    load_t idleTicks;
    load_t waitTicks;

    CPULoadInfo() {
        userTicks = 0;
        niceTicks = 0;
        sysTicks = 0;
        idleTicks = 0;
        waitTicks = 0;
    }
};

static void updateCPULoad(CPULoadInfo *load)
{
    load_t totalTicks;
    load_t currUserTicks, currSysTicks, currNiceTicks, currIdleTicks, currWaitTicks;

#if defined(USE_SYSCTL) && defined(__DragonFly__)
    static struct kinfo_cputime cp_time;

    kinfo_get_sched_cputime(&cp_time);
    /* There is one more load type exported via this interface in DragonFlyBSD -
     * interrupt load. But I think that we can do without it for our needs. */
    currUserTicks = cp_time.cp_user;
    currNiceTicks = cp_time.cp_nice;
    currSysTicks = cp_time.cp_sys;
    currIdleTicks = cp_time.cp_idle;
    /* It doesn't exist in DragonFlyBSD. */
    currWaitTicks = 0;

#elif defined (USE_SYSCTL)
    static int mibs[4] = { 0, 0, 0, 0 };
    static size_t mibsize = 4;
    unsigned long ticks[CPUSTATES];
    size_t mibdatasize = sizeof(ticks);

    if (mibs[0] == 0) {
        if (sysctlnametomib("kern.cp_time", mibs, &mibsize) < 0) {
            load->userTicks = load->sysTicks = load->niceTicks = load->idleTicks = 0;
            load->userLoad = load->sysLoad = load->niceLoad = load->idleLoad = 0;
            mibs[0] = 0;
            return;
        }
    }

    if (sysctl(mibs, mibsize, &ticks, &mibdatasize, NULL, 0) < 0) {
        load->userTicks = load->sysTicks = load->niceTicks = load->idleTicks = 0;
        load->userLoad = load->sysLoad = load->niceLoad = load->idleLoad = 0;
        return;
    }

    currUserTicks = ticks[CP_USER];
    currNiceTicks = ticks[CP_NICE];
    currSysTicks = ticks[CP_SYS];
    currIdleTicks = ticks[CP_IDLE];

#elif defined( USE_MACH )
    host_cpu_load_info r_load;

    kern_return_t error;
    mach_msg_type_number_t count;

    count = HOST_CPU_LOAD_INFO_COUNT;
    mach_port_t port = mach_host_self();
    error = host_statistics(port, HOST_CPU_LOAD_INFO,
                            (host_info_t)&r_load, &count);

    if (error != KERN_SUCCESS) {
        return;
    }

    currUserTicks = r_load.cpu_ticks[CPU_STATE_USER];
    currNiceTicks = r_load.cpu_ticks[CPU_STATE_NICE];
    currSysTicks  = r_load.cpu_ticks[CPU_STATE_SYSTEM];
    currIdleTicks = r_load.cpu_ticks[CPU_STATE_IDLE];
    currWaitTicks = 0;

#else
    char buf[256];
    static int fd = -1;

    if (fd < 0) {
        if ((fd = open("/proc/stat", O_RDONLY)) < 0) {
            log_error() << "Cannot open file \'/proc/stat\'!\n"
                        "The kernel needs to be compiled with support\n"
                        "for /proc filesystem enabled!" << endl;
            return;
        }

        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    lseek(fd, 0, SEEK_SET);
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf) - 1)) < 0 && errno == EINTR) {}

    if (n < 20) {
        log_error() << "no enough data in /proc/stat?" << endl;
        return;
    }

    buf[n] = 0;

    /* wait ticks only exist with Linux >= 2.6.0. treat as 0 otherwise */
    currWaitTicks = 0;
    //   sscanf( buf, "%*s %lu %lu %lu %lu %lu", &currUserTicks, &currNiceTicks,
    sscanf(buf, "%*s %llu %llu %llu %llu %llu", &currUserTicks, &currNiceTicks,  // RL modif
           &currSysTicks, &currIdleTicks, &currWaitTicks);
#endif

    totalTicks = (currUserTicks - load->userTicks)
                 + (currSysTicks - load->sysTicks)
                 + (currNiceTicks - load->niceTicks)
                 + (currIdleTicks - load->idleTicks)
                 + (currWaitTicks - load->waitTicks);

    if (totalTicks > 10) {
        load->userLoad = (1000 * (currUserTicks - load->userTicks)) / totalTicks;
        load->sysLoad = (1000 * (currSysTicks - load->sysTicks)) / totalTicks;
        load->niceLoad = (1000 * (currNiceTicks - load->niceTicks)) / totalTicks;
        load->idleLoad = (1000 - (load->userLoad + load->sysLoad + load->niceLoad));

        if (load->idleLoad < 0) {
            load->idleLoad = 0;
        }
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

#ifndef USE_SYSCTL
static unsigned long int scan_one(const char *buff, const char *key)
{
    const char *b = strstr(buff, key);

    if (!b) {
        return 0;
    }

    unsigned long int val = 0;

    if (sscanf(b + strlen(key), ": %lu", &val) != 1) {
        return 0;
    }

    return val;
}
#endif

static unsigned int calculateMemLoad(unsigned long int &NetMemFree)
{
    unsigned long long MemFree = 0, Buffers = 0, Cached = 0;

#ifdef USE_MACH
    /* Get VM statistics. */
    vm_statistics_data_t vm_stat;
    mach_msg_type_number_t count = sizeof(vm_stat) / sizeof(natural_t);
    kern_return_t error = host_statistics(mach_host_self(), HOST_VM_INFO,
                                          (host_info_t)&vm_stat, &count);

    if (error != KERN_SUCCESS) {
        return 0;
    }

    vm_size_t pagesize;
    host_page_size(mach_host_self(), &pagesize);

    unsigned long long MemInactive = (unsigned long long) vm_stat.inactive_count * pagesize;
    MemFree = (unsigned long long) vm_stat.free_count * pagesize;

    // blunt lie - but when's sche macht
    Buffers = MemInactive;

#elif defined( USE_SYSCTL )
    size_t len = sizeof(MemFree);

    if ((sysctlbyname("vm.stats.vm.v_free_count", &MemFree, &len, NULL, 0) == -1) || !len) {
        MemFree = 0;    /* Doesn't work under FreeBSD v2.2.x */
    }


    len = sizeof(Buffers);

    if ((sysctlbyname("vfs.bufspace", &Buffers, &len, NULL, 0) == -1) || !len) {
        Buffers = 0;    /* Doesn't work under FreeBSD v2.2.x */
    }

    len = sizeof(Cached);

    if ((sysctlbyname("vm.stats.vm.v_cache_count", &Cached, &len, NULL, 0) == -1) || !len) {
        Cached = 0;    /* Doesn't work under FreeBSD v2.2.x */
    }

#else
    /* The interesting information is definitely within the first 256 bytes */
    char buf[256];
    static int fd = -1;

    if (fd < 0) {
        if ((fd = open("/proc/meminfo", O_RDONLY)) < 0) {
            log_error() << "Cannot open file \'/proc/meminfo\'!\n"
                        "The kernel needs to be compiled with support\n"
                        "for /proc filesystem enabled!" << endl;
            return 0;
        }

        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    lseek(fd, 0, SEEK_SET);
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf) - 1)) < 0 && errno == EINTR) {}

    if (n < 20) {
        return 0;
    }

    buf[n] = '\0';
    MemFree = scan_one(buf, "MemFree");
    Buffers = scan_one(buf, "Buffers");
    Cached = scan_one(buf, "Cached");
#endif

    if (Buffers > 50 * 1024) {
        Buffers -= 50 * 1024;
    } else {
        Buffers /= 2;
    }

    if (Cached > 50 * 1024) {
        Cached -= 50 * 1024;
    } else {
        Cached /= 2;
    }

    NetMemFree = MemFree + Cached + Buffers;

    if (NetMemFree > 128 * 1024) {
        return 0;
    }

    return 1000 - (NetMemFree * 1000 / (128 * 1024));
}

// Load average calculation based on CALC_LOAD(), in the 2.6 Linux kernel
//  oldVal  - previous load avg.
//  numJobs - current number of active jobs
//  rate    - update rate, in seconds (usually 60, 300, or 900)
//  delta_t - time since last update, in seconds
double compute_load(double oldVal, unsigned int currentJobs, unsigned int rate, double delta_t)
{
    double weight = 1.0 / exp(delta_t / rate);
    return oldVal * weight + currentJobs * (1.0 - weight);
}

double getEpocTime()
{
    timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec / 1000000.0;
}

// Simulates getloadavg(), but only for specified number of jobs
//  Note: this is stateful and not thread-safe!
//        Also, it differs from getloadavg() in that its notion of load
//        is only updated as often as it's called.
int fakeloadavg(double *p_result, int resultEntries, unsigned int currentJobs)
{
    // internal state
    static const int numLoads = 3;
    static double loads[numLoads] = { 0.0, 0.0, 0.0 };
    static unsigned int rates[numLoads] = { 60, 300, 900 };
    static double lastUpdate = getEpocTime();

    // First, update all state
    double now = getEpocTime();
    double delta_t = std::max(now - lastUpdate, 0.0);   // guard against user changing system time backwards
    lastUpdate = now;

    for (int l = 0; l < numLoads; l++) {
        loads[l] = compute_load(loads[0], currentJobs, rates[l], delta_t);
    }

    // Then, return requested values
    int numFilled = std::min(std::max(resultEntries, 0), numLoads);

    for (int n = 0; n < numFilled; n++) {
        p_result[n] = loads[n];
    }

    return numFilled;
}

bool fill_stats(unsigned long &myidleload, unsigned long &myniceload, unsigned int &memory_fillgrade, StatsMsg *msg, unsigned int hint)
{
    static CPULoadInfo load;

    updateCPULoad(&load);

    myidleload = load.idleLoad;
    myniceload = load.niceLoad;

    if (msg) {
        unsigned long int MemFree = 0;

        memory_fillgrade = calculateMemLoad(MemFree);

        double avg[3];
#if HAVE_GETLOADAVG
        getloadavg(avg, 3);
        (void) hint;
#else
        fakeloadavg(avg, 3, hint);
#endif
        msg->loadAvg1 = (load_t)(avg[0] * 1000);
        msg->loadAvg5 = (load_t)(avg[1] * 1000);
        msg->loadAvg10 = (load_t)(avg[2] * 1000);

        msg->freeMem = (load_t)(MemFree / 1024.0 + 0.5);

    }

    return true;
}
