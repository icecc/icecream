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

#include "compileserver.h"

#include <algorithm>
#include <time.h>

#include "../services/logging.h"
#include "../services/job.h"

#include "job.h"


unsigned int CompileServer::s_hostIdCounter = 0;

CompileServer::CompileServer(const int fd, struct sockaddr *_addr, const socklen_t _len, const bool text_based)
    : MsgChannel(fd, _addr, _len, text_based)
    , m_remotePort(0)
    , m_hostId(0)
    , m_nodeName()
    , m_busyInstalling(0)
    , m_hostPlatform()
    , m_load(1000)
    , m_maxJobs(0)
    , m_noRemote(false)
    , m_jobList()
    , m_submittedJobsCount(0)
    , m_state(CONNECTED)
    , m_type(UNKNOWN)
    , m_chrootPossible(false)
    , m_compilerVersions()
    , m_lastCompiledJobs()
    , m_lastRequestedJobs()
    , m_cumCompiled()
    , m_cumRequested()
    , m_clientMap()
    , m_blacklist()
{
}

void CompileServer::pick_new_id()
{
    assert(!m_hostId);
    m_hostId = ++s_hostIdCounter;
}

bool CompileServer::check_remote(const Job *job) const
{
    bool local = (job->submitter() == this);
    return local || !m_noRemote;
}

bool CompileServer::platforms_compatible(const string &target) const
{
    if (target == hostPlatform()) {
        return true;
    }

    // the below doesn't work as the unmapped platform is transferred back to the
    // client and that asks the daemon for a platform he can't install (see TODO)

    static multimap<string, string> platform_map;

    if (platform_map.empty()) {
        platform_map.insert(make_pair(string("i386"), string("i486")));
        platform_map.insert(make_pair(string("i386"), string("i586")));
        platform_map.insert(make_pair(string("i386"), string("i686")));
        platform_map.insert(make_pair(string("i386"), string("x86_64")));

        platform_map.insert(make_pair(string("i486"), string("i586")));
        platform_map.insert(make_pair(string("i486"), string("i686")));
        platform_map.insert(make_pair(string("i486"), string("x86_64")));

        platform_map.insert(make_pair(string("i586"), string("i686")));
        platform_map.insert(make_pair(string("i586"), string("x86_64")));

        platform_map.insert(make_pair(string("i686"), string("x86_64")));

        platform_map.insert(make_pair(string("ppc"), string("ppc64")));
        platform_map.insert(make_pair(string("s390"), string("s390x")));
    }

    multimap<string, string>::const_iterator end = platform_map.upper_bound(target);

    for (multimap<string, string>::const_iterator it = platform_map.lower_bound(target);
            it != end;
            ++it) {
        if (it->second == hostPlatform()) {
            return true;
        }
    }

    return false;
}

/* Given a candidate CS and a JOB, check if any of the requested
   environments could be installed on the CS.  This is the case if that
   env can be run there, i.e. if the host platforms of the CS and of the
   environment are compatible.  Return an empty string if none can be
   installed, otherwise return the platform of the first found
   environments which can be installed.  */
string CompileServer::can_install(const Job *job)
{
    // trace() << "can_install host: '" << cs->host_platform << "' target: '"
    //         << job->target_platform << "'" << endl;
    if (busyInstalling()) {
#if DEBUG_SCHEDULER > 0
        trace() << nodeName() << " is busy installing since " << time(0) - cs->busyInstalling()
                << " seconds." << endl;
#endif
        return string();
    }

    Environments environments = job->environments();
    for (Environments::const_iterator it = environments.begin();
            it != environments.end(); ++it) {
        if (platforms_compatible(it->first) && !blacklisted(job, *it)) {
            return it->first;
        }
    }

    return string();
}

bool CompileServer::is_eligible(const Job *job)
{
    bool jobs_okay = int(m_jobList.size()) < m_maxJobs;
    bool load_okay = m_load < 1000;
    bool version_okay = job->minimalHostVersion() <= protocol;
    return jobs_okay
           && (m_chrootPossible || job->submitter() == this)
           && load_okay
           && version_okay
           && can_install(job).size()
           && this->check_remote(job);
}

unsigned int CompileServer::remotePort() const
{
    return m_remotePort;
}

void CompileServer::setRemotePort(unsigned int port)
{
    m_remotePort = port;
}

unsigned int CompileServer::hostId() const
{
    return m_hostId;
}

void CompileServer::setHostId(unsigned int id)
{
    m_hostId = id;
}

string CompileServer::nodeName() const
{
    return m_nodeName;
}

void CompileServer::setNodeName(const string &name)
{
    m_nodeName = name;
}

bool CompileServer::matches(const string& nm) const
{
    return m_nodeName == nm || name == nm;
}

time_t CompileServer::busyInstalling() const
{
    return m_busyInstalling;
}

void CompileServer::setBusyInstalling(time_t time)
{
    m_busyInstalling = time;
}

string CompileServer::hostPlatform() const
{
    return m_hostPlatform;
}

void CompileServer::setHostPlatform(const string &platform)
{
    m_hostPlatform = platform;
}

unsigned int CompileServer::load() const
{
    return m_load;
}

void CompileServer::setLoad(unsigned int load)
{
    m_load = load;
}

int CompileServer::maxJobs() const
{
    return m_maxJobs;
}

void CompileServer::setMaxJobs(int jobs)
{
    m_maxJobs = jobs;
}

bool CompileServer::noRemote() const
{
    return m_noRemote;
}

void CompileServer::setNoRemote(bool value)
{
    m_noRemote = value;
}

list<Job *> CompileServer::jobList() const
{
    return m_jobList;
}

void CompileServer::appendJob(Job *job)
{
    m_jobList.push_back(job);
}

void CompileServer::removeJob(Job *job)
{
    m_jobList.remove(job);
}

int CompileServer::submittedJobsCount() const
{
    return m_submittedJobsCount;
}

void CompileServer::submittedJobsIncrement()
{
    m_submittedJobsCount++;
}

void CompileServer::submittedJobsDecrement()
{
    m_submittedJobsCount--;
}

CompileServer::State CompileServer::state() const
{
    return m_state;
}

void CompileServer::setState(const CompileServer::State state)
{
    m_state = state;
}

CompileServer::Type CompileServer::type() const
{
    return m_type;
}

void CompileServer::setType(const CompileServer::Type type)
{
    m_type = type;
}

bool CompileServer::chrootPossible() const
{
    return m_chrootPossible;
}

void CompileServer::setChrootPossible(const bool possible)
{
    m_chrootPossible = possible;
}

Environments CompileServer::compilerVersions() const
{
    return m_compilerVersions;
}

void CompileServer::setCompilerVersions(const Environments &environments)
{
    m_compilerVersions = environments;
}

list<JobStat> CompileServer::lastCompiledJobs() const
{
    return m_lastCompiledJobs;
}

void CompileServer::appendCompiledJob(const JobStat &stats)
{
    m_lastCompiledJobs.push_back(stats);
}

void CompileServer::popCompiledJob()
{
    m_lastCompiledJobs.pop_front();
}

list<JobStat> CompileServer::lastRequestedJobs() const
{
    return m_lastRequestedJobs;
}

void CompileServer::appendRequestedJobs(const JobStat &stats)
{
    m_lastRequestedJobs.push_back(stats);
}

void CompileServer::popRequestedJobs()
{
    m_lastRequestedJobs.pop_front();
}

JobStat CompileServer::cumCompiled() const
{
    return m_cumCompiled;
}

void CompileServer::setCumCompiled(const JobStat &stats)
{
    m_cumCompiled = stats;
}

JobStat CompileServer::cumRequested() const
{
    return m_cumRequested;
}

void CompileServer::setCumRequested(const JobStat &stats)
{
    m_cumRequested = stats;
}

int CompileServer::getClientJobId(const int localJobId)
{
    return m_clientMap[localJobId];
}

void CompileServer::insertClientJobId(const int localJobId, const int newJobId)
{
    m_clientMap[localJobId] = newJobId;
}

void CompileServer::eraseClientJobId(const int localJobId)
{
    m_clientMap.erase(localJobId);
}

map<CompileServer *, Environments> CompileServer::blacklist() const
{
    return m_blacklist;
}

Environments CompileServer::getEnvsForBlacklistedCS(CompileServer *cs)
{
    return m_blacklist[cs];
}

void CompileServer::blacklistCompileServer(CompileServer *cs, const std::pair<std::string, std::string> &env)
{
    m_blacklist[cs].push_back(env);
}

void CompileServer::eraseCSFromBlacklist(CompileServer *cs)
{
    m_blacklist.erase(cs);
}

bool CompileServer::blacklisted(const Job *job, const pair<string, string> &environment)
{
    Environments blacklist = job->submitter()->getEnvsForBlacklistedCS(this);
    return find(blacklist.begin(), blacklist.end(), environment) != blacklist.end();
}
