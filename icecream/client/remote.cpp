#include <job.h>
#include "local.h"
#include "clinet.h"
#include "exitcode.h"
#include <client_comm.h>
#include "logging.h"
#include "cpp.h"
#include <cassert>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sendfile.h>

using namespace std;

/**
 * Execute the commands in argv remotely or locally as appropriate.
 *
 * We may need to run cpp locally; we can do that in the background
 * while trying to open a remote connection.
 *
 * This function is slightly inefficient when it falls back to running
 * gcc locally, because cpp may be run twice.  Perhaps we could adjust
 * the command line to pass in the .i file.  On the other hand, if
 * something has gone wrong, we should probably take the most
 * conservative course and run the command unaltered.  It should not
 * be a big performance problem because this should occur only rarely.
 *
 * @param argv Command to execute.  Does not include 0='distcc'.
 * Treated as read-only, because it is a pointer to the program's real
 * argv.
 *
 * @param status On return, contains the waitstatus of the compiler or
 * preprocessor.  This function can succeed (in running the compiler) even if
 * the compiler itself fails.  If either the compiler or preprocessor fails,
 * @p status is guaranteed to hold a failure value.
 **/
int build_remote(CompileJob &job )
{
    int out_fd;

    /*
    Scheduler_Stub scheduler = Scheduler::findScheduler();
    if ( scheduler.isNull() ) {
        // TODO: goto run_local
        printf( "No Scheduler found\n" );
        return -1;
    }
    Server_Stub server = scheduler.findServer( j ); // sets job-ID
    if ( server.isNull() ) {
        // TODO: goto run_local
        printf( "No Server found\n" );
        return -1;
    }
    */

    if ( dcc_connect_by_name( "localhost", 7000, &out_fd) )
        return build_local( job );

    log_info() << "got connection: fd=" << out_fd << endl;

    int version = ICECC_PROTO_VERSION;

    if ( client_write_message( out_fd, C_VERSION, version) ) {
        log_info() << "write of version failed" << endl;
        return build_local( job );
    }

    if ( !write_job( out_fd, job ) ) {
        log_info() << "write of job failed" << endl;
        return build_local( job );
    }

    int sockets[2];
    if (pipe(sockets)) {
        /* for all possible cases, this is something severe */
        exit(errno);
    }

    pid_t cpp_pid = call_cpp(job, sockets[1] );
    if ( cpp_pid == -1 )
        return build_local( job );
    close(sockets[1]);

    char buffer[14097];

    do {
        ssize_t bytes = read(sockets[0], buffer, sizeof(buffer));
        if (!bytes)
            break;
        if ( client_write_message( out_fd, C_PREPROC, bytes) ) {
            log_info() << "failed to write preproc header" << endl;
            return build_local( job );
        }
        if (write(out_fd, buffer, bytes) != bytes) {
            log_info() << "write failed" << endl;
            close(sockets[0]);
            close(out_fd);
            return build_local( job );
        }
    } while (1);

    if ( client_write_message( out_fd, C_DONE, 0) )
        return build_local( job );

    Client_Message m;
    client_read_message( out_fd, &m );
    if ( m.type != C_STATUS) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }
    int status = m.length;

    client_read_message( out_fd, &m );
    if ( m.type != C_STDOUT) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }

    assert(m.length < sizeof( buffer )); // for now :/
    read(out_fd, buffer, m.length);
    buffer[m.length] = 0;
    fprintf(stdout, "%s", buffer);

    client_read_message( out_fd, &m );
    if ( m.type != C_STDERR) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }

    while ( m.length >= sizeof( buffer ) ) {
        ssize_t n = read( out_fd, buffer, sizeof( buffer ) - 1);
        buffer[n] = 0;
        fprintf(stderr, "%s", buffer);
        m.length -= n;
    }
    assert(m.length < sizeof( buffer ));
    read(out_fd, buffer, m.length);
    buffer[m.length] = 0;
    fprintf(stderr, "%s", buffer);

    client_read_message( out_fd, &m );
    if ( m.type != C_OBJ) {
        close( out_fd );
        return EXIT_PROTOCOL_ERROR;
    }

    assert( !job.outputFile().empty() );
    int obj_fd = open( job.outputFile().c_str(),
                       O_CREAT|O_TRUNC|O_WRONLY|O_LARGEFILE, 0666 );
    if ( obj_fd == -1 ) {
        log_error() << "open failed\n";
        return EXIT_DISTCC_FAILED;
    }
    while ( m.length > 0 ) {
        ssize_t n = read( out_fd, buffer, min( m.length, sizeof( buffer ) - 1) );
        buffer[n] = 0;
        if ( write( obj_fd, buffer, n ) != n ) {
            log_error() << "writing failed\n";
            return EXIT_DISTCC_FAILED;
        }
        m.length -= n;
    }

    close( obj_fd );
    close( out_fd );
    log_info() << "got status " << status << endl;
    return status;
}
