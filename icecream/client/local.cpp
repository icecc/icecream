#include <string>
#include <job.h>
#include "exitcode.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "logging.h"
#include "filename.h"
#include <comm.h>
#include <sys/wait.h>

using namespace std;

extern const char * rs_program_name;

#define CLIENT_DEBUG 0

string find_compiler( const string &compiler )
{
    if ( compiler.at( 0 ) == '/' )
        return compiler;

    string path = ::getenv( "PATH" );
    string::size_type begin = 0;
    string::size_type end = 0;
    struct stat s;

    while ( end != string::npos ) {
        end = path.find_first_of( ':', begin );
        string part;
        if ( end == string::npos )
            part = path.substr( begin );
        else
            part = path.substr( begin, end - begin );
        begin = end + 1;

        part = part + '/' + compiler;
        if ( !lstat( part.c_str(), &s ) ) {
            if ( S_ISLNK( s.st_mode ) ) {
                char buffer[PATH_MAX];
                int ret = readlink( part.c_str(), buffer, PATH_MAX );
                if ( ret == -1 ) {
                    log_error() << "readlink failed " << strerror( errno ) << endl;
                    continue;
                }
                buffer[ret] = 0;
                string target = find_basename( buffer );
                if ( target == rs_program_name || target == "tbcompiler" ) {
                    // this is a link pointing to us, ignore it
                    continue;
                }
            }
            return part;
        }
    }
    log_error() << "couldn't find any " << compiler << endl;
    return string();
}

/**
 * Invoke a compiler locally.  This is, obviously, the alternative to
 * dcc_compile_remote().
 *
 * The server does basically the same thing, but it doesn't call this
 * routine because it wants to overlap execution of the compiler with
 * copying the input from the network.
 *
 * This routine used to exec() the compiler in place of distcc.  That
 * is slightly more efficient, because it avoids the need to create,
 * schedule, etc another process.  The problem is that in that case we
 * can't clean up our temporary files, and (not so important) we can't
 * log our resource usage.
 *
 * This is called with a lock on localhost already held.
 **/
int build_local(CompileJob &job, MsgChannel *scheduler)
{
    std::list<string> arguments;

    string compiler_name = "gcc";
    if ( job.language() == CompileJob::Lang_CXX )
        compiler_name = "g++";
    compiler_name = find_compiler( compiler_name );

    if ( compiler_name.empty() )
        return EXIT_NO_SUCH_FILE;
    arguments.push_back( compiler_name );
    appendList( arguments, job.allFlags() );

    if ( !job.inputFile().empty() )
        arguments.push_back( job.inputFile() );
    if ( !job.outputFile().empty() ) {
        arguments.push_back( "-o" );
        arguments.push_back( job.outputFile() );
    }
    char **argv = new char*[arguments.size() + 1];
    int argc = 0;
    for ( std::list<string>::const_iterator it = arguments.begin();
          it != arguments.end(); ++it )
        argv[argc++] = strdup( it->c_str() );
    argv[argc] = 0;
#if CLIENT_DEBUG
    trace() << "execing ";
    for ( int i = 0; argv[i]; i++ )
        trace() << argv[i] << " ";
    trace() << endl;
#endif
    pid_t child = 0;

    if ( scheduler )
        child = fork();

    if ( child ) {
        scheduler->send_msg( JobLocalBeginMsg() );
        Msg * umsg = scheduler->get_msg();
        uint job_id = 0;
        if (!umsg || umsg->type != M_JOB_LOCAL_ID)
        {
            log_error() << "replied not with local job id " << ( umsg ? ( char )umsg->type : '0' )  << endl;
            delete umsg;
            scheduler = 0;
        } else
            job_id = dynamic_cast<JobLocalId*>( umsg )->job_id;

        int status;
        wait( &status );
        status = WEXITSTATUS(status);
        if ( scheduler )
            scheduler->send_msg( JobLocalDoneMsg( job_id, status ) );
        return status;
    } else {
        return execv( argv[0], argv ); // if it returns at all
    }
}
