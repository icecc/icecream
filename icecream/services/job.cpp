#include "job.h"
#include "logging.h"
#include "client_comm.h"
#include "exitcode.h"

using namespace std;

void appendList( CompileJob::ArgumentsList &list,
                        const CompileJob::ArgumentsList &toadd )
{
    for ( CompileJob::ArgumentsList::const_iterator it = toadd.begin();
          it != toadd.end(); ++it )
        list.push_back( *it );
}

static bool write_argv( int fd, const CompileJob::ArgumentsList &list)
{
    if ( client_write_message( fd, C_ARGC, list.size() ) )
        return false;

    for (CompileJob::ArgumentsList::const_iterator it = list.begin();
         it != list.end(); ++it ) {
        ssize_t len = it->size();
        if ( client_write_message( fd, C_ARGV, len) )
            return false;
        if (write(fd, it->c_str(), len) != len)
            return false;
    }
    return true;
}

bool write_job( int fd, const CompileJob &job )
{
    int lang = job.language();
    if ( client_write_message( fd, C_LANG, lang ) != 0 ) {
        log_info() << "write of lang failed" << std::endl;
        return false;
    }

    CompileJob::ArgumentsList args = job.remoteFlags();
    appendList( args, job.restFlags() );
    if ( !write_argv( fd, args) ) {
        log_info() << "write of arguments failed" << std::endl;
        return false;
    }

    return true;
}

bool read_job( int in_fd, CompileJob &job )
{
    Client_Message m;
    int ret = client_read_message( in_fd, &m );
    if ( ret )
        return ret;

    if ( m.type != C_LANG ) {
        log_info() << "didn't get language\n";
        return EXIT_PROTOCOL_ERROR;
    }
    job.setLanguage( static_cast<CompileJob::Language>(  m.length ) );

    ret = client_read_message( in_fd, &m );
    if ( ret )
        return ret;

    if ( m.type != C_ARGC ) {
        log_info() << "didn't get argc\n";
        return EXIT_PROTOCOL_ERROR;
    }

    int argc = m.length;
    CompileJob::ArgumentsList args;

    char buffer[4096];
    for ( int i = 0; i < argc; i++ ) {
        ret = client_read_message( in_fd, &m );
        if ( ret )
            return ret;
        if ( m.type != C_ARGV )
            return EXIT_PROTOCOL_ERROR;
        if ( m.length >= 4096 )
            return EXIT_PROTOCOL_ERROR;
        read( in_fd, buffer, m.length );
        buffer[m.length] = 0;
        args.push_back( buffer );
    }
    job.setRestFlags( args );

    // TODO: PROF data if available
    return true;
}
