#include <job.h>
#include "local.h"
#include "exitcode.h"
#include <client_comm.h>
#include "logging.h"
#include "cpp.h"
#include <cassert>
#include <comm.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sendfile.h>

using namespace std;

string get_absfilename( const string &_file )
{
    string file;

    assert( !_file.empty() );
    if ( _file.at( 0 ) != '/' ) {
        static char buffer[PATH_MAX];

#ifdef HAVE_GETCWD
        getcwd(buffer, sizeof( buffer ) );
#else
        getwd(buffer);
#endif
        buffer[PATH_MAX - 1] = 0;
        file = buffer + '/' + _file;
    } else {
        file = _file;
    }

    string::size_type idx = file.find( "/.." );
    while ( idx != string::npos ) {
        file.replace( idx, 3, "/" );
        idx = file.find( "/.." );
    }
    idx = file.find( "/." );
    while ( idx != string::npos ) {
        file.replace( idx, 2, "/" );
        idx = file.find( "/." );
    }
    idx = file.find( "//" );
    while ( idx != string::npos ) {
        file.replace( idx, 2, "/" );
        idx = file.find( "//" );
    }
    return file;
}

int build_remote(CompileJob &job )
{
    Service *serv = new Service ("localhost", 8765);
    MsgChannel *scheduler = serv->channel();
    if ( ! scheduler ) {
        log_error() << "no scheduler found\n";
        return build_local( job );
    }

    // TODO: getenv("ICECC_VERSION")
    GetCSMsg getcs ("gcc33", get_absfilename( job.inputFile() ), job.language() );
    if (!scheduler->send_msg (getcs)) {
        delete serv;
        return build_local( job );
    }
    Msg *umsg = scheduler->get_msg();
    if (!umsg || umsg->type != M_USE_CS)
    {
       delete umsg;
       delete serv;
       return build_local( job );
    }
    UseCSMsg *usecs = dynamic_cast<UseCSMsg *>(umsg);
    string hostname = usecs->hostname;
    unsigned int port = usecs->port;
    job.setJobID( usecs->job_id );
    printf ("Have to use host %s:%d\n", hostname.c_str(), port );
    printf ("Job ID: %d\n", job.jobID());
    delete usecs;
    EndMsg em;
    // if the scheduler ignores us, ignore it in return :/
    ( void )scheduler->send_msg (em);
    delete scheduler;
    delete serv;

    serv = new Service (hostname, port);
    MsgChannel *cserver = serv->channel();
    if ( ! cserver ) {
        log_error() << "no server found behind given hostname " << hostname << ":" << port << endl;
        delete cserver;
        delete serv;
        return build_local( job );
    }

    int out_fd = cserver->fd;

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
