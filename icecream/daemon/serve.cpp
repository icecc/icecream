#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <cassert>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_SIGNAL_H
#  include <sys/signal.h>
#endif /* HAVE_SYS_SIGNAL_H */
#include <sys/param.h>

#include <job.h>
#include <comm.h>

#include "exitcode.h"
#include "tempfile.h"
#include "workit.h"
#include "logging.h"
#include "serve.h"
#include <sys/sendfile.h>
#include <exception>

using namespace std;

class myexception: public exception
{
    int code;
public:
    myexception( int _exitcode ) : exception(), code( _exitcode ) {}
    int exitcode() const { return code; }
};

/**
 * Read a request, run the compiler, and send a response.
 **/
int handle_connection( CompileJob *job, MsgChannel *serv )
{
    pid_t pid = fork();
    if ( pid == -1 )
        return EXIT_DISTCC_FAILED;

    if ( pid ) // parent
        return 0;

    if ( !scheduler || !scheduler->send_msg( JobBeginMsg( job->jobID() ) ) ) {
        log_error() << "can't reach scheduler to tell him about job start of " << job->jobID() << endl;
        exit ( EXIT_DISTCC_FAILED );
    }

    Msg *msg = 0; // The current read message
    char tmp_input[PATH_MAX];
    tmp_input[0] = 0;
    int ti = 0; // the socket
    unsigned int job_id = 0;
    int obj_fd = 0; // the obj_fd
    string obj_file;

    try {
        const char *dot;
        if (job->language() == CompileJob::Lang_C)
            dot = ".i";
        else if (job->language() == CompileJob::Lang_CXX)
            dot = ".ii";
        else
            assert(0);

    int ret;
    if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input ) ) != 0 ) {
        log_error() << "can't create tmpfile " << strerror( errno ) << endl;
        throw myexception( ret );
    }

    ti = open( tmp_input, O_CREAT|O_WRONLY|O_LARGEFILE );
    if ( ti == -1 ) {
        log_error() << "open of " << tmp_input << " failed " << strerror( errno ) << endl;
        throw myexception( EXIT_DISTCC_FAILED );
    }


    while ( 1 ) {
        msg = serv->get_msg();

        if ( !msg ) {
            log_error() << "no message while reading file chunk\n";
            throw myexception( EXIT_PROTOCOL_ERROR );
        }

        if ( msg->type == M_END ) {
            delete msg;
            msg = 0;
            break;
        }

        if ( msg->type != M_FILE_CHUNK ) {
            log_error() << "protocol error while looking for FILE_CHUNK\n";
            throw myexception( EXIT_PROTOCOL_ERROR );
        }

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>( msg );
        if ( !fcmsg ) {
            log_error() << "FileChunkMsg not dynamic_castable\n";
            throw myexception( EXIT_PROTOCOL_ERROR );
        }
        ssize_t len = fcmsg->len;
        off_t off = 0;
        while ( len ) {
            ssize_t bytes = write( ti, fcmsg->buffer + off, len );
            if ( bytes == -1 ) {
                log_error() << "write to " << tmp_input << " failed. " << strerror( errno ) << endl;
                throw myexception( EXIT_DISTCC_FAILED );
            }
            len -= bytes;
            off += bytes;
        }

        delete msg;
        msg = 0;
    }
    close( ti );
    ti = 0;

    CompileResultMsg rmsg;

    ret = work_it( *job, tmp_input,
                   rmsg.out, rmsg.err, rmsg.status, obj_file );
    unlink( tmp_input );
    tmp_input[0] = 0; // unlinked

    job_id = job->jobID();
    delete job;
    job = 0;

    if ( ret )
        throw myexception( ret );

    if ( !serv->send_msg( rmsg ) ) {
        log_info() << "write of result failed\n";
        throw myexception( EXIT_DISTCC_FAILED );
    }

    obj_fd = open( obj_file.c_str(), O_RDONLY|O_LARGEFILE );
    if ( obj_fd == -1 ) {
        log_error() << "open failed\n";
        throw myexception( EXIT_DISTCC_FAILED );
    }

    unsigned char buffer[100000];
    do {
        ssize_t bytes = read(obj_fd, buffer, sizeof(buffer));
        if (!bytes)
            break;
        FileChunkMsg fcmsg( buffer, bytes );
        if ( !serv->send_msg( fcmsg ) ) {
            log_info() << "write of chunk failed" << endl;
            throw myexception( EXIT_DISTCC_FAILED );
        }
    } while (1);

    throw myexception( 0 );

    } catch ( myexception e )
    {
        delete msg;
        delete job;

        if ( *tmp_input )
            unlink( tmp_input );

        if ( ti )
            close( ti );

        serv->send_msg( EndMsg() );

        printf( "exception %d %p\n", job_id, scheduler );
        if ( job_id && scheduler)
            scheduler->send_msg( JobDoneMsg( job_id ) );

        if ( obj_fd > 0)
            close( obj_fd );

        if ( !obj_file.empty() )
            unlink( obj_file.c_str() );
        exit( e.exitcode() );
    }
}
