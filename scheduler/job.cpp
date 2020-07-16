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

#include "job.h"

#include "compileserver.h"

Job::Job(const unsigned int _id, CompileServer *subm)
    : m_id(_id)
    , m_localClientId(0)
    , m_state(PENDING)
    , m_server(nullptr)
    , m_submitter(subm)
    , m_startTime(0)
    , m_startOnScheduler(0)
    , m_doneTime(0)
    , m_targetPlatform()
    , m_fileName()
    , m_masterJobFor()
    , m_argFlags(0)
    , m_language()
    , m_preferredHost()
    , m_minimalHostVersion(0)
    , m_requiredFeatures(0)
    , m_niceness(0)
{
    m_submitter->submittedJobsIncrement();
}

Job::~Job()
{
    // XXX is this really deleted on all other paths?
    /*    fd2chan.erase (channel->fd);
        delete channel;*/
    m_submitter->submittedJobsDecrement();
}

unsigned int Job::id() const
{
    return m_id;
}

unsigned int Job::localClientId() const
{
    return m_localClientId;
}

void Job::setLocalClientId(const unsigned int id)
{
    m_localClientId = id;
}

Job::State Job::state() const
{
    return m_state;
}

void Job::setState(const Job::State state)
{
    m_state = state;
}

CompileServer *Job::server() const
{
    return m_server;
}

void Job::setServer(CompileServer *server)
{
    m_server = server;
}

CompileServer *Job::submitter() const
{
    return m_submitter;
}

void Job::setSubmitter(CompileServer *submitter)
{
    m_submitter = submitter;
}

Environments Job::environments() const
{
    return m_environments;
}

void Job::setEnvironments(const Environments &environments)
{
    m_environments = environments;
}

void Job::appendEnvironment(const std::pair<std::string, std::string> &env)
{
    m_environments.push_back(env);
}

void Job::clearEnvironments()
{
    m_environments.clear();
}

time_t Job::startTime() const
{
    return m_startTime;
}

void Job::setStartTime(const time_t time)
{
    m_startTime = time;
}

time_t Job::startOnScheduler() const
{
    return m_startOnScheduler;
}

void Job::setStartOnScheduler(const time_t time)
{
    m_startOnScheduler = time;
}

time_t Job::doneTime() const
{
    return m_doneTime;
}

void Job::setDoneTime(const time_t time)
{
    m_doneTime = time;
}

std::string Job::targetPlatform() const
{
    return m_targetPlatform;
}

void Job::setTargetPlatform(const std::string &platform)
{
    m_targetPlatform = platform;
}

std::string Job::fileName() const
{
    return m_fileName;
}

void Job::setFileName(const std::string &fileName)
{
    m_fileName = fileName;
}

std::list<Job *> Job::masterJobFor() const
{
    return m_masterJobFor;
}

void Job::appendJob(Job *job)
{
    m_masterJobFor.push_back(job);
}

unsigned int Job::argFlags() const
{
    return m_argFlags;
}

void Job::setArgFlags(const unsigned int argFlags)
{
    m_argFlags = argFlags;
}

std::string Job::language() const
{
    return m_language;
}

void Job::setLanguage(const std::string &language)
{
    m_language = language;
}

std::string Job::preferredHost() const
{
    return m_preferredHost;
}

void Job::setPreferredHost(const std::string &host)
{
    m_preferredHost = host;
}

int Job::minimalHostVersion() const
{
    return m_minimalHostVersion;
}

void Job::setMinimalHostVersion(int version)
{
    m_minimalHostVersion = version;
}

unsigned int Job::requiredFeatures() const
{
    return m_requiredFeatures;
}

void Job::setRequiredFeatures(unsigned int features)
{
    m_requiredFeatures = features;
}

int Job::niceness() const
{
    return m_niceness;
}

void Job::setNiceness(int nice)
{
    m_niceness = nice;
}
