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

#ifndef JOB_H
#define JOB_H

#include <list>
#include <string>
#include <time.h>

#include "../services/comm.h"

class CompileServer;

class Job
{
public:
    enum State {
        PENDING,
        WAITINGFORCS,
        COMPILING
    };

    Job(const unsigned int _id, CompileServer *subm);
    ~Job();

    unsigned int id() const;

    unsigned int localClientId() const;
    void setLocalClientId(const unsigned int id);

    State state() const;
    void setState(const State state);

    CompileServer *server() const;
    void setServer(CompileServer *server);

    CompileServer *submitter() const;
    void setSubmitter(CompileServer *submitter);

    Environments environments() const;
    void setEnvironments(const Environments &environments);
    void appendEnvironment(const std::pair<std::string, std::string> &env);
    void clearEnvironments();

    time_t startTime() const;
    void setStartTime(const time_t time);

    time_t startOnScheduler() const;
    void setStartOnScheduler(const time_t time);

    time_t doneTime() const;
    void setDoneTime(const time_t time);

    std::string targetPlatform() const;
    void setTargetPlatform(const std::string &platform);

    std::string fileName() const;
    void setFileName(const std::string &fileName);

    std::list<Job *> masterJobFor() const;
    void appendJob(Job *job);

    unsigned int argFlags() const;
    void setArgFlags(const unsigned int argFlags);

    std::string language() const;
    void setLanguage(const std::string &language);

    std::string preferredHost() const;
    void setPreferredHost(const std::string &host);

    int minimalHostVersion() const;
    void setMinimalHostVersion( int version );

    unsigned int requiredFeatures() const;
    void setRequiredFeatures(unsigned int features);

    int niceness() const;
    void setNiceness( int niceness );

private:
    const unsigned int m_id;
    unsigned int m_localClientId;
    State m_state;
    CompileServer *m_server;  // on which server we build
    CompileServer *m_submitter;  // who submitted us
    Environments m_environments;
    time_t m_startTime;  // _local_ to the compiler server
    time_t m_startOnScheduler;  // starttime local to scheduler
    /**
     * the end signal from client and daemon is a bit of a race and
     * in 99.9% of all cases it's catched correctly. But for the remaining
     * 0.1% we need a solution too - otherwise these jobs are eating up slots.
     * So the solution is to track done jobs (client exited, daemon didn't signal)
     * and after 10s no signal, kill the daemon (and let it rehup) **/
    time_t m_doneTime;

    std::string m_targetPlatform;
    std::string m_fileName;
    std::list<Job *> m_masterJobFor;
    unsigned int m_argFlags;
    std::string m_language; // for debugging
    std::string m_preferredHost; // for debugging daemons
    int m_minimalHostVersion; // minimal version required for the the remote server
    unsigned int m_requiredFeatures; // flags the job requires on the remote server
    int m_niceness; // nice priority (0-20)
};

#endif
