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
#include <map>
#include <iostream>
#include <sstream>

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
    typedef std::map<uint32_t, std::string>  OutputFiles;
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

    /* extra files are processed in this order */
    enum ExFileEnum {
       eExFile_stdOutput     = 0, // unused
       eExFile_START        = 1,
       eExFile_dwarfFission = 1,
       eExFile_coverage     = 2,
       eExFile_st_i         = 3,
       eExFile_st_ii        = 4,
       eExFile_st_s         = 5,
       eExFile_END          = 5
    } ;
    const static std::string ef_ext[];

    CompileJob()
        : m_id(0)
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
        m_output_files[0] = file;
    }

    std::string outputFile() const
    {
        return m_output_files.at(0);
    }

    const OutputFiles& outputFiles() const
    {
        return m_output_files;
    }

    void setExtraOutputFile(uint32_t index, const std::string &file)
    {
        //printf("setExtraOutputFile[%d] = %s\n", index, file.c_str());
        m_output_files[index] = file;
    }
    std::string ExtraOutputFileExt(const std::string &ext)
    {
        std::string file = outputFile().substr(0, outputFile().find_last_of('.')) + ext;
        return file;
    }
    void setExtraOutputFileExt(uint32_t index, const std::string &ext)
    {
        setExtraOutputFile(index, ExtraOutputFileExt(ext));
    }
    void setExtraOutputFileEnum(ExFileEnum index)
    {
        setExtraOutputFileExt(index, ef_ext[index]);
    }
    void setExtraOutputFileRemote(ExFileEnum index, const std::string &file)
    {
        setExtraOutputFile(index, file);
        m_extra_files |= (1 << index);
    }
    std::string ExtraOutputFileEnum(ExFileEnum index)
    {
        return ExtraOutputFileExt(ef_ext[index]);
    }
    uint32_t ExtraOutputFiles() const
    {
        return m_extra_files;
    }

    bool dwarfFissionEnabled() const
    {
        return m_output_files.find(eExFile_dwarfFission) != m_output_files.end();
    }
    bool extraOutputEnabled() const
    {
        uint32_t c(0);
        for (auto i : m_output_files) {
            ++c;
        }

        if (dwarfFissionEnabled()) {
            if (c >= 3) {
                return true;
            }
        }
        else if (c >= 2) {
            return true;
        }
        return false;   
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

private:
    std::list<std::string> flags(Argument_Type argumentType) const;
    void setTargetPlatform();

    unsigned int m_id;
    Language m_language;
    std::string m_compiler_pathname;
    std::string m_compiler_name;
    std::string m_environment_version;
    ArgumentsList m_flags;
    std::string m_input_file;
    uint32_t m_extra_files = 0;
    OutputFiles  m_output_files;
    std::string m_working_directory;
    std::string m_target_platform;
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
