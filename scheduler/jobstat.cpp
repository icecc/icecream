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

#include "jobstat.h"

JobStat::JobStat()
    : m_outputSize(0)
    , m_compileTimeReal(0)
    , m_compileTimeUser(0)
    , m_compileTimeSys(0)
    , m_jobId(0)
{
}

unsigned long JobStat::outputSize() const
{
    return m_outputSize;
}

void JobStat::setOutputSize(unsigned long size)
{
    m_outputSize = size;
}

unsigned long JobStat::compileTimeReal() const
{
    return m_compileTimeReal;
}

void JobStat::setCompileTimeReal(unsigned long time)
{
    m_compileTimeReal = time;
}

unsigned long JobStat::compileTimeUser() const
{
    return m_compileTimeUser;
}

void JobStat::setCompileTimeUser(unsigned long time)
{
    m_compileTimeUser = time;
}

unsigned long JobStat::compileTimeSys() const
{
    return m_compileTimeSys;
}

void JobStat::setCompileTimeSys(unsigned long time)
{
    m_compileTimeSys = time;
}

unsigned int JobStat::jobId() const
{
    return m_jobId;
}

void JobStat::setJobId(unsigned int id)
{
    m_jobId = id;
}

JobStat& JobStat::operator+=(const JobStat& st)
{
    m_outputSize += st.m_outputSize;
    m_compileTimeReal += st.m_compileTimeReal;
    m_compileTimeUser += st.m_compileTimeUser;
    m_compileTimeSys +=  st.m_compileTimeSys;
    m_jobId = 0;
    return *this;
}

JobStat& JobStat::operator-=(const JobStat &st)
{
    m_outputSize -= st.m_outputSize;
    m_compileTimeReal -= st.m_compileTimeReal;
    m_compileTimeUser -= st.m_compileTimeUser;
    m_compileTimeSys -= st.m_compileTimeSys;
    m_jobId = 0;
    return *this;
}

JobStat& JobStat::operator/=(int d)
{
    m_outputSize /= d;
    m_compileTimeReal /= d;
    m_compileTimeUser /= d;
    m_compileTimeSys /= d;
    m_jobId = 0;
    return *this;
}
