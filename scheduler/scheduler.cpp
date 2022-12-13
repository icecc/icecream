/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

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

#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <time.h>
#include <getopt.h>
#include <string>
#include <list>
#include <map>
#include <queue>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <string>
#include <limits>
#include <stdio.h>
#include <pwd.h>
#include "../services/comm.h"
#include "../services/getifaddrs.h"
#include "../services/logging.h"
#include "../services/job.h"
#include "../services/util.h"
#include "config.h"

#include "compileserver.h"
#include "job.h"
#include "scheduler.h"

/* TODO:
   * leak check
   * are all filedescs closed when done?
   * simplify lifetime of the various structures (Jobs/Channels/CompileServers know
     of each other and sometimes take over ownership)
 */

/* TODO:
  - iron out differences in code size between architectures
   + ia64/i686: 1.63
   + x86_64/i686: 1.48
   + ppc/i686: 1.22
   + ppc64/i686: 1.59
  (missing data for others atm)
*/

/* The typical flow of messages for a remote job should be like this:
     prereq: daemon is connected to scheduler
     * client does GET_CS
     * request gets queued
     * request gets handled
     * scheduler sends USE_CS
     * client asks remote daemon
     * daemon sends JOB_BEGIN
     * client sends END + closes connection
     * daemon sends JOB_DONE (this can be swapped with the above one)
   This means, that iff the client somehow closes the connection we can and
   must remove all traces of jobs resulting from that client in all lists.
 */

using namespace std;

static string pidFilePath;

static map<int, CompileServer *> fd2cs;
static volatile sig_atomic_t exit_main_loop = false;

time_t starttime;
time_t last_announce;
static string scheduler_interface = "";
static unsigned int scheduler_port = 8765;

// A subset of connected_hosts representing the compiler servers
static list<CompileServer *> css;
static list<CompileServer *> monitors;
static list<CompileServer *> controls;
static list<string> block_css;
static unsigned int new_job_id;
static map<unsigned int, Job *> jobs;

/* XXX Uah.  Don't use a queue for the job requests.  It's a hell
   to delete anything out of them (for clean up).  */
// Job requests from one submitter.
struct JobRequestsGroup {
    list<Job *> l;
    CompileServer *submitter;
    // Priority as unix nice values 0 (highest) to 20 (lowest).
    // Values <0 are mapped to 0 (otherwise somebody could use this to starve
    // the whole cluster).
    int niceness;
    bool remove_job(Job *);
};
// All pending job requests, grouped by the same submitter and niceness value,
// and sorted with higher priority first.
static list<JobRequestsGroup *> job_requests;

static list<JobStat> all_job_stats;
static JobStat cum_job_stats;

static float server_speed(CompileServer *cs, Job *job = nullptr, bool blockDebug = false);

/* Searches the queue for JOB and removes it.
   Returns true if something was deleted.  */
bool JobRequestsGroup::remove_job(Job *job)
{
    assert(niceness == job->niceness());
    for (list<Job *>::iterator it = l.begin(); it != l.end(); ++it)
        if (*it == job) {
            l.erase(it);
            return true;
        }
    return false;
}

static void add_job_stats(Job *job, JobDoneMsg *msg)
{
    JobStat st;

    /* We don't want to base our timings on failed or too small jobs.  */
    if (msg->out_uncompressed < 4096
            || msg->exitcode != 0) {
        return;
    }

    st.setOutputSize(msg->out_uncompressed);
    st.setCompileTimeReal(msg->real_msec);
    st.setCompileTimeUser(msg->user_msec);
    st.setCompileTimeSys(msg->sys_msec);
    st.setJobId(job->id());

    if (job->argFlags() & CompileJob::Flag_g) {
        st.setOutputSize(st.outputSize() * 10 / 36);    // average over 1900 jobs: faktor 3.6 in osize
    } else if (job->argFlags() & CompileJob::Flag_g3) {
        st.setOutputSize(st.outputSize() * 10 / 45);    // average over way less jobs: factor 1.25 over -g
    }

    // the difference between the -O flags isn't as big as the one between -O0 and -O>=1
    // the numbers are actually for gcc 3.3 - but they are _very_ rough heurstics anyway)
    if (job->argFlags() & CompileJob::Flag_O
            || job->argFlags() & CompileJob::Flag_O2
            || job->argFlags() & CompileJob::Flag_Ol2) {
        st.setOutputSize(st.outputSize() * 58 / 35);
    }

    if (job->server()->lastCompiledJobs().size() >= 7) {
        /* Smooth out spikes by not allowing one job to add more than
           20% of the current speed.  */
        float this_speed = (float) st.outputSize() / (float) st.compileTimeUser();
        /* The current speed of the server, but without adjusting to the current
           job, hence no second argument.  */
        float cur_speed = server_speed(job->server());

        if ((this_speed / 1.2) > cur_speed) {
            st.setOutputSize((long unsigned) (cur_speed * 1.2 * st.compileTimeUser()));
        } else if ((this_speed * 1.2) < cur_speed) {
            st.setOutputSize((long unsigned)(cur_speed / 1.2 * st.compileTimeUser()));
        }
    }

    job->server()->appendCompiledJob(st);
    job->server()->setCumCompiled(job->server()->cumCompiled() + st);

    if (job->server()->lastCompiledJobs().size() > 200) {
        job->server()->setCumCompiled(job->server()->cumCompiled() - *job->server()->lastCompiledJobs().begin());
        job->server()->popCompiledJob();
    }

    job->submitter()->appendRequestedJobs(st);
    job->submitter()->setCumRequested(job->submitter()->cumRequested() + st);

    if (job->submitter()->lastRequestedJobs().size() > 200) {
        job->submitter()->setCumRequested(job->submitter()->cumRequested() - *job->submitter()->lastRequestedJobs().begin());
        job->submitter()->popRequestedJobs();
    }

    all_job_stats.push_back(st);
    cum_job_stats += st;

    if (all_job_stats.size() > 2000) {
        cum_job_stats -= *all_job_stats.begin();
        all_job_stats.pop_front();
    }

#if DEBUG_SCHEDULER > 1
    if (job->argFlags() < 7000) {
        trace() << "add_job_stats " << job->language() << " "
                << (time(0) - starttime) << " "
                << st.compileTimeUser() << " "
                << (job->argFlags() & CompileJob::Flag_g ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_g3 ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_O ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_O2 ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_Ol2 ? '1' : '0')
                << " " << st.outputSize() << " " << msg->out_uncompressed << " "
                << job->server()->nodeName() << " "
                << float(msg->out_uncompressed) / st.compileTimeUser() << " "
                << server_speed(job->server(), NULL, true) << endl;
    }
#endif
}

static bool handle_end(CompileServer *cs, Msg *);

static void notify_monitors(Msg *m)
{
    list<CompileServer *>::iterator it;
    list<CompileServer *>::iterator it_old;

    for (it = monitors.begin(); it != monitors.end();) {
        it_old = it++;

        /* If we can't send it, don't be clever, simply close this monitor.  */
        if (!(*it_old)->send_msg(*m, MsgChannel::SendNonBlocking /*| MsgChannel::SendBulkOnly*/)) {
            trace() << "monitor is blocking... removing" << endl;
            handle_end(*it_old, nullptr);
        }
    }

    delete m;
}

static float server_speed(CompileServer *cs, Job *job, bool blockDebug)
{
#if DEBUG_SCHEDULER <= 2
    (void)blockDebug;
#endif
    if (cs->lastCompiledJobs().size() == 0 || cs->cumCompiled().compileTimeUser() == 0) {
        return 0;
    } else {
        float f = (float)cs->cumCompiled().outputSize()
                  / (float) cs->cumCompiled().compileTimeUser();

        // we only care for the load if we're about to add a job to it
        if (job) {
            if (job->submitter() == cs) {
                int clientCount = cs->clientCount();
                if( clientCount == 0 ) {
                    // Older client/daemon that doesn't send client count. Use the number of jobs
                    // that we've already been told about as the fallback value (it will sometimes
                    // be an underestimate).
                    clientCount = cs->submittedJobsCount();
                }
                if (clientCount > cs->maxJobs()) {
                    // The submitter would be overloaded by building all its jobs locally,
                    // so penalize it heavily in order to send jobs preferably to other nodes,
                    // so that the submitter should preferably do tasks that cannot be distributed,
                    // such as linking or preparing jobs for remote nodes.
                    f *= 0.1;
#if DEBUG_SCHEDULER > 2
                    if(!blockDebug)
                        log_info() << "penalizing local build for job " << job->id() << endl;
#endif
                } else if (clientCount == cs->maxJobs()) {
                    // This means the submitter would be fully loaded by its jobs. It is still
                    // preferable to distribute the job, unless the submitter is noticeably faster.
                    f *= 0.8;
#if DEBUG_SCHEDULER > 2
                    if(!blockDebug)
                        log_info() << "slightly penalizing local build for job " << job->id() << endl;
#endif
                }
                else if (clientCount <= cs->maxJobs() / 2) {
                    // The submitter has only few jobs, slightly prefer building the job locally
                    // in order to save the overhead of distributing.
                    // Note that this is unreliable, the submitter may be in fact running a large
                    // parallel build but this is just the first of the jobs and other icecc instances
                    // haven't been launched yet. There's probably no good way to detect this reliably.
                    f *= 1.1;
#if DEBUG_SCHEDULER > 2
                    if(!blockDebug)
                        log_info() << "slightly preferring local build for job " << job->id() << endl;
#endif
                } else {
                    // the remaining case, don't adjust
                    f *= 1;
                }
                // ignoring load for submitter - assuming the load is our own
            } else {
                f *= float(1000 - cs->load()) / 1000;
            }

            /* Gradually throttle with the number of assigned jobs. This
             * takes care of the fact that not all slots are equally fast on
             * CPUs with SMT and dynamic clock ramping.
             */
            f *= (1.0f - (0.5f * cs->currentJobCount() / cs->maxJobs()));
        }

        // below we add a pessimism factor - assuming the first job a computer got is not representative
        if (cs->lastCompiledJobs().size() < 7) {
            f *= (-0.5 * cs->lastCompiledJobs().size() + 4.5);
        }

        return f;
    }
}

static void handle_monitor_stats(CompileServer *cs, StatsMsg *m = nullptr)
{
    if (monitors.empty()) {
        return;
    }

    string msg;
    char buffer[1000];
    sprintf(buffer, "Name:%s\n", cs->nodeName().c_str());
    msg += buffer;
    sprintf(buffer, "IP:%s\n", cs->name.c_str());
    msg += buffer;
    sprintf(buffer, "MaxJobs:%d\n", cs->maxJobs());
    msg += buffer;
    sprintf(buffer, "NoRemote:%s\n", cs->noRemote() ? "true" : "false");
    msg += buffer;
    sprintf(buffer, "Platform:%s\n", cs->hostPlatform().c_str());
    msg += buffer;
    sprintf(buffer, "Version:%d\n", cs->maximum_remote_protocol);
    msg += buffer;
    sprintf(buffer, "Features:%s\n", supported_features_to_string(cs->supportedFeatures()).c_str());
    msg += buffer;
    sprintf(buffer, "Speed:%f\n", server_speed(cs));
    msg += buffer;

    if (m) {
        sprintf(buffer, "Load:%d\n", m->load);
        msg += buffer;
        sprintf(buffer, "LoadAvg1:%u\n", m->loadAvg1);
        msg += buffer;
        sprintf(buffer, "LoadAvg5:%u\n", m->loadAvg5);
        msg += buffer;
        sprintf(buffer, "LoadAvg10:%u\n", m->loadAvg10);
        msg += buffer;
        sprintf(buffer, "FreeMem:%u\n", m->freeMem);
        msg += buffer;
    } else {
        sprintf(buffer, "Load:%u\n", cs->load());
        msg += buffer;
    }

    notify_monitors(new MonStatsMsg(cs->hostId(), msg));
}

static Job *create_new_job(CompileServer *submitter)
{
    ++new_job_id;
    assert(jobs.find(new_job_id) == jobs.end());

    Job *job = new Job(new_job_id, submitter);
    jobs[new_job_id] = job;
    return job;
}

static void enqueue_job_request(Job *job)
{
    for( list<JobRequestsGroup*>::iterator it = job_requests.begin(); it != job_requests.end(); ++it ) {
        if( (*it)->submitter == job->submitter() && (*it)->niceness == job->niceness()) {
            (*it)->l.push_back(job);
            return;
        }
        if( (*it)->niceness > job->niceness()) { // lower priority starts here, insert group
            JobRequestsGroup *newone = new JobRequestsGroup();
            newone->submitter = job->submitter();
            newone->niceness = job->niceness();
            newone->l.push_back(job);
            job_requests.insert(it, newone);
            return;
        }
    }
    JobRequestsGroup *newone = new JobRequestsGroup();
    newone->submitter = job->submitter();
    newone->niceness = job->niceness();
    newone->l.push_back(job);
    job_requests.push_back(newone);
}

static void enqueue_job_requests_group(JobRequestsGroup* group) {
    for( list<JobRequestsGroup*>::iterator it = job_requests.begin(); it != job_requests.end(); ++it ) {
        if( (*it)->niceness > group->niceness) { // lower priority starts here, insert group
            job_requests.insert(it, group);
            return;
        }
    }
    job_requests.push_back(group);
}

// Gives a position in job_requests, used to iterate items.
struct JobRequestPosition
{
    JobRequestPosition() : group( nullptr ), job( nullptr ) {}
    JobRequestPosition(JobRequestsGroup* g, Job* j) : group( g ), job( j ) {}
    bool isValid() const { return group != nullptr; }
    JobRequestsGroup* group;
    Job* job;
};

static JobRequestPosition get_first_job_request()
{
    if (job_requests.empty()) {
        return JobRequestPosition();
    }

    JobRequestsGroup *first = job_requests.front();
    assert(!first->l.empty());
    return JobRequestPosition( first, first->l.front());
}

static JobRequestPosition get_next_job_request(const JobRequestPosition& pos)
{
    assert(!job_requests.empty());
    assert(pos.group != nullptr && pos.job != nullptr);

    JobRequestsGroup* group = pos.group;
    // Get next job in the same group.
    list<Job*>::iterator jobIt = std::find(group->l.begin(), group->l.end(), pos.job);
    assert(jobIt != group->l.end());
    ++jobIt;
    if( jobIt != group->l.end())
        return JobRequestPosition( group, *jobIt );
    // Get next group.
    list<JobRequestsGroup*>::iterator groupIt = std::find(job_requests.begin(), job_requests.end(), group);
    assert(groupIt != job_requests.end());
    ++groupIt;
    if( groupIt != job_requests.end())
    {
        group = *groupIt;
        assert(!group->l.empty());
        return JobRequestPosition( group, group->l.front());
    }
    // end
    return JobRequestPosition();
}

// Removes the given job request.
// Also tries to rotate submitters in a round-robin fashion to try to serve
// them all fairly.
static void remove_job_request(const JobRequestPosition& pos)
{
    assert(!job_requests.empty());
    assert(pos.group != nullptr && pos.job != nullptr);

    JobRequestsGroup* group = pos.group;
    assert(std::find(job_requests.begin(), job_requests.end(), group) != job_requests.end());
    job_requests.remove(group);
    assert(std::find(group->l.begin(), group->l.end(), pos.job) != group->l.end());
    group->remove_job(pos.job);

    if (group->l.empty()) {
        delete group;
    } else {
        enqueue_job_requests_group(group);
    }
}

static string dump_job(Job *job);

static bool handle_cs_request(MsgChannel *cs, Msg *_m)
{
    GetCSMsg *m = dynamic_cast<GetCSMsg *>(_m);

    if (!m) {
        return false;
    }

    CompileServer *submitter = static_cast<CompileServer *>(cs);

    submitter->setClientCount(m->client_count);

    Job *master_job = nullptr;

    for (unsigned int i = 0; i < m->count; ++i) {
        Job *job = create_new_job(submitter);
        job->setEnvironments(m->versions);
        job->setTargetPlatform(m->target);
        job->setArgFlags(m->arg_flags);
        switch(m->lang) {
            case CompileJob::Lang_C:
                job->setLanguage("C");
                break;
            case CompileJob::Lang_CXX:
                job->setLanguage("C++");
                break;
            case CompileJob::Lang_OBJC:
                job->setLanguage("ObjC");
                break;
            case CompileJob::Lang_OBJCXX:
                job->setLanguage("ObjC++");
                break;
            case CompileJob::Lang_Custom:
                job->setLanguage("<custom>");
                break;
            default:
                job->setLanguage("???"); // presumably newer client?
                break;
        }
        job->setFileName(m->filename);
        job->setLocalClientId(m->client_id);
        job->setPreferredHost(m->preferred_host);
        job->setMinimalHostVersion(m->minimal_host_version);
        job->setRequiredFeatures(m->required_features);
        job->setNiceness(max(0, min(20,int(m->niceness))));
        enqueue_job_request(job);
        std::ostream &dbg = log_info();
        dbg << "NEW " << job->id() << " client="
            << submitter->nodeName() << " versions=[";

        Environments envs = job->environments();

        for (Environments::const_iterator it = envs.begin();
                it != envs.end();) {
            dbg << it->second << "(" << it->first << ")";

            if (++it != envs.end()) {
                dbg << ", ";
            }
        }

        dbg << "] " << m->filename << " " << job->language() << " " << job->niceness() << endl;
        notify_monitors(new MonGetCSMsg(job->id(), submitter->hostId(), m));

        if (!master_job) {
            master_job = job;
        } else {
            master_job->appendJob(job);
        }
    }

    return true;
}

static bool handle_local_job(CompileServer *cs, Msg *_m)
{
    JobLocalBeginMsg *m = dynamic_cast<JobLocalBeginMsg *>(_m);

    if (!m) {
        return false;
    }

    ++new_job_id;
    trace() << "handle_local_job " << (m->fulljob ? "(full) " : "") << m->outfile
        << " " << m->id << endl;
    cs->insertClientLocalJobId(m->id, new_job_id, m->fulljob);
    notify_monitors(new MonLocalJobBeginMsg(new_job_id, m->outfile, m->stime, cs->hostId()));
    return true;
}

static bool handle_local_job_done(CompileServer *cs, Msg *_m)
{
    JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg *>(_m);

    if (!m) {
        return false;
    }

    trace() << "handle_local_job_done " << m->job_id << endl;
    notify_monitors(new JobLocalDoneMsg(cs->getClientLocalJobId(m->job_id)));
    cs->eraseClientLocalJobId(m->job_id);
    return true;
}

/* Given a candidate CS and a JOB, check all installed environments
   on the CS for a match.  Return an empty string if none of the required
   environments for this job is installed.  Otherwise return the
   host platform of the first found installed environment which is among
   the requested.  That can be send to the client, which then completely
   specifies which environment to use (name, host platform and target
   platform).  */
static string envs_match(CompileServer *cs, const Job *job)
{
    if (job->submitter() == cs) {
        return cs->hostPlatform();    // it will compile itself
    }

    Environments compilerVersions = cs->compilerVersions();

    /* Check all installed envs on the candidate CS ...  */
    for (Environments::const_iterator it = compilerVersions.begin();
            it != compilerVersions.end(); ++it) {
        if (it->first == job->targetPlatform()) {
            /* ... IT now is an installed environment which produces code for
               the requested target platform.  Now look at each env which
               could be installed from the client (i.e. those coming with the
               job) if it matches in name and additionally could be run
               by the candidate CS.  */
            Environments environments = job->environments();
            for (Environments::const_iterator it2 = environments.begin();
                    it2 != environments.end(); ++it2) {
                if (it->second == it2->second && cs->platforms_compatible(it2->first)) {
                    return it2->first;
                }
            }
        }
    }

    return string();
}

static list<CompileServer *> filter_ineligible_servers(Job *job)
{
    list<CompileServer *> eligible;
    std::copy_if(
        css.begin(),
        css.end(),
        std::back_inserter(eligible),
        [=](CompileServer* cs) {
            if (!cs->is_eligible_now(job)) {
#if DEBUG_SCHEDULER > 1
                if ((cs->currentJobCount() >= cs->maxJobs() + cs->maxPreloadCount()) || (cs->load() >= 1000)) {
                    trace() << "overloaded " << cs->nodeName() << " " << cs->currentJobCount() << "/"
                            <<  cs->maxJobs() << " jobs, load:" << cs->load() << endl;
                } else
                    trace() << cs->nodeName() << " not eligible" << endl;
#endif
                return false;
            }

            // incompatible architecture or busy installing
            if (!cs->can_install(job).size()) {
#if DEBUG_SCHEDULER > 2
                trace() << cs->nodeName() << " can't install " << job->id() << endl;
#endif
                return false;
            }

            /* Don't use non-chroot-able daemons for remote jobs.  XXX */
            if (!cs->chrootPossible() && cs != job->submitter()) {
                trace() << cs->nodeName() << " can't use chroot\n";
                return false;
            }

            // Check if remote & if remote allowed
            if (!cs->check_remote(job)) {
                trace() << cs->nodeName() << " fails remote job check\n";
                return false;
            }

            return true;
        });
    return eligible;
}

static CompileServer *pick_server_random(list<CompileServer *> &eligible)
{
    auto iter = eligible.cbegin();
    std::advance(iter, random() % eligible.size());
    return *iter;
}

static CompileServer *pick_server_round_robin(list<CompileServer *> &eligible)
{
    unsigned int oldest_job = 0;
    CompileServer *selected = nullptr;

    // The scheduler assigns each job a unique ID from a monotonically increasing
    // integer sequence starting from 1. When a job is assigned to a compile
    // server, the scheduler records the assigned job ID, which is then available
    // from lastPickedId().
    for (CompileServer * const cs: eligible) {
#if DEBUG_SCHEDULER > 1
        trace()
            << "considering server " << cs->nodeName() << " with last job ID "
            << cs->lastPickedId() << " and oldest known job ID " << oldest_job
            << endl;
#endif
        if (!selected || cs->lastPickedId() < oldest_job) {
            selected = cs;
            oldest_job = cs->lastPickedId();
        }
    }
    return selected;
}

static CompileServer *pick_server_least_busy(list<CompileServer *> &eligible)
{
    unsigned long min_load = 0;
    list<CompileServer *> selected_list;

    // We want to pick the server with the fewest run jobs, but in a round-robin
    // fashion if multiple happen to be the least-busy so we can distribute the
    // load out better.
    for (CompileServer * const cs: eligible) {
#if DEBUG_SCHEDULER > 1
        trace()
            << "considering server " << cs->nodeName() << " with "
            << cs->currentJobCount() << " of " << cs->maxJobs() << " maximum jobs"
            << endl;
#endif
        if (cs->maxJobs()) {
            unsigned long cs_load = 0;

            // Calculate the ceiling of the current job load ratio
            if (cs->currentJobCount()) {
                cs_load = 1 + ((cs->currentJobCount() - 1) / cs->maxJobs());
            }

            if (cs_load < min_load) {
                min_load = cs_load;
            }
        }
    }

    std::copy_if(
        eligible.begin(),
        eligible.end(),
        std::back_inserter(selected_list),
        [=](CompileServer* cs) {
            return cs->maxJobs() && size_t(cs->currentJobCount()) / cs->maxJobs() == min_load;
        });

#if DEBUG_SCHEDULER > 1
    trace()
        << "servers to consider further: " << selected_list.size()
        << ", using ROUND_ROBIN for final selection" << endl;
#endif
    return pick_server_round_robin(selected_list);
}

static CompileServer *pick_server_new(Job *job, list<CompileServer *> &eligible)
{
    CompileServer *selected = nullptr;

    for (CompileServer * const cs: eligible) {
        if ((cs->lastCompiledJobs().size() == 0) && (cs->currentJobCount() == 0) && cs->maxJobs()) {
            if (!selected) {
                selected = cs;
            } else if (!envs_match(cs, job).empty()) {
                // if there is one server that already got the environment and one that
                // hasn't compiled at all, pick the one with environment first
                selected = cs;
            }
        }
    }
    return selected;
}

static CompileServer *pick_server_fastest(Job *job, list<CompileServer *> &eligible)
{
    // If we have no statistics simply use any server which is usable
    if (!all_job_stats.size()) {
        CompileServer *selected = pick_server_random(eligible);
        trace()
            << "no job stats - returning randomly selected "
            << selected->nodeName()
            << " load: "
            << selected->load()
            << " can install: "
            << selected->can_install(job)
            << endl;
        return selected;
    }

    CompileServer *best = nullptr;
    // best uninstalled
    CompileServer *bestui = nullptr;
    // best preloadable host
    CompileServer *bestpre = nullptr;

    // Any "new" servers with no stats should be selected first so we can get the stats we need.
    best = pick_server_new(job, eligible);
    if (best) {
        return best;
    }

    for (CompileServer * const cs : eligible) {

#if DEBUG_SCHEDULER > 1
        trace() << cs->nodeName() << " compiled " << cs->lastCompiledJobs().size() << " got now: " <<
                cs->currentJobCount() << " speed: " << server_speed(cs, job, true) << " compile time " <<
                cs->cumCompiled().compileTimeUser() << " produced code " << cs->cumCompiled().outputSize() <<
                " client count: " << cs->clientCount() << endl;
#endif

        // Some portion of the selection will go to a host that has not been selected
        // in a while so we can maintain reasonably up-to-date statistics. The greater
        // the weight, the less likely this is to happen.
        uint8_t weight_limit = std::numeric_limits<uint8_t>::max() - STATS_UPDATE_WEIGHT;
        uint8_t weight_factor = weight_limit / std::numeric_limits<uint8_t>::max();

        // Job IDs are assigned from a monotonically increasing sequence by the
        // scheduler, and each compile server records the ID of the last job it
        // ran. We use that here to determine whether a job should simply run on
        // the "next" host that hasn't seen a job for a long time.
        if (weight_factor > 0 && (!cs->lastPickedId() ||
            ((job->id() - cs->lastPickedId()) > (weight_factor * eligible.size())))) {
            best = cs;
            break;
        }

        if (!envs_match(cs, job).empty()) {
            if (!best) {
                best = cs;
            }
            // Search the server with the earliest projected time to compile
            // the job.  (XXX currently this is equivalent to the fastest one)
            else if ((best->lastCompiledJobs().size() != 0)
                     && (server_speed(best, job) < server_speed(cs, job))) {
                if (cs->currentJobCount() < cs->maxJobs()) {
                    best = cs;
                } else {
                    bestpre = cs;
                }
            }

        } else {
            if (!bestui) {
                bestui = cs;
            }
            // Search the server with the earliest projected time to compile
            // the job.  (XXX currently this is equivalent to the fastest one)
            else if ((bestui->lastCompiledJobs().size() != 0)
                     && (server_speed(bestui, job) < server_speed(cs, job))) {
                if (cs->currentJobCount() < cs->maxJobs()) {
                    bestui = cs;
                } else {
                    bestpre = cs;
                }
            }
        }
    }

    if (best) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best installed " << best->nodeName() << " " <<  server_speed(best, job, true) << endl;
#endif
        return best;
    }

    if (bestui) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best uninstalled " << bestui->nodeName() << " " <<  server_speed(bestui, job, true) << endl;
#endif
        return bestui;
    }

    if (bestpre) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best preload " << bestpre->nodeName() << " " <<  server_speed(bestpre, job, true) << endl;
#endif
    }

    return bestpre;
}

static CompileServer *pick_server(Job *job, SchedulerAlgorithmName schedulerAlgorithm)
{
#if DEBUG_SCHEDULER > 0
    /* consistency checking for now */
    for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
        CompileServer *cs = *it;

        const list<Job *>& jobList = cs->jobList();
        for (list<Job *>::const_iterator it2 = jobList.begin(); it2 != jobList.end(); ++it2) {
            assert(jobs.find((*it2)->id()) != jobs.end());
        }
    }

    for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
            it != jobs.end(); ++it) {
        Job *j = it->second;

        if (j->state() == Job::COMPILING) {
            CompileServer *cs = j->server();
            const list<Job *>& jobList = cs->jobList();
            assert(find(jobList.begin(), jobList.end(), j) != jobList.end());
        }
    }
#endif

    // Ignore ineligible servers
    list<CompileServer *> eligible = filter_ineligible_servers(job);

#if DEBUG_SCHEDULER > 1
    trace() << "pick_server " << job->id() << " " << job->targetPlatform() << endl;
#endif

    /* if the user wants to test/prefer one specific daemon, we return it if available */
    if (!job->preferredHost().empty()) {
        for (CompileServer* const cs : css) {
            if (cs->matches(job->preferredHost()) && cs->is_eligible_now(job)) {
#if DEBUG_SCHEDULER > 1
                trace() << "taking preferred " << cs->nodeName() << " " <<  server_speed(cs, job, true) << endl;
#endif
                return cs;
            }
        }

        return nullptr;
    }

    // Don't bother running an algorithm if we don't need to.
    if ( eligible.size() == 0 ) {
        trace() << "no eligible servers" << endl;
        return nullptr;
    } else if (eligible.size() == 1) {
        CompileServer *selected = eligible.front();
        trace() << "returning only available server "
            << selected->nodeName()
            << " load: "
            << selected->load()
            << " can install: "
            << selected->can_install(job)
            << endl;
        return selected;
    }

    CompileServer *selected;
    switch (schedulerAlgorithm) {
        case SchedulerAlgorithmName::NONE:
        case SchedulerAlgorithmName::UNDEFINED:
            [[fallthrough]];
        default:
            trace()
                << "unknown scheduler algorithm " << schedulerAlgorithm
                << ", using " << SchedulerAlgorithmName::RANDOM << endl;
            [[fallthrough]];
        case SchedulerAlgorithmName::RANDOM:
            selected = pick_server_random(eligible);
            break;
        case SchedulerAlgorithmName::ROUND_ROBIN:
            selected = pick_server_round_robin(eligible);
            break;
        case SchedulerAlgorithmName::LEAST_BUSY:
            selected = pick_server_least_busy(eligible);
            break;
        case SchedulerAlgorithmName::FASTEST:
            selected = pick_server_fastest(job, eligible);
            break;
    }

    if (selected) {
        trace()
            << "selected " << selected->nodeName()
            << " using " << schedulerAlgorithm << " algorithm" << endl;
    } else {
        trace()
            << "failed to select a server using "
            << schedulerAlgorithm << " algorithm" << endl;
    }
    return selected;
}

/* Prunes the list of connected servers by those which haven't
   answered for a long time. Return the number of seconds when
   we have to cleanup next time. */
static time_t prune_servers()
{
    list<CompileServer *>::iterator it;

    time_t now = time(nullptr);
    time_t min_time = MAX_SCHEDULER_PING;

    for (it = controls.begin(); it != controls.end();) {
        if ((now - (*it)->last_talk) >= MAX_SCHEDULER_PING) {
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        }

        min_time = min(min_time, MAX_SCHEDULER_PING - now + (*it)->last_talk);
        ++it;
    }

    for (it = css.begin(); it != css.end();) {
        (*it)->startInConnectionTest();
        time_t cs_in_conn_timeout = (*it)->getNextTimeout();
        if(cs_in_conn_timeout != -1)
        {
            min_time = min(min_time, cs_in_conn_timeout);
        }

        if ((*it)->busyInstalling() && ((now - (*it)->busyInstalling()) >= MAX_BUSY_INSTALLING)) {
            trace() << "busy installing for a long time - removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        }

        /* protocol version 27 and newer use TCP keepalive */
        if (IS_PROTOCOL_VERSION(27, *it)) {
            ++it;
            continue;
        }

        if ((now - (*it)->last_talk) >= MAX_SCHEDULER_PING) {
            if ((*it)->maxJobs() >= 0) {
                trace() << "send ping " << (*it)->nodeName() << endl;
                (*it)->setMaxJobs((*it)->maxJobs() * -1);   // better not give it away

                if ((*it)->send_msg(PingMsg())) {
                    // give it MAX_SCHEDULER_PONG to answer a ping
                    (*it)->last_talk = time(nullptr) - MAX_SCHEDULER_PING
                                       + 2 * MAX_SCHEDULER_PONG;
                    min_time = min(min_time, (time_t) 2 * MAX_SCHEDULER_PONG);
                    ++it;
                    continue;
                }
            }

            // R.I.P.
            trace() << "removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        } else {
            min_time = min(min_time, MAX_SCHEDULER_PING - now + (*it)->last_talk);
        }

#if DEBUG_SCHEDULER > 1
        if ((random() % 400) < 0) {
            // R.I.P.
            trace() << "FORCED removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, 0);
            continue;
        }
#endif

        ++it;
    }

    return min_time;
}

static bool empty_queue(SchedulerAlgorithmName schedulerAlgorithm)
{
    JobRequestPosition jobPosition = get_first_job_request();
    if (!jobPosition.isValid()) {
        return false;
    }

    assert(!css.empty());

    CompileServer *use_cs = nullptr;
    Job* job = jobPosition.job;

    while (true) {
        use_cs = pick_server(job, schedulerAlgorithm);

        if (use_cs) {
            break;
        }

        /* Ignore the load on the submitter itself if no other host could
           be found.  We only obey to its max job number.  */
        use_cs = job->submitter();
        if ((use_cs->currentJobCount() < use_cs->maxJobs())
                && job->preferredHost().empty()
                /* This should be trivially true.  */
                && use_cs->can_install(job).size()) {
            break;
        }

        jobPosition = get_next_job_request( jobPosition );
        if (!jobPosition.isValid()) { // no job found in the whole job_requests list
            jobPosition = get_first_job_request();
            assert( jobPosition.isValid());
            job = jobPosition.job;
            for (CompileServer * const cs : css) {
                if(!job->preferredHost().empty() && !cs->matches(job->preferredHost()))
                    continue;
                if(cs->is_eligible_ever(job)) {
                    trace() << "No suitable host found, delaying" << endl;
                    return false;
                }
            }
            // This means that there's nobody who could possibly handle the job,
            // so there's no point in delaying.
            log_info() << "No suitable host found, assigning submitter" << endl;
            use_cs = job->submitter();
            break;
        }
    }

    remove_job_request( jobPosition );

    job->setState(Job::WAITINGFORCS);
    job->setServer(use_cs);

    string host_platform = envs_match(use_cs, job);
    bool gotit = true;

    if (host_platform.empty()) {
        gotit = false;
        host_platform = use_cs->can_install(job);
    }

    // mix and match between job ids
    unsigned matched_job_id = 0;
    unsigned count = 0;

    list<JobStat> lastRequestedJobs = job->submitter()->lastRequestedJobs();
    for (list<JobStat>::const_iterator l = lastRequestedJobs.begin();
            l != lastRequestedJobs.end(); ++l) {
        unsigned rcount = 0;

        list<JobStat> lastCompiledJobs = use_cs->lastCompiledJobs();
        for (list<JobStat>::const_iterator r = lastCompiledJobs.begin();
                r != lastCompiledJobs.end(); ++r) {
            if (l->jobId() == r->jobId()) {
                matched_job_id = l->jobId();
            }

            if (++rcount > 16) {
                break;
            }
        }

        if (matched_job_id || (++count > 16)) {
            break;
        }
    }
    if(IS_PROTOCOL_VERSION(37, job->submitter()) && use_cs == job->submitter())
    {
        NoCSMsg m2(job->id(), job->localClientId());
        if (!job->submitter()->send_msg(m2)) {
            trace() << "failed to deliver job " << job->id() << endl;
            handle_end(job->submitter(), nullptr);   // will care for the rest
            return true;
        }
    }
    else
    {
        UseCSMsg m2(host_platform, use_cs->name, use_cs->remotePort(), job->id(),
                gotit, job->localClientId(), matched_job_id);
        if (!job->submitter()->send_msg(m2)) {
            trace() << "failed to deliver job " << job->id() << endl;
            handle_end(job->submitter(), nullptr);   // will care for the rest
            return true;
        }
    }


#if DEBUG_SCHEDULER >= 0
    if (!gotit) {
        trace() << "put " << job->id() << " in joblist of " << use_cs->nodeName() << " (will install now)" << endl;
    } else {
        trace() << "put " << job->id() << " in joblist of " << use_cs->nodeName() << endl;
    }
#endif
    use_cs->appendJob(job);

    /* if it doesn't have the environment, it will get it. */
    if (!gotit) {
        use_cs->setBusyInstalling(time(nullptr));
    }

    string env;

    if (!job->masterJobFor().empty()) {
        Environments environments = job->environments();
        for (Environments::const_iterator it = environments.begin(); it != environments.end(); ++it) {
            if (it->first == use_cs->hostPlatform()) {
                env = it->second;
                break;
            }
        }
    }

    if (!env.empty()) {
        list<Job *> masterJobFor = job->masterJobFor();
        for (Job * const jobTmp : masterJobFor) {
            // remove all other environments
            jobTmp->clearEnvironments();
            jobTmp->appendEnvironment(make_pair(use_cs->hostPlatform(), env));
        }
    }

    return true;
}

static bool handle_login(CompileServer *cs, Msg *_m)
{
    LoginMsg *m = dynamic_cast<LoginMsg *>(_m);

    if (!m) {
        return false;
    }

    std::ostream &dbg = trace();

    cs->setRemotePort(m->port);
    cs->setCompilerVersions(m->envs);
    cs->setMaxJobs(m->max_kids);
    cs->setNoRemote(m->noremote);

    if (m->nodename.length()) {
        cs->setNodeName(m->nodename);
    } else {
        cs->setNodeName(cs->name);
    }

    cs->setHostPlatform(m->host_platform);
    cs->setChrootPossible(m->chroot_possible);
    cs->setSupportedFeatures(m->supported_features);
    cs->pick_new_id();

    for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it)
        if (cs->matches(*it)) {
            return false;
        }

    dbg << "login " << m->nodename << " protocol version: " << cs->protocol
        << " features: " << supported_features_to_string(m->supported_features)
        << " [";
    for (Environments::const_iterator it = m->envs.begin(); it != m->envs.end(); ++it) {
        dbg << it->second << "(" << it->first << "), ";
    }
    dbg << "]" << endl;

    handle_monitor_stats(cs);

    /* remove any other clients with the same IP and name, they must be stale */
    for (list<CompileServer *>::iterator it = css.begin(); it != css.end();) {
        if (cs->eq_ip(*(*it)) && cs->nodeName() == (*it)->nodeName()) {
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        }

        ++it;
    }

    css.push_back(cs);

    /* Configure the daemon */
    if (IS_PROTOCOL_VERSION(24, cs)) {
        cs->send_msg(ConfCSMsg());
    }

    return true;
}

static bool handle_relogin(MsgChannel *mc, Msg *_m)
{
    LoginMsg *m = dynamic_cast<LoginMsg *>(_m);

    if (!m) {
        return false;
    }

    CompileServer *cs = static_cast<CompileServer *>(mc);
    cs->setCompilerVersions(m->envs);
    cs->setBusyInstalling(0);

    std::ostream &dbg = trace();
    dbg << "RELOGIN " << cs->nodeName() << "(" << cs->hostPlatform() << "): [";

    for (Environments::const_iterator it = m->envs.begin(); it != m->envs.end(); ++it) {
        dbg << it->second << "(" << it->first << "), ";
    }

    dbg << "]" << endl;

    /* Configure the daemon */
    if (IS_PROTOCOL_VERSION(24, cs)) {
        cs->send_msg(ConfCSMsg());
    }

    return false;
}

static bool handle_mon_login(CompileServer *cs, Msg *_m)
{
    MonLoginMsg *m = dynamic_cast<MonLoginMsg *>(_m);

    if (!m) {
        return false;
    }

    monitors.push_back(cs);
    // monitors really want to be fed lazily
    cs->setBulkTransfer();

    for (list<CompileServer *>::const_iterator it = css.begin(); it != css.end(); ++it) {
        handle_monitor_stats(*it);
    }

    fd2cs.erase(cs->fd);   // no expected data from them
    return true;
}

static bool handle_job_begin(CompileServer *cs, Msg *_m)
{
    JobBeginMsg *m = dynamic_cast<JobBeginMsg *>(_m);

    if (!m) {
        return false;
    }

    if (jobs.find(m->job_id) == jobs.end()) {
        trace() << "handle_job_begin: no valid job id " << m->job_id << endl;
        return false;
    }

    Job *job = jobs[m->job_id];

    if (job->server() != cs) {
        trace() << "that job isn't handled by " << cs->name << endl;
        return false;
    }

    cs->setClientCount(m->client_count);

    job->setState(Job::COMPILING);
    job->setStartTime(m->stime);
    job->setStartOnScheduler(time(nullptr));
    notify_monitors(new MonJobBeginMsg(m->job_id, m->stime, cs->hostId()));
#if DEBUG_SCHEDULER >= 0
    trace() << "BEGIN: " << m->job_id << " client=" << job->submitter()->nodeName()
            << "(" << job->targetPlatform() << ")" << " server="
            << job->server()->nodeName() << "(" << job->server()->hostPlatform()
            << ")" << endl;
#endif

    return true;
}


static bool handle_job_done(CompileServer *cs, Msg *_m)
{
    JobDoneMsg *m = dynamic_cast<JobDoneMsg *>(_m);

    if (!m) {
        return false;
    }

    Job *j = nullptr;

    if (uint32_t clientId = m->unknown_job_client_id()) {
        // The daemon has sent a done message for a job for which it doesn't know the job id (happens
        // if the job is cancelled before we send back the job id). Find the job using the client id.
        map<unsigned int, Job *>::iterator mit;

        for (mit = jobs.begin(); mit != jobs.end(); ++mit) {
            Job *job = mit->second;
            trace() << "looking for waitcs " << job->server() << " " << job->submitter()  << " " << cs
                    << " " << job->state() << " " << job->localClientId() << " " << clientId
                    << endl;

            if (job->server() == nullptr && job->submitter() == cs && job->localClientId() == clientId) {
                trace() << "STOP (WAITFORCS) FOR " << mit->first << endl;
                j = job;
                m->set_job_id( j->id()); // Now we know the job's id.

                /* Unfortunately the job_requests queues are also tagged based on the daemon,
                so we need to clean them up also.  */
                list<JobRequestsGroup *>::iterator it;

                for (it = job_requests.begin(); it != job_requests.end(); ++it)
                    if ((*it)->submitter == cs) {
                        JobRequestsGroup *l = *it;
                        list<Job *>::iterator jit;

                        for (jit = l->l.begin(); jit != l->l.end(); ++jit) {
                            if (*jit == j) {
                                l->l.erase(jit);
                                break;
                            }
                        }

                        if (l->l.empty()) {
                            it = job_requests.erase(it);
                            break;
                        }
                    }
            }
        }
    } else if (jobs.find(m->job_id) != jobs.end()) {
        j = jobs[m->job_id];
    }

    if (!j) {
        trace() << "job ID not present " << m->job_id << endl;
        return false;
    }

    if (m->is_from_server() && (j->server() != cs)) {
        log_info() << "the server isn't the same for job " << m->job_id << endl;
        log_info() << "server: " << j->server()->nodeName() << endl;
        log_info() << "msg came from: " << cs->nodeName() << endl;
        // the daemon is not following matz's rules: kick him
        handle_end(cs, nullptr);
        return false;
    }

    if (!m->is_from_server() && (j->submitter() != cs)) {
        log_info() << "the submitter isn't the same for job " << m->job_id << endl;
        log_info() << "submitter: " << j->submitter()->nodeName() << endl;
        log_info() << "msg came from: " << cs->nodeName() << endl;
        // the daemon is not following matz's rules: kick him
        handle_end(cs, nullptr);
        return false;
    }

    cs->setClientCount(m->client_count);

    if (m->exitcode == 0) {
        std::ostream &dbg = trace();
        dbg << "END " << m->job_id
            << " status=" << m->exitcode;

        if (m->in_uncompressed)
            dbg << " in=" << m->in_uncompressed
                << "(" << int(m->in_compressed * 100 / m->in_uncompressed) << "%)";
        else {
            dbg << " in=0(0%)";
        }

        if (m->out_uncompressed)
            dbg << " out=" << m->out_uncompressed
                << "(" << int(m->out_compressed * 100 / m->out_uncompressed) << "%)";
        else {
            dbg << " out=0(0%)";
        }

        dbg << " real=" << m->real_msec
            << " user=" << m->user_msec
            << " sys=" << m->sys_msec
            << " pfaults=" << m->pfaults
            << " server=" << j->server()->nodeName()
            << endl;
    } else {
        trace() << "END " << m->job_id
                << " status=" << m->exitcode << endl;
    }

    if (j->server()) {
        j->server()->removeJob(j);
    }

    add_job_stats(j, m);
    notify_monitors(new MonJobDoneMsg(*m));
    jobs.erase(m->job_id);
    delete j;

    return true;
}

static bool handle_ping(CompileServer *cs, Msg * /*_m*/)
{
    cs->last_talk = time(nullptr);

    if (cs->maxJobs() < 0) {
        cs->setMaxJobs(cs->maxJobs() * -1);
    }

    return true;
}

static bool handle_stats(CompileServer *cs, Msg *_m)
{
    StatsMsg *m = dynamic_cast<StatsMsg *>(_m);

    if (!m) {
        return false;
    }

    /* Before protocol 25, ping and stat handling was
       clutched together.  */
    if (!IS_PROTOCOL_VERSION(25, cs)) {
        cs->last_talk = time(nullptr);

        if (cs->maxJobs() < 0) {
            cs->setMaxJobs(cs->maxJobs() * -1);
        }
    }

    for (CompileServer * const c : css)
        if (c == cs) {
            c->setLoad(m->load);
            c->setClientCount(m->client_count);
            handle_monitor_stats(c, m);
            return true;
        }

    return false;
}

static bool handle_blacklist_host_env(CompileServer *cs, Msg *_m)
{
    BlacklistHostEnvMsg *m = dynamic_cast<BlacklistHostEnvMsg *>(_m);

    if (!m) {
        return false;
    }

    for (list<CompileServer *>::const_iterator it = css.begin(); it != css.end(); ++it)
        if ((*it)->name == m->hostname) {
            trace() << "Blacklisting host " << m->hostname << " for environment " << m->environment
                    << " (" << m->target << ")" << endl;
            cs->blacklistCompileServer(*it, make_pair(m->target, m->environment));
        }

    return true;
}

static string dump_job(Job *job)
{
    char buffer[1000];
    string line;

    string jobState;
    switch(job->state()) {
    case Job::PENDING:
        jobState = "PEND";
        break;
    case Job::WAITINGFORCS:
        jobState = "WAIT";
        break;
    case Job::COMPILING:
        jobState = "COMP";
        break;
    default:
        jobState = "Huh?";
    }
    snprintf(buffer, sizeof(buffer), "%u %s sub:%s on:%s ",
             job->id(),
             jobState.c_str(),
             job->submitter() ? job->submitter()->nodeName().c_str() : "<>",
             job->server() ? job->server()->nodeName().c_str() : "<unknown>");
    buffer[sizeof(buffer) - 1] = 0;
    line = buffer;
    line = line + job->fileName();
    return line;
}

/* Splits the string S between characters in SET and add them to list L.  */
static void split_string(const string &s, const char *set, list<string> &l)
{
    string::size_type end = 0;

    while (end != string::npos) {
        string::size_type start = s.find_first_not_of(set, end);

        if (start == string::npos) {
            break;
        }

        end = s.find_first_of(set, start);

        /* Do we really need to check end here or is the subtraction
           defined on every platform correctly (with GCC it's ensured,
        that (npos - start) is the rest of the string).  */
        if (end != string::npos) {
            l.push_back(s.substr(start, end - start));
        } else {
            l.push_back(s.substr(start));
        }
    }
}

static bool handle_control_login(CompileServer *cs)
{
    cs->setType(CompileServer::LINE);
    cs->last_talk = time(nullptr);
    cs->setBulkTransfer();
    cs->setState(CompileServer::LOGGEDIN);
    assert(find(controls.begin(), controls.end(), cs) == controls.end());
    controls.push_back(cs);

    std::ostringstream o;
    o << "200-ICECC " VERSION ": "
      << time(nullptr) - starttime << "s uptime, "
      << css.size() << " hosts, "
      << jobs.size() << " jobs in queue "
      << "(" << new_job_id << " total)." << endl;
    o << "200 Use 'help' for help and 'quit' to quit." << endl;
    return cs->send_msg(TextMsg(o.str()));
}

static bool handle_line(CompileServer *cs, Msg *_m)
{
    TextMsg *m = dynamic_cast<TextMsg *>(_m);

    if (!m) {
        return false;
    }

    string line;
    list<string> l;
    split_string(m->text, " \t\n", l);
    string cmd;

    cs->last_talk = time(nullptr);

    if (l.empty()) {
        cmd = "";
    } else {
        cmd = l.front();
        l.pop_front();
        transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    }

    if (cmd == "listcs") {
        for (CompileServer * const it : css) {
            char buffer[1000];
            sprintf(buffer, " (%s:%u) ", it->name.c_str(), it->remotePort());
            line = " " + it->nodeName() + buffer;
            line += "[" + it->hostPlatform() + "] speed=";
            sprintf(buffer, "%.2f jobs=%d/%d load=%u", server_speed(it),
                    it->currentJobCount(), it->maxJobs(), it->load());
            line += buffer;

            if (it->busyInstalling()) {
                sprintf(buffer, " busy installing since %ld s",  time(nullptr) - it->busyInstalling());
                line += buffer;
            }

            if (!cs->send_msg(TextMsg(line))) {
                return false;
            }

            const list<Job *>& jobList = it->jobList();
            for (list<Job *>::const_iterator it2 = jobList.begin(); it2 != jobList.end(); ++it2) {
                if (!cs->send_msg(TextMsg("   " + dump_job(*it2)))) {
                    return false;
                }
            }
        }
    } else if (cmd == "listblocks") {
        for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it) {
            if (!cs->send_msg(TextMsg("   " + (*it)))) {
                return false;
            }
        }
    } else if (cmd == "listjobs") {
        for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
                it != jobs.end(); ++it)
            if (!cs->send_msg(TextMsg(" " + dump_job(it->second)))) {
                return false;
            }
    } else if (cmd == "quit" || cmd == "exit") {
        handle_end(cs, nullptr);
        return false;
    } else if (cmd == "removecs" || cmd == "blockcs") {
        if (l.empty()) {
            if (!cs->send_msg(TextMsg(string("401 Sure. But which hosts?")))) {
                return false;
            }
        } else {
            for (list<string>::const_iterator si = l.begin(); si != l.end(); ++si) {
                if (cmd == "blockcs")
                    block_css.push_back(*si);
                for (CompileServer * const it : css) {
                    if (it->matches(*si)) {
                        if (cs->send_msg(TextMsg(string("removing host ") + *si))) {
                            handle_end(it, nullptr);
                        }
                        break;
                    }
                }
            }
        }
    } else if (cmd == "unblockcs") {
        if (l.empty()) {
            if(!cs->send_msg (TextMsg (string ("401 Sure. But which host?"))))
                return false;
        } else {
            for (list<string>::const_iterator si = l.begin(); si != l.end(); ++si) {
                for (list<string>::iterator it = block_css.begin(); it != block_css.end(); ++it) {
                    if (*si == *it) {
                        block_css.erase(it);
                        break;
                    }
                }
            }
        }
    } else if (cmd == "internals") {
        for (CompileServer * const it : css) {
            Msg *msg = nullptr;

            if (!l.empty()) {
                list<string>::const_iterator si;

                for (si = l.begin(); si != l.end(); ++si) {
                    if (it->matches(*si)) {
                        break;
                    }
                }

                if (si == l.end()) {
                    continue;
                }
            }

            if (it->send_msg(GetInternalStatus())) {
                msg = it->get_msg();
            }

            if (msg && *msg == Msg::STATUS_TEXT) {
                if (!cs->send_msg(TextMsg(dynamic_cast<StatusTextMsg *>(msg)->text))) {
                    return false;
                }
            } else {
                if (!cs->send_msg(TextMsg(it->nodeName() + " not reporting\n"))) {
                    return false;
                }
            }

            delete msg;
        }
    } else if (cmd == "help") {
        if (!cs->send_msg(TextMsg(
                             "listcs\nlistblocks\nlistjobs\nremovecs\nblockcs\nunblockcs\ninternals\nhelp\nquit"))) {
            return false;
        }
    } else {
        string txt = "Invalid command '";
        txt += m->text;
        txt += "'";

        if (!cs->send_msg(TextMsg(txt))) {
            return false;
        }
    }

    return cs->send_msg(TextMsg(string("200 done")));
}

// return false if some error occurred, leaves C open.  */
static bool try_login(CompileServer *cs, Msg *m)
{
    bool ret = true;

    switch (*m) {
    case Msg::LOGIN:
        cs->setType(CompileServer::DAEMON);
        ret = handle_login(cs, m);
        break;
    case Msg::MON_LOGIN:
        cs->setType(CompileServer::MONITOR);
        ret = handle_mon_login(cs, m);
        break;
    default:
        log_info() << "Invalid first message " << m->to_string() << endl;
        ret = false;
        break;
    }

    if (ret) {
        cs->setState(CompileServer::LOGGEDIN);
    } else {
        handle_end(cs, m);
    }

    delete m;
    return ret;
}

static bool handle_end(CompileServer *toremove, Msg *m)
{
#if DEBUG_SCHEDULER > 1
    trace() << "Handle_end " << toremove << " " << m << endl;
#else
    (void)m;
#endif

    switch (toremove->type()) {
    case CompileServer::MONITOR:
        assert(find(monitors.begin(), monitors.end(), toremove) != monitors.end());
        monitors.remove(toremove);
#if DEBUG_SCHEDULER > 1
        trace() << "handle_end(moni) " << monitors.size() << endl;
#endif
        break;
    case CompileServer::DAEMON:
        log_info() << "remove daemon " << toremove->nodeName() << endl;

        notify_monitors(new MonStatsMsg(toremove->hostId(), "State:Offline\n"));

        /* A daemon disconnected.  We must remove it from the css list,
           and we have to delete all jobs scheduled on that daemon.
        There might be still clients connected running on the machine on which
         the daemon died.  We expect that the daemon dying makes the client
         disconnect soon too.  */
        css.remove(toremove);

        /* Unfortunately the job_requests queues are also tagged based on the daemon,
           so we need to clean them up also.  */

        for (list<JobRequestsGroup *>::iterator it = job_requests.begin(); it != job_requests.end();) {
            if ((*it)->submitter == toremove) {
                JobRequestsGroup *l = *it;
                list<Job *>::iterator jit;

                for (jit = l->l.begin(); jit != l->l.end(); ++jit) {
                    trace() << "STOP (DAEMON) FOR " << (*jit)->id() << endl;
                    notify_monitors(new MonJobDoneMsg(JobDoneMsg((*jit)->id(),  255)));

                    if ((*jit)->server()) {
                        (*jit)->server()->setBusyInstalling(0);
                    }

                    jobs.erase((*jit)->id());
                    delete(*jit);
                }

                delete l;
                it = job_requests.erase(it);
            } else {
                ++it;
            }
        }

        for (map<unsigned int, Job *>::iterator mit = jobs.begin(); mit != jobs.end();) {
            Job *job = mit->second;

            if (job->server() == toremove || job->submitter() == toremove) {
                trace() << "STOP (DAEMON2) FOR " << mit->first << endl;
                notify_monitors(new MonJobDoneMsg(JobDoneMsg(job->id(),  255)));

                /* If this job is removed because the submitter is removed
                also remove the job from the servers joblist.  */
                if (job->server() && job->server() != toremove) {
                    job->server()->removeJob(job);
                }

                if (job->server()) {
                    job->server()->setBusyInstalling(0);
                }

                jobs.erase(mit++);
                delete job;
            } else {
                ++mit;
            }
        }

        for (CompileServer * const cs : css) {
            cs->eraseCSFromBlacklist(toremove);
        }

        break;
    case CompileServer::LINE:
        toremove->send_msg(TextMsg("200 Good Bye!"));
        controls.remove(toremove);

        break;
    default:
        trace() << "remote end had UNKNOWN type?" << endl;
        break;
    }

    fd2cs.erase(toremove->fd);
    delete toremove;
    return true;
}

/* Returns TRUE if C was not closed.  */
static bool handle_activity(CompileServer *cs)
{
    Msg *m;
    bool ret = true;
    m = cs->get_msg(0, true);

    if (!m) {
        handle_end(cs, m);
        return false;
    }

    /* First we need to login.  */
    if (cs->state() == CompileServer::CONNECTED) {
        return try_login(cs, m);
    }

    switch (*m) {
    case Msg::JOB_BEGIN:
        ret = handle_job_begin(cs, m);
        break;
    case Msg::JOB_DONE:
        ret = handle_job_done(cs, m);
        break;
    case Msg::PING:
        ret = handle_ping(cs, m);
        break;
    case Msg::STATS:
        ret = handle_stats(cs, m);
        break;
    case Msg::END:
        handle_end(cs, m);
        ret = false;
        break;
    case Msg::JOB_LOCAL_BEGIN:
        ret = handle_local_job(cs, m);
        break;
    case Msg::JOB_LOCAL_DONE:
        ret = handle_local_job_done(cs, m);
        break;
    case Msg::LOGIN:
        ret = handle_relogin(cs, m);
        break;
    case Msg::TEXT:
        ret = handle_line(cs, m);
        break;
    case Msg::GET_CS:
        ret = handle_cs_request(cs, m);
        break;
    case Msg::BLACKLIST_HOST_ENV:
        ret = handle_blacklist_host_env(cs, m);
        break;
    default:
        log_info() << "Invalid message type arrived " << m->to_string() << endl;
        handle_end(cs, m);
        ret = false;
        break;
    }

    delete m;
    return ret;
}

static int open_broad_listener(int port, const string &interface)
{
    int listen_fd;
    struct sockaddr_in myaddr;

    if ((listen_fd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        log_perror("socket()");
        return -1;
    }

    int optval = 1;

    if (setsockopt(listen_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0) {
        log_perror("setsockopt()");
        return -1;
    }

    if (!build_address_for_interface(myaddr, interface, port)) {
        return -1;
    }

    if (::bind(listen_fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return -1;
    }

    return listen_fd;
}

static int open_tcp_listener(short port, const string &interface)
{
    int fd;
    struct sockaddr_in myaddr;

    if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("socket()");
        return -1;
    }

    int optval = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror("setsockopt()");
        return -1;
    }

    /* Although we poll() on fd we need O_NONBLOCK, due to
       possible network errors making accept() block although poll() said
       there was some activity.  */
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        log_perror("fcntl()");
        return -1;
    }

    if (!build_address_for_interface(myaddr, interface, port)) {
        return -1;
    }

    if (::bind(fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return -1;
    }

    if (listen(fd, 1024) < 0) {
        log_perror("listen()");
        return -1;
    }

    return fd;
}

static void usage(const std::string reason = "")
{
    if (! reason.empty()) {
        cerr << reason << endl;
    }

    cerr << "ICECREAM scheduler " VERSION "\n";
    cerr << "usage: icecc-scheduler [options] \n"
         << "Options:\n"
         << "  -n, --netname <name>\n"
         << "  -i, --interface <net_interface>\n"
         << "  -p, --port <port>\n"
         << "  -h, --help\n"
         << "  -l, --log-file <file>\n"
         << "  -d, --daemonize\n"
         << "  -u, --user-uid\n"
         << "  -v[v[v]]]\n"
         << "  -r, --persistent-client-connection\n"
         << "  -a, --algorithm <name>\n"
         << endl;

    exit(1);
}

static void trigger_exit(int signum)
{
    if (!exit_main_loop) {
        exit_main_loop = true;
    } else {
        // hmm, we got killed already. try better
        static const char msg[] = "forced exit.\n";
        ignore_result(write(STDERR_FILENO, msg, strlen( msg )));
        _exit(1);
    }

    // make BSD happy
    signal(signum, trigger_exit);
}

static void handle_scheduler_announce(const char* buf, const char* netname, bool persistent_clients, struct sockaddr_in broad_addr)
{
    /* Another scheduler is announcing it's running, disconnect daemons if it has a better version
       or the same version but was started earlier. */
    time_t other_time;
    int other_protocol_version;
    string other_netname;
    Broadcasts::getSchedulerVersionData(buf, &other_protocol_version, &other_time, &other_netname);
    trace() << "Received scheduler announcement from " << inet_ntoa(broad_addr.sin_addr)
            << ":" << ntohs(broad_addr.sin_port)
            << " (version " << int(other_protocol_version) << ", netname " << other_netname << ")" << endl;
    if (other_protocol_version >= 36)
    {
        if (other_netname == netname)
        {
            if (other_protocol_version > PROTOCOL_VERSION || (other_protocol_version == PROTOCOL_VERSION && other_time < starttime))
            {
                if (!persistent_clients){
                    log_info() << "Scheduler from " << inet_ntoa(broad_addr.sin_addr)
                        << ":" << ntohs(broad_addr.sin_port)
                        << " (version " << int(other_protocol_version) << ") has announced itself as a preferred"
                        " scheduler, disconnecting all connections." << endl;
                    if (!css.empty() || !monitors.empty())
                    {
                        while (!css.empty())
                        {
                            handle_end(css.front(), nullptr);
                        }
                        while (!monitors.empty())
                        {
                            handle_end(monitors.front(), nullptr);
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char *argv[])
{
    int listen_fd, remote_fd, broad_fd, text_fd;
    struct sockaddr_in remote_addr;
    socklen_t remote_len;
    const char *netname = "ICECREAM";
    bool detach = false;
    bool persistent_clients = false;
    int debug_level = Error;
    string logfile;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno = 0;
    SchedulerAlgorithmName scheduler_algo = SchedulerAlgorithmName::FASTEST;

    if (getuid() == 0) {
        struct passwd *pw = getpwnam("icecc");

        if (pw) {
            user_uid = pw->pw_uid;
            user_gid = pw->pw_gid;
        } else {
            warn_icecc_user_errno = errno ? errno : ENOENT; // apparently errno can be 0 on error here
            user_uid = 65534;
            user_gid = 65533;
        }
    } else {
        user_uid = getuid();
        user_gid = getgid();
    }

    while (true) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, nullptr, 'n' },
            { "help", 0, nullptr, 'h' },
            { "persistent-client-connection", 0, nullptr, 'r' },
            { "interface", 1, nullptr, 'i' },
            { "port", 1, nullptr, 'p' },
            { "daemonize", 0, nullptr, 'd'},
            { "log-file", 1, nullptr, 'l'},
            { "user-uid", 1, nullptr, 'u'},
            { "algorithm", 1, nullptr, 'a' },
            { nullptr, 0, nullptr, 0 }
        };

        const int c = getopt_long(argc, argv, "n:i:p:hl:vdru:a:", long_options, &option_index);

        if (c == -1) {
            break;    // eoo
        }

        switch (c) {
        case 0:
            (void) long_options[option_index].name;
            break;
        case 'd':
            detach = true;
            break;
        case 'r':
            persistent_clients= true;
            break;
        case 'l':
            if (optarg && *optarg) {
                logfile = optarg;
            } else {
                usage("Error: -l requires argument");
            }

            break;
        case 'v':

            if (debug_level < MaxVerboseLevel) {
                debug_level++;
            }

            break;
        case 'n':

            if (optarg && *optarg) {
                netname = optarg;
            } else {
                usage("Error: -n requires argument");
            }

            break;
        case 'i':

            if (optarg && *optarg) {
                string interface = optarg;
                if (interface.empty()) {
                    usage("Error: Invalid network interface specified");
                }

                scheduler_interface = interface;
            } else {
                usage("Error: -i requires argument");
            }

            break;
        case 'p':

            if (optarg && *optarg) {
                scheduler_port = atoi(optarg);

                if (0 == scheduler_port) {
                    usage("Error: Invalid port specified");
                }
            } else {
                usage("Error: -p requires argument");
            }

            break;
        case 'u':

            if (optarg && *optarg) {
                struct passwd *pw = getpwnam(optarg);

                if (!pw) {
                    usage("Error: -u requires a valid username");
                } else {
                    user_uid = pw->pw_uid;
                    user_gid = pw->pw_gid;
                    warn_icecc_user_errno = 0;

                    if (!user_gid || !user_uid) {
                        usage("Error: -u <username> must not be root");
                    }
                }
            } else {
                usage("Error: -u requires a valid username");
            }

            break;
        case 'a':

            if (optarg && *optarg) {
                string algorithm_name = optarg;
                std::transform(
                        algorithm_name.begin(),
                        algorithm_name.end(),
                        algorithm_name.begin(),
                        ::tolower);

                if (algorithm_name == "random") {
                    scheduler_algo = SchedulerAlgorithmName::RANDOM;
                } else if (algorithm_name == "round_robin") {
                    scheduler_algo = SchedulerAlgorithmName::ROUND_ROBIN;
                } else if (algorithm_name == "least_busy") {
                    scheduler_algo = SchedulerAlgorithmName::LEAST_BUSY;
                } else if (algorithm_name == "fastest") {
                    scheduler_algo = SchedulerAlgorithmName::FASTEST;
                } else {
                    usage("Error: " + algorithm_name + " is an unknown scheduler algorithm.");
                }

            } else {
                usage("Error: -s requires a valid scheduler name");
            }

            break;

        default:
            usage();
        }
    }

    if (warn_icecc_user_errno != 0) {
        log_errno("No icecc user on system. Falling back to nobody.", errno);
    }

    if (getuid() == 0) {
        if (!logfile.size() && detach) {
            if (mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
                if (errno == EEXIST) {
                    if (-1 == chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)){
                        log_perror("chmod() failure");
                    }

                    if (-1 == chown("/var/log/icecc", user_uid, user_gid)){
                        log_perror("chown() failure");
                    }
                }
            }

            logfile = "/var/log/icecc/scheduler.log";
        }

        if (setgroups(0, nullptr) < 0) {
            log_perror("setgroups() failed");
            return 1;
        }

        if (setgid(user_gid) < 0) {
            log_perror("setgid() failed");
            return 1;
        }

        if (setuid(user_uid) < 0) {
            log_perror("setuid() failed");
            return 1;
        }
    }

    setup_debug(debug_level, logfile);

    log_info() << "ICECREAM scheduler " VERSION " starting up, port " << scheduler_port << endl;
    log_info() << "Debug level: " << debug_level << endl;

    if (detach) {
        if (daemon(0, 0) != 0) {
            log_errno("Failed to detach.", errno);
            exit(1);
        }
    }

    listen_fd = open_tcp_listener(scheduler_port, scheduler_interface);

    if (listen_fd < 0) {
        return 1;
    }

    text_fd = open_tcp_listener(scheduler_port + 1, scheduler_interface);

    if (text_fd < 0) {
        return 1;
    }

    broad_fd = open_broad_listener(scheduler_port, scheduler_interface);

    if (broad_fd < 0) {
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        return 1;
    }

    starttime = time(nullptr);
    if( getenv( "ICECC_FAKE_STARTTIME" ) != nullptr )
        starttime -= 1000;

    ofstream pidFile;
    string progName = argv[0];
    progName = find_basename(progName);
    pidFilePath = string(RUNDIR) + string("/") + progName + string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << getpid() << endl;
    pidFile.close();

    signal(SIGTERM, trigger_exit);
    signal(SIGINT, trigger_exit);
    signal(SIGALRM, trigger_exit);

    log_info() << "scheduler ready, algorithm: " <<  scheduler_algo << endl;

    time_t next_listen = 0;

    Broadcasts::broadcastSchedulerVersion(scheduler_port, netname, starttime);
    last_announce = starttime;

    while (!exit_main_loop) {
        int timeout = prune_servers();

        while (empty_queue(scheduler_algo)) {
            continue;
        }

        /* Announce ourselves from time to time, to make other possible schedulers disconnect
           their daemons if we are the preferred scheduler (daemons with version new enough
           should automatically select the best scheduler, but old daemons connect randomly). */
        if (last_announce + 120 < time(nullptr)) {
            Broadcasts::broadcastSchedulerVersion(scheduler_port, netname, starttime);
            last_announce = time(nullptr);
        }

        vector< pollfd > pollfds;
        pollfds.reserve( fd2cs.size() + css.size() + 5 );
        pollfd pfd; // tmp variable

        if (time(nullptr) >= next_listen) {
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            pollfds.push_back( pfd );

            pfd.fd = text_fd;
            pfd.events = POLLIN;
            pollfds.push_back( pfd );
        }

        pfd.fd = broad_fd;
        pfd.events = POLLIN;
        pollfds.push_back( pfd );

        for (map<int, CompileServer *>::const_iterator it = fd2cs.begin(); it != fd2cs.end();) {
            int i = it->first;
            CompileServer *cs = it->second;
            bool ok = true;
            ++it;

            /* handle_activity() can delete c and make the iterator
               invalid.  */
            while (ok && cs->has_msg()) {
                if (!handle_activity(cs)) {
                    ok = false;
                }
            }

            if (ok) {
                pfd.fd = i;
                pfd.events = POLLIN;
                pollfds.push_back( pfd );
            }
        }

        list<CompileServer *> cs_in_tsts;
        for (CompileServer * const cs : css)
        {
            if (cs->getConnectionInProgress())
            {
                int csInFd = cs->getInFd();
                cs_in_tsts.push_back(cs);
                pfd.fd = csInFd;
                pfd.events = POLLIN | POLLOUT;
                pollfds.push_back( pfd );
            }
        }

        int active_fds = poll(pollfds.data(), pollfds.size(), timeout * 1000);
        int poll_errno = errno;

        if (active_fds < 0 && errno == EINTR) {
            reset_debug_if_needed(); // we possibly got SIGHUP
            continue;
        }
        reset_debug_if_needed();

        if (active_fds < 0) {
            errno = poll_errno;
            log_perror("poll()");
            return 1;
        }

        if (pollfd_is_set(pollfds, listen_fd, POLLIN)) {
            active_fds--;
            bool pending_connections = true;

            while (pending_connections) {
                remote_len = sizeof(remote_addr);
                remote_fd = accept(listen_fd,
                                   (struct sockaddr *) &remote_addr,
                                   &remote_len);

                if (remote_fd < 0) {
                    pending_connections = false;
                }

                if (remote_fd < 0 && errno != EAGAIN && errno != EINTR
                        && errno != EWOULDBLOCK) {
                    log_perror("accept()");
                    /* don't quit because of ECONNABORTED, this can happen during
                     * floods  */
                }

                if (remote_fd >= 0) {
                    CompileServer *cs = new CompileServer(remote_fd, (struct sockaddr *) &remote_addr, remote_len, false);
                    trace() << "accepted " << cs->name << endl;
                    cs->last_talk = time(nullptr);

                    if (!cs->protocol) { // protocol mismatch
                        delete cs;
                        continue;
                    }

                    fd2cs[cs->fd] = cs;

                    while (!cs->read_a_bit() || cs->has_msg()) {
                        if (! handle_activity(cs)) {
                            break;
                        }
                    }
                }
            }

            next_listen = time(nullptr) + 1;
        }

        if (active_fds && pollfd_is_set(pollfds, text_fd, POLLIN)) {
            active_fds--;
            remote_len = sizeof(remote_addr);
            remote_fd = accept(text_fd,
                               (struct sockaddr *) &remote_addr,
                               &remote_len);

            if (remote_fd < 0 && errno != EAGAIN && errno != EINTR) {
                log_perror("accept()");
                /* Don't quit the scheduler just because a debugger couldn't
                   connect.  */
            }

            if (remote_fd >= 0) {
                CompileServer *cs = new CompileServer(remote_fd, (struct sockaddr *) &remote_addr, remote_len, true);
                fd2cs[cs->fd] = cs;

                if (!handle_control_login(cs)) {
                    handle_end(cs, nullptr);
                    continue;
                }

                while (!cs->read_a_bit() || cs->has_msg())
                    if (!handle_activity(cs)) {
                        break;
                    }
            }
        }

        if (active_fds && pollfd_is_set(pollfds, broad_fd, POLLIN)) {
            active_fds--;
            char buf[Broadcasts::BROAD_BUFLEN + 1];
            struct sockaddr_in broad_addr;
            socklen_t broad_len = sizeof(broad_addr);
            /* We can get either a daemon request for a scheduler (1 byte) or another scheduler
               announcing itself (4 bytes + time). */

            int buflen = recvfrom(broad_fd, buf, Broadcasts::BROAD_BUFLEN, 0, (struct sockaddr *) &broad_addr,
                    &broad_len);
            if (buflen < 0 || buflen > Broadcasts::BROAD_BUFLEN){
                int err = errno;
                log_perror("recvfrom()");

                /* Some linux 2.6 kernels can return from select with
                   data available, and then return from read() with EAGAIN
                   even on a blocking socket (breaking POSIX).  Happens
                   when the arriving packet has a wrong checksum.  So
                   we ignore EAGAIN here, but still abort for all other errors. */
                if (err != EAGAIN && err != EWOULDBLOCK) {
                    return -1;
                }
            }
            int daemon_version;
            if (DiscoverSched::isSchedulerDiscovery(buf, buflen, &daemon_version)) {
                /* Daemon is searching for a scheduler, only answer if daemon would be able to talk to us. */
                if ( daemon_version >= MIN_PROTOCOL_VERSION){
                    log_info() << "broadcast from " << inet_ntoa(broad_addr.sin_addr)
                        << ":" << ntohs(broad_addr.sin_port)
                        << " (version " << daemon_version << ")\n";
                    int reply_len = DiscoverSched::prepareBroadcastReply(buf, netname, starttime);
                    if (sendto(broad_fd, buf, reply_len, 0,
                                (struct sockaddr *) &broad_addr, broad_len) != reply_len) {
                        log_perror("sendto()");
                    }
                }
            }
            else if(Broadcasts::isSchedulerVersion(buf, buflen)) {
                handle_scheduler_announce(buf, netname, persistent_clients, broad_addr);
            }
        }

        for (map<int, CompileServer *>::const_iterator it = fd2cs.begin();
                active_fds > 0 && it != fd2cs.end();) {
            int i = it->first;
            CompileServer *cs = it->second;
            /* handle_activity can delete the channel from the fd2cs list,
               hence advance the iterator right now, so it doesn't become
               invalid.  */
            ++it;

            if (pollfd_is_set(pollfds, i, POLLIN)) {
                while (!cs->read_a_bit() || cs->has_msg()) {
                    if (!handle_activity(cs)) {
                        break;
                    }
                }

                active_fds--;
            }
        }

        for (list<CompileServer *>::const_iterator it = cs_in_tsts.begin();
                it != cs_in_tsts.end(); ++it) {
            if(find(css.begin(), css.end(), *it) == css.end()) {
                continue; // deleted meanwhile
            }
            if((*it)->getConnectionInProgress())
            {
                if(active_fds > 0 && pollfd_is_set(pollfds, (*it)->getInFd(), POLLIN | POLLOUT) && (*it)->isConnected())
                {
                    active_fds--;
                    (*it)->updateInConnectivity(true);
                }
                else if((active_fds == 0 || pollfd_is_set(pollfds, (*it)->getInFd(), POLLIN | POLLOUT)) && !(*it)->isConnected())
                {
                    (*it)->updateInConnectivity(false);
                }
            }
        }
    }

    shutdown(broad_fd, SHUT_RDWR);
    while (!css.empty())
        handle_end(css.front(), nullptr);
    while (!monitors.empty())
        handle_end(monitors.front(), nullptr);
    if ((-1 == close(broad_fd)) && (errno != EBADF)){
        log_perror("close failed");
    }
    if (-1 == unlink(pidFilePath.c_str()) && errno != ENOENT){
        log_perror("unlink failed") << "\t" << pidFilePath << endl;
    }
    return 0;
}
