/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

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

#ifndef _COMPILE_JOB_H
#define _COMPILE_JOB_H

#include <list>
#include <string>

typedef enum {
    Arg_Unspecified,
    Arg_Local,
    Arg_Remote,
    Arg_Rest
} Argument_Type;

class ArgumentsList : public std::list<std::pair<std::string, Argument_Type> >
{
public:
    void append(std::string s, Argument_Type t) {
        push_back(make_pair(s, t));
    }
};

class CompileJob
{
public:
    typedef enum {
        Lang_C,
        Lang_CXX,
        Lang_OBJC,
        Lang_Custom
    } Language;

    typedef enum {
        Flag_None = 0,
        Flag_g = 0x1,
        Flag_g3 = 0x2,
        Flag_O = 0x4,
        Flag_O2 = 0x8,
        Flag_Ol2 = 0x10
    } Flag;

    CompileJob()
        : m_id(0)
    {
        __setTargetPlatform();
    }

    void setCompilerName(const std::string &name)
    {
        m_compiler_name = name;
    }

    std::string compilerName() const
    {
        return m_compiler_name;
    }

    void setLanguage(Language lg)
    {
        m_language = lg;
    }

    Language language() const
    {
        return m_language;
    }

    void setCompilerPathname(const std::string& pathname)
    {
        m_compiler_pathname = pathname;
    }

    std::string compilerPathname() const
    {
        return m_compiler_pathname;
    }

    void setEnvironmentVersion(const std::string &ver)
    {
        m_environment_version = ver;
    }

    std::string environmentVersion() const
    {
        return m_environment_version;
    }

    unsigned int argumentFlags() const;

    void setFlags(const ArgumentsList &flags)
    {
        m_flags = flags;
    }
    std::list<std::string> localFlags() const;
    std::list<std::string> remoteFlags() const;
    std::list<std::string> restFlags() const;
    std::list<std::string> allFlags() const;

    void setInputFile(const std::string &file)
    {
        m_input_file = file;
    }

    std::string inputFile() const
    {
        return m_input_file;
    }

    void setOutputFile(const std::string &file)
    {
        m_output_file = file;
    }

    std::string outputFile() const
    {
        return m_output_file;
    }

    void setJobID(unsigned int id)
    {
        m_id = id;
    }

    unsigned int jobID() const
    {
        return m_id;
    }

    void appendFlag(std::string arg, Argument_Type argumentType)
    {
        m_flags.append(arg, argumentType);
    }

    std::string targetPlatform() const
    {
        return m_target_platform;
    }

    void setTargetPlatform(const std::string &_target)
    {
        m_target_platform = _target;
    }

private:
    std::list<std::string> flags(Argument_Type argumentType) const;
    void __setTargetPlatform();

    unsigned int m_id;
    Language m_language;
    std::string m_compiler_pathname;
    std::string m_compiler_name;
    std::string m_environment_version;
    ArgumentsList m_flags;
    std::string m_input_file, m_output_file;
    std::string m_target_platform;
};

inline void appendList(std::list<std::string> &list, const std::list<std::string> &toadd)
{
    // Cannot splice since toadd is a reference-to-const
    list.insert(list.end(), toadd.begin(), toadd.end());
}

#endif
