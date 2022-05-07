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

#ifndef COMPILESERVER_H
#define COMPILESERVER_H

#include <string>
#include <list>
#include <map>

#include "../services/comm.h"
#include "jobstat.h"

class Job;

using namespace std;

/* One compile server (receiver, compile daemon)  */
class CompileServer : public MsgChannel
{
public:
    enum State {
        CONNECTED,
        LOGGEDIN
    };

    enum Type {
        UNKNOWN,
        DAEMON,
        MONITOR,
        LINE
    };

    CompileServer(const int fd, struct sockaddr *_addr, const socklen_t _len, const bool text_based);

    void pick_new_id();

    bool check_remote(const Job *job) const;
    bool platforms_compatible(const string &target) const;
    string can_install(const Job *job, bool ignore_installing = false) const;
    bool is_eligible_ever(const Job *job) const;
    bool is_eligible_now(const Job *job) const;

    unsigned int remotePort() const;
    void setRemotePort(const unsigned int port);

    unsigned int hostId() const;
    void setHostId(const unsigned int id);

    string nodeName() const;
    void setNodeName(const string &name);

    bool matches(const string& nm) const;

    time_t busyInstalling() const;
    void setBusyInstalling(const time_t time);

    string hostPlatform() const;
    void setHostPlatform(const string &platform);

    unsigned int load() const;
    void setLoad(const unsigned int load);

    int maxJobs() const;
    void setMaxJobs(const int jobs);
    int maxPreloadCount() const;
    int currentJobCount() const;
    int currentJobCountRemote() const;
    int currentJobCountLocal() const;

    bool noRemote() const;
    void setNoRemote(const bool value);

    const list<Job *>& jobList() const;
    void appendJob(Job *job);
    void removeJob(Job *job);
    unsigned int lastPickedId();

    State state() const;
    void setState(const State state);

    Type type() const;
    void setType(const Type type);

    bool chrootPossible() const;
    void setChrootPossible(const bool possible);

    bool featuresSupported(unsigned int features) const;
    unsigned int supportedFeatures() const;
    void setSupportedFeatures(unsigned int features);

    int clientCount() const;
    void setClientCount( int clientCount );
    int submittedJobsCount() const;
    void submittedJobsIncrement();
    void submittedJobsDecrement();

    Environments compilerVersions() const;
    void setCompilerVersions(const Environments &environments);

    list<JobStat> lastCompiledJobs() const;
    void appendCompiledJob(const JobStat &stats);
    void popCompiledJob();

    list<JobStat> lastRequestedJobs() const;
    void appendRequestedJobs(const JobStat &stats);
    void popRequestedJobs();

    JobStat cumCompiled() const;
    void setCumCompiled(const JobStat &stats);

    JobStat cumRequested() const;
    void setCumRequested(const JobStat &stats);


    unsigned int hostidCounter() const;

    int getClientLocalJobId(const int localJobId);
    void insertClientLocalJobId(const int localJobId, const int newJobId, bool fulljob);
    void eraseClientLocalJobId(const int localJobId);

    map<const CompileServer *, Environments> blacklist() const;
    Environments getEnvsForBlacklistedCS(const CompileServer *cs);
    void blacklistCompileServer(CompileServer *cs, const std::pair<std::string, std::string> &env);
    void eraseCSFromBlacklist(CompileServer *cs);

    int getInFd() const;
    void startInConnectionTest();
    time_t getConnectionTimeout();
    time_t getNextTimeout();
    bool getConnectionInProgress();
    bool isConnected();
    void updateInConnectivity(bool acceptingIn);

private:
    bool blacklisted(const Job *job, const pair<string, string> &environment) const;

    /* The listener port, on which it takes compile requests.  */
    unsigned int m_remotePort;
    unsigned int m_hostId;
    string m_nodeName;
    time_t m_busyInstalling;
    string m_hostPlatform;

    // LOAD is load * 1000
    unsigned int m_load;
    int m_maxJobs;
    bool m_noRemote;
    list<Job *> m_jobList;
    State m_state;
    Type m_type;
    bool m_chrootPossible;
    unsigned int m_featuresSupported;
    int m_clientCount; // number of client connections the daemon has
    int m_submittedJobsCount;
    unsigned int m_lastPickId;

    Environments m_compilerVersions;  // Available compilers

    list<JobStat> m_lastCompiledJobs;
    list<JobStat> m_lastRequestedJobs;
    JobStat m_cumCompiled;  // cumulated
    JobStat m_cumRequested;

    static unsigned int s_hostIdCounter;

    // map client ID for daemon to our IDs
    struct LocalJobInfo
    {
        int id;
        bool fulljob;
    };
    map<int, LocalJobInfo> m_clientLocalMap;

    map<const CompileServer *, Environments> m_blacklist;

    int m_inFd;
    unsigned int m_inConnAttempt;
    time_t m_nextConnTime;
    time_t m_lastConnStartTime;
    bool m_acceptingInConnection;
};

#endif
