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

#ifndef JOBSTAT_H
#define JOBSTAT_H

struct JobStat {
public:
    JobStat();

    unsigned long outputSize() const;
    void setOutputSize(unsigned long size);

    unsigned long compileTimeReal() const;
    void setCompileTimeReal(unsigned long time);

    unsigned long compileTimeUser() const;
    void setCompileTimeUser(unsigned long time);

    unsigned long compileTimeSys() const;
    void setCompileTimeSys(unsigned long time);

    unsigned int jobId() const;
    void setJobId(unsigned int id);

    JobStat& operator+=(const JobStat& st);
    JobStat& operator-=(const JobStat& st);
    JobStat& operator/=(int d);

private:
    unsigned long m_outputSize;  // output size (uncompressed)
    unsigned long m_compileTimeReal;  // in milliseconds
    unsigned long m_compileTimeUser;
    unsigned long m_compileTimeSys;
    unsigned int m_jobId;
};

inline
JobStat operator+(const JobStat& l, const JobStat& r)
{
    return JobStat( l ) += r;
}

inline
JobStat operator-(const JobStat& l, const JobStat& r)
{
    return JobStat( l ) -= r;
}

inline
JobStat operator/(const JobStat& st, int d)
{
    return JobStat( st ) /= d;
}

#endif
