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
#include <sys/select.h>
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
#include <stdio.h>
#include <pwd.h>
#include "../services/comm.h"
#include "../services/logging.h"
#include "../services/job.h"
#include "config.h"

#include "compileserver.h"
#include "job.h"

#define DEBUG_SCHEDULER 0

/* TODO:
   * leak check
   * are all filedescs closed when done?
   * simplify livetime of the various structures (Jobs/Channels/CompileServers know
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
struct UnansweredList {
    list<Job *> l;
    CompileServer *server;
    bool remove_job(Job *);
};
static list<UnansweredList *> toanswer;

static list<JobStat> all_job_stats;
static JobStat cum_job_stats;

static float server_speed(CompileServer *cs, Job *job = 0);
static void broadcast_scheduler_version();

/* Searches the queue for JOB and removes it.
   Returns true if something was deleted.  */
bool UnansweredList::remove_job(Job *job)
{
    list<Job *>::iterator it;

    for (it = l.begin(); it != l.end(); ++it)
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
                << server_speed(job->server()) << endl;
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
            handle_end(*it_old, 0);
        }
    }

    delete m;
}

static float server_speed(CompileServer *cs, Job *job)
{
    if (cs->lastCompiledJobs().size() == 0 || cs->cumCompiled().compileTimeUser() == 0) {
        return 0;
    } else {
        float f = (float)cs->cumCompiled().outputSize()
                  / (float) cs->cumCompiled().compileTimeUser();

        // we only care for the load if we're about to add a job to it
        if (job) {
            if (job->submitter() == cs) {
                /* The submitter of a job gets more speed if it's capable of handling its requests on its own.
                   So if he is equally fast to the rest of the farm it will be preferred to chose him
                   to compile the job.  Then this can be done locally without needing the preprocessor.
                   However if there are more requests than the number of jobs the submitter can handle,
                   it is assumed the submitter is doing a massively parallel build, in which case it is
                   better not to build on the submitter and let it do other work (such as preprocessing
                   output for other nodes) that can be done only locally.  */
                if (cs->submittedJobsCount() <= cs->maxJobs()) {
                    f *= 1.1;
                } else {
                    f *= 0.1;    // penalize heavily
                }
            } else { // ignoring load for submitter - assuming the load is our own
                f *= float(1000 - cs->load()) / 1000;
            }
        }

        // below we add a pessimism factor - assuming the first job a computer got is not representative
        if (cs->lastCompiledJobs().size() < 7) {
            f *= (-0.5 * cs->lastCompiledJobs().size() + 4.5);
        }

        return f;
    }
}

static void handle_monitor_stats(CompileServer *cs, StatsMsg *m = 0)
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
    sprintf(buffer, "Speed:%f\n", server_speed(cs));
    msg += buffer;

    if (m) {
        sprintf(buffer, "Load:%d\n", m->load);
        msg += buffer;
        sprintf(buffer, "LoadAvg1:%d\n", m->loadAvg1);
        msg += buffer;
        sprintf(buffer, "LoadAvg5:%d\n", m->loadAvg5);
        msg += buffer;
        sprintf(buffer, "LoadAvg10:%d\n", m->loadAvg10);
        msg += buffer;
        sprintf(buffer, "FreeMem:%d\n", m->freeMem);
        msg += buffer;
    } else {
        sprintf(buffer, "Load:%d\n", cs->load());
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
    if (!toanswer.empty() && toanswer.back()->server == job->submitter()) {
        toanswer.back()->l.push_back(job);
    } else {
        UnansweredList *newone = new UnansweredList();
        newone->server = job->submitter();
        newone->l.push_back(job);
        toanswer.push_back(newone);
    }
}

static Job *get_job_request(void)
{
    if (toanswer.empty()) {
        return 0;
    }

    UnansweredList *first = toanswer.front();
    assert(!first->l.empty());
    return first->l.front();
}

/* Removes the first job request (the one returned by get_job_request()) */
static void remove_job_request(void)
{
    if (toanswer.empty()) {
        return;
    }

    UnansweredList *first = toanswer.front();
    toanswer.pop_front();
    first->l.pop_front();

    if (first->l.empty()) {
        delete first;
    } else {
        toanswer.push_back(first);
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

    Job *master_job = 0;

    for (unsigned int i = 0; i < m->count; ++i) {
        Job *job = create_new_job(submitter);
        job->setEnvironments(m->versions);
        job->setTargetPlatform(m->target);
        job->setArgFlags(m->arg_flags);
        job->setLanguage((m->lang == CompileJob::Lang_C) ? "C" : "C++");
        job->setFileName(m->filename);
        job->setLocalClientId(m->client_id);
        job->setPreferredHost(m->preferred_host);
        job->setMinimalHostVersion(m->minimal_host_version);
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

        dbg << "] " << m->filename << " " << job->language() << endl;
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
    trace() << "handle_local_job " << m->outfile << " " << m->id << endl;
    cs->insertClientJobId(m->id, new_job_id);
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
    notify_monitors(new JobLocalDoneMsg(cs->getClientJobId(m->job_id)));
    cs->eraseClientJobId(m->job_id);
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

static CompileServer *pick_server(Job *job)
{
#if DEBUG_SCHEDULER > 1
    trace() << "pick_server " << job->id() << " " << job->targetPlatform() << endl;
#endif

#if DEBUG_SCHEDULER > 0

    /* consistency checking for now */
    for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
        CompileServer *cs = *it;

        list<Job *> jobList = cs->jobList();
        for (list<Job *>::const_iterator it2 = jobList.begin(); it2 != jobList.end(); ++it2) {
            assert(jobs.find((*it2)->id()) != jobs.end());
        }
    }

    for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
            it != jobs.end(); ++it) {
        Job *j = it->second;

        if (j->state() == Job::COMPILING) {
            CompileServer *cs = j->server();
            list<Job *> jobList = cs->jobList();
            assert(find(jobList.begin(), jobList.end(), j) != jobList.end());
        }
    }

#endif

    /* if the user wants to test/prefer one specific daemon, we look for that one first */
    if (!job->preferredHost().empty()) {
        for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
            if ((*it)->matches(job->preferredHost()) && (*it)->is_eligible(job)) {
                return *it;
            }
        }

        return 0;
    }

    /* If we have no statistics simply use any server which is usable.  */
    if (!all_job_stats.size ()) {
        CompileServer *selected = NULL;
        int eligible_count = 0;

        for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
            if ((*it)->is_eligible( job )) {
                ++eligible_count;
                // Do not select the first one (which could be broken and so we might never get job stats),
                // but rather select randomly.
                if( random() % eligible_count == 0 )
                  selected = *it;
            }
        }

        if( selected != NULL ) {
            trace() << "no job stats - returning randomly selected " << selected->nodeName() << " load: " << selected->load() << " can install: " << selected->can_install(job) << endl;
            return selected;
        }

        return 0;
    }

    /* Now guess about the job.  First see, if this submitter already
       had other jobs.  Use them as base.  */
    JobStat guess;

    if (job->submitter()->lastRequestedJobs().size() > 0) {
        guess = job->submitter()->cumRequested()
                / job->submitter()->lastRequestedJobs().size();
    } else {
        /* Otherwise simply average over all jobs.  */
        guess = cum_job_stats / all_job_stats.size();
    }

    CompileServer *best = 0;
    // best uninstalled
    CompileServer *bestui = 0;
    // best preloadable host
    CompileServer *bestpre = 0;

    uint matches = 0;

    for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
        CompileServer *cs = *it;

        /* For now ignore overloaded servers.  */
        /* Pre-loadable (cs->jobList().size()) == (cs->maxJobs()) is checked later.  */
        if ((int(cs->jobList().size()) > cs->maxJobs()) || (cs->load() >= 1000)) {
#if DEBUG_SCHEDULER > 1
            trace() << "overloaded " << cs->nodeName() << " " << cs->jobList().size() << "/"
                    <<  cs->maxJobs() << " jobs, load:" << cs->load() << endl;
#endif
            continue;
        }

        // incompatible architecture or busy installing
        if (!cs->can_install(job).size()) {
#if DEBUG_SCHEDULER > 2
            trace() << cs->nodeName() << " can't install " << job->id() << endl;
#endif
            continue;
        }

        /* Don't use non-chroot-able daemons for remote jobs.  XXX */
        if (!cs->chrootPossible() && cs != job->submitter()) {
            trace() << cs->nodeName() << " can't use chroot\n";
            continue;
        }

        // Check if remote & if remote allowed
        if (!cs->check_remote(job)) {
            trace() << cs->nodeName() << " fails remote job check\n";
            continue;
        }


#if DEBUG_SCHEDULER > 1
        trace() << cs->nodeName() << " compiled " << cs->lastCompiledJobs().size() << " got now: " <<
                cs->jobList().size() << " speed: " << server_speed(cs, job) << " compile time " <<
                cs->cumCompiled().compileTimeUser() << " produced code " << cs->cumCompiled().outputSize() << endl;
#endif

        if ((cs->lastCompiledJobs().size() == 0) && (cs->jobList().size() == 0) && cs->maxJobs()) {
            /* Make all servers compile a job at least once, so we'll get an
               idea about their speed.  */
            if (!envs_match(cs, job).empty()) {
                best = cs;
                matches++;
            } else {
                // if there is one server that already got the environment and one that
                // hasn't compiled at all, pick the one with environment first
                bestui = cs;
            }

            break;
        }

        if (!envs_match(cs, job).empty()) {
            if (!best) {
                best = cs;
            }
            /* Search the server with the earliest projected time to compile
               the job.  (XXX currently this is equivalent to the fastest one)  */
            else if ((best->lastCompiledJobs().size() != 0)
                     && (server_speed(best, job) < server_speed(cs, job))) {
                if (int(cs->jobList().size()) < cs->maxJobs()) {
                    best = cs;
                } else {
                    bestpre = cs;
                }
            }

            matches++;
        } else {
            if (!bestui) {
                bestui = cs;
            }
            /* Search the server with the earliest projected time to compile
               the job.  (XXX currently this is equivalent to the fastest one)  */
            else if ((bestui->lastCompiledJobs().size() != 0)
                     && (server_speed(bestui, job) < server_speed(cs, job))) {
                if (int(cs->jobList().size()) < cs->maxJobs()) {
                    bestui = cs;
                } else {
                    bestpre = cs;
                }
            }
        }
    }

    // to make sure we find the fast computers at least after some time, we overwrite
    // the install rule for every 19th job - if the farm is only filled a bit
    if (bestui && ((matches < 11) && (matches < (css.size() / 3))) && ((job->id() % 19) != 0)) {
        best = 0;
    }

    if (best) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best installed " << best->nodeName() << " " <<  server_speed(best, job) << endl;
#endif
        return best;
    }

    if (bestui) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best uninstalled " << bestui->nodeName() << " " <<  server_speed(bestui, job) << endl;
#endif
        return bestui;
    }

    if (bestpre) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best preload " << bestui->nodeName() << " " <<  server_speed(bestui, job) << endl;
#endif
    }

    return bestpre;
}

/* Prunes the list of connected servers by those which haven't
   answered for a long time. Return the number of seconds when
   we have to cleanup next time. */
static time_t prune_servers()
{
    list<CompileServer *>::iterator it;

    time_t now = time(0);
    time_t min_time = MAX_SCHEDULER_PING;

    for (it = controls.begin(); it != controls.end();) {
        if ((now - (*it)->last_talk) >= MAX_SCHEDULER_PING) {
            CompileServer *old = *it;
            ++it;
            handle_end(old, 0);
            continue;
        }

        min_time = min(min_time, MAX_SCHEDULER_PING - now + (*it)->last_talk);
        ++it;
    }

    for (it = css.begin(); it != css.end();) {
        if ((*it)->busyInstalling() && ((now - (*it)->busyInstalling()) >= MAX_BUSY_INSTALLING)) {
            trace() << "busy installing for a long time - removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, 0);
            continue;
        }

        /* protocol version 27 and newer use TCP keepalive */
        if (IS_PROTOCOL_27(*it)) {
            ++it;
            continue;
        }

        if ((now - (*it)->last_talk) >= MAX_SCHEDULER_PING) {
            if ((*it)->maxJobs() >= 0) {
                trace() << "send ping " << (*it)->nodeName() << endl;
                (*it)->setMaxJobs((*it)->maxJobs() * -1);   // better not give it away

                if ((*it)->send_msg(PingMsg())) {
                    // give it MAX_SCHEDULER_PONG to answer a ping
                    (*it)->last_talk = time(0) - MAX_SCHEDULER_PING
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
            handle_end(old, 0);
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

static Job *delay_current_job()
{
    assert(!toanswer.empty());

    if (toanswer.size() == 1) {
        return 0;
    }

    UnansweredList *first = toanswer.front();
    toanswer.pop_front();
    toanswer.push_back(first);
    return get_job_request();
}

static bool empty_queue()
{
    Job *job = get_job_request();

    if (!job) {
        return false;
    }

    assert(!css.empty());

    Job *first_job = job;
    CompileServer *cs = 0;

    while (true) {
        cs = pick_server(job);

        if (cs) {
            break;
        }

        /* Ignore the load on the submitter itself if no other host could
           be found.  We only obey to its max job number.  */
        cs = job->submitter();

        if (!((int(cs->jobList().size()) < cs->maxJobs())
                && job->preferredHost().empty()
                /* This should be trivially true.  */
                && cs->can_install(job).size())) {
            job = delay_current_job();

            if ((job == first_job) || !job) { // no job found in the whole toanswer list
                trace() << "No suitable host found, delaying" << endl;
                return false;
            }
        } else {
            break;
        }
    }

    remove_job_request();

    job->setState(Job::WAITINGFORCS);
    job->setServer(cs);

    string host_platform = envs_match(cs, job);
    bool gotit = true;

    if (host_platform.empty()) {
        gotit = false;
        host_platform = cs->can_install(job);
    }

    // mix and match between job ids
    unsigned matched_job_id = 0;
    unsigned count = 0;

    list<JobStat> lastRequestedJobs = job->submitter()->lastRequestedJobs();
    for (list<JobStat>::const_iterator l = lastRequestedJobs.begin();
            l != lastRequestedJobs.end(); ++l) {
        unsigned rcount = 0;

        list<JobStat> lastCompiledJobs = cs->lastCompiledJobs();
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

    UseCSMsg m2(host_platform, cs->name, cs->remotePort(), job->id(),
                gotit, job->localClientId(), matched_job_id);

    if (!job->submitter()->send_msg(m2)) {
        trace() << "failed to deliver job " << job->id() << endl;
        handle_end(job->submitter(), 0);   // will care for the rest
        return true;
    }

#if DEBUG_SCHEDULER >= 0
    if (!gotit) {
        trace() << "put " << job->id() << " in joblist of " << cs->nodeName() << " (will install now)" << endl;
    } else {
        trace() << "put " << job->id() << " in joblist of " << cs->nodeName() << endl;
    }
#endif
    cs->appendJob(job);

    /* if it doesn't have the environment, it will get it. */
    if (!gotit) {
        cs->setBusyInstalling(time(0));
    }

    string env;

    if (!job->masterJobFor().empty()) {
        Environments environments = job->environments();
        for (Environments::const_iterator it = environments.begin(); it != environments.end(); ++it) {
            if (it->first == cs->hostPlatform()) {
                env = it->second;
                break;
            }
        }
    }

    if (!env.empty()) {
        list<Job *> masterJobFor = job->masterJobFor();
        for (list<Job *>::iterator it = masterJobFor.begin(); it != masterJobFor.end(); ++it) {
            // remove all other environments
            (*it)->clearEnvironments();
            (*it)->appendEnvironment(make_pair(cs->hostPlatform(), env));
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
    cs->pick_new_id();

    for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it)
        if (cs->matches(*it)) {
            return false;
        }

    dbg << "login " << m->nodename << " protocol version: " << cs->protocol;
#if 1
    dbg << " [";

    for (Environments::const_iterator it = m->envs.begin(); it != m->envs.end(); ++it) {
        dbg << it->second << "(" << it->first << "), ";
    }

    dbg << "]" << endl;
#endif

    handle_monitor_stats(cs);

    /* remove any other clients with the same IP and name, they must be stale */
    for (list<CompileServer *>::iterator it = css.begin(); it != css.end();) {
        if (cs->eq_ip(*(*it)) && cs->nodeName() == (*it)->nodeName()) {
            CompileServer *old = *it;
            ++it;
            handle_end(old, 0);
            continue;
        }

        ++it;
    }

    css.push_back(cs);

    /* Configure the daemon */
    if (IS_PROTOCOL_24(cs)) {
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
    if (IS_PROTOCOL_24(cs)) {
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

    job->setState(Job::COMPILING);
    job->setStartTime(m->stime);
    job->setStartOnScheduler(time(0));
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

    Job *j = 0;

    if (m->exitcode == CLIENT_WAS_WAITING_FOR_CS) {
        // the daemon saw a cancel of what he believes is waiting in the scheduler
        map<unsigned int, Job *>::iterator mit;

        for (mit = jobs.begin(); mit != jobs.end(); ++mit) {
            Job *job = mit->second;
            trace() << "looking for waitcs " << job->server() << " " << job->submitter()  << " " << cs
                    << " " << job->state() << " " << job->localClientId() << " " << m->job_id
                    << endl;

            if (job->server() == 0 && job->submitter() == cs && job->state() == Job::PENDING
                    && job->localClientId() == m->job_id) {
                trace() << "STOP (WAITFORCS) FOR " << mit->first << endl;
                j = job;
                m->job_id = j->id(); // that's faked

                /* Unfortunately the toanswer queues are also tagged based on the daemon,
                so we need to clean them up also.  */
                list<UnansweredList *>::iterator it;

                for (it = toanswer.begin(); it != toanswer.end(); ++it)
                    if ((*it)->server == cs) {
                        UnansweredList *l = *it;
                        list<Job *>::iterator jit;

                        for (jit = l->l.begin(); jit != l->l.end(); ++jit) {
                            if (*jit == j) {
                                l->l.erase(jit);
                                break;
                            }
                        }

                        if (l->l.empty()) {
                            it = toanswer.erase(it);
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

    if (j->state() == Job::PENDING) {
        trace() << "job ID still pending ?! scheduler recently restarted? " << m->job_id << endl;
        return false;
    }

    if (m->is_from_server() && (j->server() != cs)) {
        log_info() << "the server isn't the same for job " << m->job_id << endl;
        log_info() << "server: " << j->server()->nodeName() << endl;
        log_info() << "msg came from: " << cs->nodeName() << endl;
        // the daemon is not following matz's rules: kick him
        handle_end(cs, 0);
        return false;
    }

    if (!m->is_from_server() && (j->submitter() != cs)) {
        log_info() << "the submitter isn't the same for job " << m->job_id << endl;
        log_info() << "submitter: " << j->submitter()->nodeName() << endl;
        log_info() << "msg came from: " << cs->nodeName() << endl;
        // the daemon is not following matz's rules: kick him
        handle_end(cs, 0);
        return false;
    }

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
    cs->last_talk = time(0);

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
    if (!IS_PROTOCOL_25(cs)) {
        cs->last_talk = time(0);

        if (cs && (cs->maxJobs() < 0)) {
            cs->setMaxJobs(cs->maxJobs() * -1);
        }
    }

    for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it)
        if (*it == cs) {
            (*it)->setLoad(m->load);
            handle_monitor_stats(*it, m);
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
    snprintf(buffer, sizeof(buffer), "%d %s sub:%s on:%s ",
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
    cs->last_talk = time(0);
    cs->setBulkTransfer();
    cs->setState(CompileServer::LOGGEDIN);
    assert(find(controls.begin(), controls.end(), cs) == controls.end());
    controls.push_back(cs);

    std::ostringstream o;
    o << "200-ICECC " VERSION ": "
      << time(0) - starttime << "s uptime, "
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

    char buffer[1000];
    string line;
    list<string> l;
    split_string(m->text, " \t\n", l);
    string cmd;

    cs->last_talk = time(0);

    if (l.empty()) {
        cmd = "";
    } else {
        cmd = l.front();
        l.pop_front();
        transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    }

    if (cmd == "listcs") {
        for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
            sprintf(buffer, " (%s:%d) ", (*it)->name.c_str(), (*it)->remotePort());
            line = " " + (*it)->nodeName() + buffer;
            line += "[" + (*it)->hostPlatform() + "] speed=";
            sprintf(buffer, "%.2f jobs=%d/%d load=%d", server_speed(*it),
                    (int)(*it)->jobList().size(), (*it)->maxJobs(), (*it)->load());
            line += buffer;

            if ((*it)->busyInstalling()) {
                sprintf(buffer, " busy installing since %ld s",  time(0) - (*it)->busyInstalling());
                line += buffer;
            }

            if (!cs->send_msg(TextMsg(line))) {
                return false;
            }

            list<Job *> jobList = (*it)->jobList();
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
        handle_end(cs, 0);
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
                for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
                    if ((*it)->matches(*si)) {
                        if (cs->send_msg(TextMsg(string("removing host ") + *si))) {
                            handle_end(*it, 0);
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
    } else if (cmd == "crashme") {
        *(volatile int *)0 = 42;  // ;-)
    } else if (cmd == "internals") {
        for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
            Msg *msg = NULL;

            if (!l.empty()) {
                list<string>::const_iterator si;

                for (si = l.begin(); si != l.end(); ++si) {
                    if ((*it)->matches(*si)) {
                        break;
                    }
                }

                if (si == l.end()) {
                    continue;
                }
            }

            if ((*it)->send_msg(GetInternalStatus())) {
                msg = (*it)->get_msg();
            }

            if (msg && msg->type == M_STATUS_TEXT) {
                if (!cs->send_msg(TextMsg(dynamic_cast<StatusTextMsg *>(msg)->text))) {
                    return false;
                }
            } else {
                if (!cs->send_msg(TextMsg((*it)->nodeName() + " not reporting\n"))) {
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

    switch (m->type) {
    case M_LOGIN:
        cs->setType(CompileServer::DAEMON);
        ret = handle_login(cs, m);
        break;
    case M_MON_LOGIN:
        cs->setType(CompileServer::MONITOR);
        ret = handle_mon_login(cs, m);
        break;
    default:
        log_info() << "Invalid first message " << (char)m->type << endl;
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

        /* Unfortunately the toanswer queues are also tagged based on the daemon,
           so we need to clean them up also.  */

        for (list<UnansweredList *>::iterator it = toanswer.begin(); it != toanswer.end();) {
            if ((*it)->server == toremove) {
                UnansweredList *l = *it;
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
                it = toanswer.erase(it);
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

        for (list<CompileServer *>::iterator itr = css.begin(); itr != css.end(); ++itr) {
            (*itr)->eraseCSFromBlacklist(toremove);
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
    m = cs->get_msg(0);

    if (!m) {
        handle_end(cs, m);
        return false;
    }

    /* First we need to login.  */
    if (cs->state() == CompileServer::CONNECTED) {
        return try_login(cs, m);
    }

    switch (m->type) {
    case M_JOB_BEGIN:
        ret = handle_job_begin(cs, m);
        break;
    case M_JOB_DONE:
        ret = handle_job_done(cs, m);
        break;
    case M_PING:
        ret = handle_ping(cs, m);
        break;
    case M_STATS:
        ret = handle_stats(cs, m);
        break;
    case M_END:
        handle_end(cs, m);
        ret = false;
        break;
    case M_JOB_LOCAL_BEGIN:
        ret = handle_local_job(cs, m);
        break;
    case M_JOB_LOCAL_DONE:
        ret = handle_local_job_done(cs, m);
        break;
    case M_LOGIN:
        ret = handle_relogin(cs, m);
        break;
    case M_TEXT:
        ret = handle_line(cs, m);
        break;
    case M_GET_CS:
        ret = handle_cs_request(cs, m);
        break;
    case M_BLACKLIST_HOST_ENV:
        ret = handle_blacklist_host_env(cs, m);
        break;
    default:
        log_info() << "Invalid message type arrived " << (char)m->type << endl;
        handle_end(cs, m);
        ret = false;
        break;
    }

    delete m;
    return ret;
}

static int open_broad_listener(int port)
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

    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons(port);
    myaddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return -1;
    }

    return listen_fd;
}

static int open_tcp_listener(short port)
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

    /* Although we select() on fd we need O_NONBLOCK, due to
       possible network errors making accept() block although select() said
       there was some activity.  */
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        log_perror("fcntl()");
        return -1;
    }

    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons(port);
    myaddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return -1;
    }

    if (listen(fd, 10) < 0) {
        log_perror("listen()");
        return -1;
    }

    return fd;
}

#define BROAD_BUFLEN 32
#define BROAD_BUFLEN_OLD 16
static int prepare_broadcast_reply(char* buf, const char* netname)
{
    if (buf[0] < 33) { // old client
        buf[0]++;
        memset(buf + 1, 0, BROAD_BUFLEN_OLD - 1);
        snprintf(buf + 1, BROAD_BUFLEN_OLD - 1, "%s", netname);
        buf[BROAD_BUFLEN_OLD - 1] = 0;
        return BROAD_BUFLEN_OLD;
    } else { // net client
        buf[0] += 2;
        memset(buf + 1, 0, BROAD_BUFLEN - 1);
        uint32_t tmp_version = PROTOCOL_VERSION;
        uint64_t tmp_time = starttime;
        memcpy(buf + 1, &tmp_version, sizeof(uint32_t));
        memcpy(buf + 1 + sizeof(uint32_t), &tmp_time, sizeof(uint64_t));
        const int OFFSET = 1 + sizeof(uint32_t) + sizeof(uint64_t);
        snprintf(buf + OFFSET, BROAD_BUFLEN - OFFSET, "%s", netname);
        buf[BROAD_BUFLEN - 1] = 0;
        return BROAD_BUFLEN;
    }
}

static void broadcast_scheduler_version()
{
    const int schedbuflen = 4 + sizeof(uint64_t);
    char buf[ schedbuflen ];
    buf[0] = 'I';
    buf[1] = 'C';
    buf[2] = 'E';
    buf[3] = PROTOCOL_VERSION;
    uint64_t tmp_time = starttime;
    memcpy(buf + 4, &tmp_time, sizeof(uint64_t));
    DiscoverSched::broadcastData(scheduler_port, buf, sizeof(buf));
}

static void usage(const char *reason = 0)
{
    if (reason) {
        cerr << reason << endl;
    }

    cerr << "ICECREAM scheduler " VERSION "\n";
    cerr << "usage: icecc-scheduler [options] \n"
         << "Options:\n"
         << "  -n, --netname <name>\n"
         << "  -p, --port <port>\n"
         << "  -h, --help\n"
         << "  -l, --log-file <file>\n"
         << "  -d, --daemonize\n"
         << "  -u, --user-uid\n"
         << "  -v[v[v]]]\n"
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
        write(STDERR_FILENO, msg, strlen( msg ));
        _exit(1);
    }

    // make BSD happy
    signal(signum, trigger_exit);
}

int main(int argc, char *argv[])
{
    int listen_fd, remote_fd, broad_fd, text_fd;
    struct sockaddr_in remote_addr;
    socklen_t remote_len;
    char *netname = (char *)"ICECREAM";
    bool detach = false;
    int debug_level = Error;
    string logfile;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno = 0;

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
            { "netname", 1, NULL, 'n' },
            { "help", 0, NULL, 'h' },
            { "port", 1, NULL, 'p' },
            { "daemonize", 0, NULL, 'd'},
            { "log-file", 1, NULL, 'l'},
            { "user-uid", 1, NULL, 'u'},
            { 0, 0, 0, 0 }
        };

        const int c = getopt_long(argc, argv, "n:p:hl:vdr:u:", long_options, &option_index);

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
        case 'l':
            if (optarg && *optarg) {
                logfile = optarg;
            } else {
                usage("Error: -l requires argument");
            }

            break;
        case 'v':

            if (debug_level & Warning) {
                if (debug_level & Info) { // for second call
                    debug_level |= Debug;
                } else {
                    debug_level |= Info;
                }
            } else {
                debug_level |= Warning;
            }

            break;
        case 'n':

            if (optarg && *optarg) {
                netname = optarg;
            } else {
                usage("Error: -n requires argument");
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

        default:
            usage();
        }
    }

    if (warn_icecc_user_errno != 0) {
        log_errno("Error: no icecc user on system. Falling back to nobody.", errno);
    }

    if (getuid() == 0) {
        if (!logfile.size() && detach) {
            if (mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
                if (errno == EEXIST) {
                    chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
                    chown("/var/log/icecc", user_uid, user_gid);
                }
            }

            logfile = "/var/log/icecc/scheduler.log";
        }

        if (setgroups(0, NULL) < 0) {
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

    if (detach) {
        daemon(0, 0);
    }

    listen_fd = open_tcp_listener(scheduler_port);

    if (listen_fd < 0) {
        return 1;
    }

    text_fd = open_tcp_listener(scheduler_port + 1);

    if (text_fd < 0) {
        return 1;
    }

    broad_fd = open_broad_listener(scheduler_port);

    if (broad_fd < 0) {
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        return 1;
    }

    starttime = time(0);

    ofstream pidFile;
    string progName = argv[0];
    progName = progName.substr(progName.rfind('/') + 1);
    pidFilePath = string(RUNDIR) + string("/") + progName + string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << getpid() << endl;
    pidFile.close();

    signal(SIGTERM, trigger_exit);
    signal(SIGINT, trigger_exit);
    signal(SIGALRM, trigger_exit);

    time_t next_listen = 0;

    broadcast_scheduler_version();
    last_announce = starttime;

    while (!exit_main_loop) {
        struct timeval tv;
        tv.tv_usec = 0;
        tv.tv_sec = prune_servers();

        while (empty_queue()) {
            continue;
        }

        /* Announce ourselves from time to time, to make other possible schedulers disconnect
           their daemons if we are the preferred scheduler (daemons with version new enough
           should automatically select the best scheduler, but old daemons connect randomly). */
        if (last_announce + 120 < time(NULL)) {
            broadcast_scheduler_version();
            last_announce = time(NULL);
        }

        fd_set read_set;
        int max_fd = 0;
        FD_ZERO(&read_set);

        if (time(0) >= next_listen) {
            max_fd = listen_fd;
            FD_SET(listen_fd, &read_set);

            if (text_fd > max_fd) {
                max_fd = text_fd;
            }

            FD_SET(text_fd, &read_set);
        }

        if (broad_fd > max_fd) {
            max_fd = broad_fd;
        }

        FD_SET(broad_fd, &read_set);

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
                if (i > max_fd) {
                    max_fd = i;
                }

                FD_SET(i, &read_set);
            }
        }

        max_fd = select(max_fd + 1, &read_set, NULL, NULL, &tv);

        if (max_fd < 0 && errno == EINTR) {
            continue;
        }

        if (max_fd < 0) {
            log_perror("select()");
            return 1;
        }

        if (FD_ISSET(listen_fd, &read_set)) {
            max_fd--;
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
                    cs->last_talk = time(0);

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

            next_listen = time(0) + 1;
        }

        if (max_fd && FD_ISSET(text_fd, &read_set)) {
            max_fd--;
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
                    handle_end(cs, 0);
                    continue;
                }

                while (!cs->read_a_bit() || cs->has_msg())
                    if (!handle_activity(cs)) {
                        break;
                    }
            }
        }

        if (max_fd && FD_ISSET(broad_fd, &read_set)) {
            max_fd--;
            char buf[BROAD_BUFLEN];
            struct sockaddr_in broad_addr;
            socklen_t broad_len = sizeof(broad_addr);
            /* We can get either a daemon request for a scheduler (1 byte) or another scheduler
               announcing itself (4 bytes + time). */
            const int schedbuflen = 4 + sizeof(uint64_t);

            int buflen = recvfrom(broad_fd, buf, max( 1, schedbuflen), 0, (struct sockaddr *) &broad_addr,
                                  &broad_len);
            if (buflen != 1 && buflen != schedbuflen) {
                int err = errno;
                log_perror("recvfrom()");

                /* Some linux 2.6 kernels can return from select with
                   data available, and then return from read() with EAGAIN
                even on a blocking socket (breaking POSIX).  Happens
                 when the arriving packet has a wrong checksum.  So
                 we ignore EAGAIN here, but still abort for all other errors. */
                if (err != EAGAIN) {
                    return -1;
                }
            }
            /* Daemon is searching for a scheduler, only answer if daemon would be able to talk to us. */
            else if (buflen == 1 && buf[0] >= MIN_PROTOCOL_VERSION) {
                log_info() << "broadcast from " << inet_ntoa(broad_addr.sin_addr)
                           << ":" << ntohs(broad_addr.sin_port)
                           << " (version " << int(buf[0]) << ")\n";
                int reply_len = prepare_broadcast_reply(buf, netname);
                if (sendto(broad_fd, buf, reply_len, 0,
                           (struct sockaddr *) &broad_addr, broad_len) != reply_len) {
                    log_perror("sendto()");
                }
            }
            else if (buflen == schedbuflen && buf[0] == 'I' && buf[1] == 'C' && buf[2] == 'E') {
                /* Another scheduler is announcing it's running, disconnect daemons if it has a better version
                   or the same version but was started earlier. */
                uint64_t tmp_time;
                memcpy(&tmp_time, buf + 4, sizeof(uint64_t));
                time_t other_time = tmp_time;
                if (buf[3] > PROTOCOL_VERSION || other_time < starttime) {
                    if (!css.empty() || !monitors.empty()) {
                        log_info() << "Scheduler from " << inet_ntoa(broad_addr.sin_addr)
                               << ":" << ntohs(broad_addr.sin_port)
                               << " (version " << int(buf[3]) << ") has announced itself as a preferred"
                            " scheduler, disconnecting all connections." << endl;
                        while (!css.empty())
                            handle_end(css.front(), NULL);
                        while (!monitors.empty())
                            handle_end(monitors.front(), NULL);
                    }
                }
            }
        }

        for (map<int, CompileServer *>::const_iterator it = fd2cs.begin();
                max_fd && it != fd2cs.end();) {
            int i = it->first;
            CompileServer *cs = it->second;
            /* handle_activity can delete the channel from the fd2cs list,
               hence advance the iterator right now, so it doesn't become
               invalid.  */
            ++it;

            if (FD_ISSET(i, &read_set)) {
                while (!cs->read_a_bit() || cs->has_msg()) {
                    if (!handle_activity(cs)) {
                        break;
                    }
                }

                max_fd--;
            }
        }
    }

    shutdown(broad_fd, SHUT_RDWR);
    close(broad_fd);
    unlink(pidFilePath.c_str());
    return 0;
}
