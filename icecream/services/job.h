#ifndef _COMPILE_JOB_H
#define _COMPILE_JOB_H

#include <list>
#include <string>

class CompileJob {

public:
    typedef enum {Lang_C, Lang_CXX, Lang_OBJC} Language;
    typedef std::list<std::string> ArgumentsList;

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

    void setLocalFlags( const ArgumentsList & l ) {
        m_local_flags = l;
    }
    ArgumentsList localFlags() const {
        return m_local_flags;
    }
    void setRemoteFlags( const ArgumentsList & l ) {
        m_remote_flags = l;
    }
    ArgumentsList remoteFlags() const {
        return m_remote_flags;
    }
    void setRestFlags( const ArgumentsList & l ) {
        m_rest_flags = l;
    }
    ArgumentsList restFlags() const {
        return m_rest_flags;
    }

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
    unsigned int m_id;
    Language m_language;
    std::string m_environment_version;
    ArgumentsList m_remote_flags;
    ArgumentsList m_local_flags;
    ArgumentsList m_rest_flags;
    std::string m_input_file, m_output_file;
};

void appendList( CompileJob::ArgumentsList &list,
                 const CompileJob::ArgumentsList &toadd );
bool write_job( int fd, const CompileJob &job );
bool read_job( int in_fd, CompileJob &job );

#endif
