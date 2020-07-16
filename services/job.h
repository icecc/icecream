/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
  This file is part of Icecream.

  Copyright (c) 2004-2014 Stephan Kulow <coolo@suse.de>

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

#ifndef ICECREAM_COMPILE_JOB_H
#define ICECREAM_COMPILE_JOB_H

#include <list>
#include <string>
#include <iostream>
#include <sstream>

typedef enum {
    Arg_Local,  // Local-only args.
    Arg_Remote, // Remote-only args.
    Arg_Rest    // Args to use both locally and remotely.
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
        Lang_OBJCXX,
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
        , m_dwarf_fission(false)
        , m_block_rewrite_includes(false)
    {
        setTargetPlatform();
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

    // Not used remotely.
    void setCompilerPathname(const std::string& pathname)
    {
        m_compiler_pathname = pathname;
    }

    // Not used remotely.
    // Use find_compiler(), as this may be empty.
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
    std::list<std::string> nonLocalFlags() const;
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

    // Since protocol 41 this is just a shortcut saying that allFlags() contains "-gsplit-dwarf".
    void setDwarfFissionEnabled(bool flag)
    {
        m_dwarf_fission = flag;
    }

    bool dwarfFissionEnabled() const
    {
        return m_dwarf_fission;
    }

    void setWorkingDirectory(const std::string& dir)
    {
        m_working_directory = dir;
    }

    std::string workingDirectory() const
    {
        return m_working_directory;
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

    // Not used remotely.
    void setBlockRewriteIncludes(bool flag)
    {
        m_block_rewrite_includes = flag;
    }

    // Not used remotely.
    bool blockRewriteIncludes() const
    {
        return m_block_rewrite_includes;
    }

private:
    std::list<std::string> flags(Argument_Type argumentType) const;
    void setTargetPlatform();

    unsigned int m_id;
    Language m_language;
    std::string m_compiler_pathname;
    std::string m_compiler_name;
    std::string m_environment_version;
    ArgumentsList m_flags;
    std::string m_input_file, m_output_file;
    std::string m_working_directory;
    std::string m_target_platform;
    bool m_dwarf_fission;
    bool m_block_rewrite_includes;
};

inline void appendList(std::list<std::string> &list, const std::list<std::string> &toadd)
{
    // Cannot splice since toadd is a reference-to-const
    list.insert(list.end(), toadd.begin(), toadd.end());
}

inline std::ostream &operator<<( std::ostream &output,
                                 const CompileJob::Language &l )
{
    switch (l) {
    case CompileJob::Lang_CXX:
        output << "C++";
        break;
    case CompileJob::Lang_C:
        output << "C";
        break;
    case CompileJob::Lang_Custom:
        output << "<custom>";
        break;
    case CompileJob::Lang_OBJC:
        output << "ObjC";
        break;
    case CompileJob::Lang_OBJCXX:
        output << "ObjC++";
        break;
    }
    return output;
}

inline std::string concat_args(const std::list<std::string> &args)
{
    std::stringstream str;
    str << "'";

    for (std::list<std::string>::const_iterator it = args.begin(); it != args.end();) {
        str << *it++;
        if (it != args.end())
            str << ", ";
    }
    return str.str() + "'";
}

#endif
