#ifndef _COMPILE_JOB_H
#define _COMPILE_JOB_H

#include <list>
#include <string>

typedef enum { Arg_Unspecified, Arg_Local, Arg_Remote, Arg_Rest } Argument_Type;
class ArgumentsList : public std::list< std::pair<std::string, Argument_Type> >
{
public:
    void append( std::string s, Argument_Type t ) {
        push_back( make_pair( s, t ) );
    }
};

class CompileJob {

public:
    typedef enum {Lang_C, Lang_CXX, Lang_OBJC} Language;

    CompileJob() : m_id( 0 ) {}

    void setLanguage( Language lg ) {
        m_language = lg;
    }
    Language language() const {
        return m_language;
    }

    void setEnvironmentVersion( const std::string& ver ) {
        m_environment_version = ver;
    }

    std::string environmentVersion() const {
        return m_environment_version;
    }

    void setFlags( const ArgumentsList &flags ) {
        m_flags = flags;
    }
    std::list<std::string> localFlags() const;
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

private:
    std::list<std::string> flags( Argument_Type argumentType ) const;

    unsigned int m_id;
    Language m_language;
    std::string m_environment_version;
    ArgumentsList m_flags;
    std::string m_input_file, m_output_file;
};

void appendList( std::list<std::string> &list,
                 const std::list<std::string> &toadd )
{
    // Cannot splice since toadd is a reference-to-const
    list.insert( list.end(), toadd.begin(), toadd.end() );
}

bool write_job( int fd, const CompileJob &job );
bool read_job( int in_fd, CompileJob &job );

#endif
