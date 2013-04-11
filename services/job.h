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
#include <map>
#include <string>

typedef enum {
    Arg_Local, ///< arguments which definitely must be local (they e.g. create files)
    Arg_Cpp,   ///< arguments related to preprocessing (used locally or remote depending on mode)
    Arg_Remote, ///< argument related to actual compiling (used remotely)
    Arg_Rest   ///< uknown type of argument (always used)
} Argument_Type;

class ArgumentsList : public std::list< std::pair<std::string, Argument_Type> >
{
public:
    void append( std::string s, Argument_Type t ) {
        push_back( make_pair( s, t ) );
    }
};

/// Defines how included headers and preprocessing are handled
enum PreprocessMode {
    /**
    Local -E, remote compile of just the resulting file.
    */
    LocalPreprocess,
    /**
    Local -frewrite-includes, remote compile of just the resulting file.
    Clang works suboptimally when handling an already preprocessed source file,
    for example error messages quote (already preprocessed) parts of the source.
    Therefore it is better to only locally merge all #include files into the source
    file and do the actual preprocessing remotely together with compiling.
    There exists a Clang patch to implement option -frewrite-includes that does
    such #include rewritting, and it's been only recently merged upstream.
    */
    RewriteIncludes,
    /**
    Local -H to find out what headers would be included, they'll be sent to remote
    node for normal compile.
    */
    SendHeaders
};


class CompileJob {

public:
    typedef enum {Lang_C, Lang_CXX, Lang_OBJC, Lang_Custom} Language;
    typedef enum { Flag_None = 0, Flag_g = 0x1, Flag_g3 = 0x2, Flag_O = 0x4, Flag_O2 = 0x8, Flag_Ol2 = 0x10 } Flag;

    CompileJob() : m_id( 0 ) { __setTargetPlatform(); }

    void setCompilerName(const std::string& name) { m_compiler_name = name; }
    std::string compilerName() const { return m_compiler_name; }

    void setLanguage( Language lg ) { m_language = lg; }
    Language language() const { return m_language; }

    void setEnvironmentVersion( const std::string& ver ) {
        m_environment_version = ver;
    }

    std::string environmentVersion() const {
        return m_environment_version;
    }

    unsigned int argumentFlags() const;

    void setFlags( const ArgumentsList &flags ) {
        m_flags = flags;
    }
    std::list<std::string> localFlags() const;
    std::list<std::string> cppFlags() const;
    std::list<std::string> remoteFlags() const;
    std::list<std::string> restFlags() const;
    std::list<std::string> allFlags() const;

    void setInputFile( const std::string &file ) {
        m_input_file = file;
    }

    std::string inputFile() const {
        return m_input_file;
    }

    void setOutputFile( const std::string &file ) {
        m_output_file = file;
    }

    std::string outputFile() const {
        return m_output_file;
    }

    void setJobID( unsigned int id ) {
        m_id = id;
    }
    unsigned int jobID() const {
        return m_id;
    }

    void appendFlag( std::string arg, Argument_Type argumentType ) {
        m_flags.append( arg, argumentType );
    }

    std::string targetPlatform() const { return m_target_platform; }
    void setTargetPlatform( const std::string &_target ) {
        m_target_platform = _target;
    }

    void setPreprocessMode( PreprocessMode mode ) {
        m_preprocessMode = mode;
    }

    PreprocessMode preprocessMode() const {
        return m_preprocessMode;
    }

    void addIncludeFile( const std::string& md5, const std::string& filename ) {
        m_include_files[ md5 ] = filename;
    }

    const std::map< std::string, std::string >& includeFiles() const {
        return m_include_files;
    }

private:
    std::list<std::string> flags( Argument_Type argumentType ) const;
    void __setTargetPlatform();

    unsigned int m_id;
    Language m_language;
    std::string m_compiler_name;
    std::string m_environment_version;
    ArgumentsList m_flags;
    std::string m_input_file, m_output_file;
    std::string m_target_platform;
    // md5 -> full filename of all include files (used when sending headers)
    std::map< std::string, std::string > m_include_files;
    PreprocessMode m_preprocessMode;
};

inline void appendList( std::list<std::string> &list,
                 const std::list<std::string> &toadd )
{
    // Cannot splice since toadd is a reference-to-const
    list.insert( list.end(), toadd.begin(), toadd.end() );
}

#endif
