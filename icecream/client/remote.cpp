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
    Service *serv = new Service ("localhost", 10245);
    MsgChannel *local_daemon = serv->channel();
    if ( ! local_daemon ) {
        log_error() << "no local daemon found\n";
        return build_local( job );
    }
    if ( !local_daemon->send_msg( GetSchedulerMsg() ) ) {
        log_error() << "failed to write get scheduler\n";
        return build_local( job );
    }
    log_info() << "asked for scheduler\n";

    Msg *umsg = local_daemon->get_msg();
    if ( !umsg || umsg->type != M_USE_SCHEDULER ) {
        log_error() << "umsg != scheduler\n";
        delete local_daemon;
        delete serv;
        return build_local( job );
    }
    UseSchedulerMsg *ucs = dynamic_cast<UseSchedulerMsg*>( umsg );
    delete local_daemon;
    delete serv;

    log_info() << "contacting scheduler " << ucs->hostname << ":" << ucs->port << endl;

    serv = new Service( ucs->hostname, ucs->port );
    MsgChannel *scheduler = serv->channel();
    if ( ! scheduler ) {
        log_error() << "no scheduler found at " << ucs->hostname << ":" << ucs->port << endl;
        return build_local( job );
    }
    delete ucs;

    // TODO: getenv("ICECC_VERSION")
    GetCSMsg getcs ("gcc33", get_absfilename( job.inputFile() ), job.language() );
    if (!scheduler->send_msg (getcs)) {
        log_error() << "asked for CS\n";
        delete serv;
        return build_local( job );
    }
    umsg = scheduler->get_msg();
    if (!umsg || umsg->type != M_USE_CS)
    {
        log_error() << "replied not with use_cs " << ( umsg ? ( char )umsg->type : '0' )  << endl;
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

    CompileFileMsg compile_file( &job );
    if ( !cserver->send_msg( compile_file ) ) {
        log_info() << "write of job failed" << endl;
        delete cserver;
        delete serv;
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

    unsigned char buffer[50000]; // some random but huge number

    do {
        ssize_t bytes = read(sockets[0], buffer, sizeof(buffer));
        if (!bytes)
            break;
        FileChunkMsg fcmsg( buffer, bytes );
        if ( !cserver->send_msg( fcmsg ) ) {
            log_info() << "write of chunk failed" << endl;
            close(sockets[0]);
            return build_local( job );
        }
    } while (1);

    EndMsg emsg;
    if ( !cserver->send_msg( emsg ) ) {
        log_info() << "write of end failed" << endl;
        return build_local( job );
    }

    Msg *msg = cserver->get_msg();
    if ( !msg )
        return build_local( job );

    if ( msg->type != M_COMPILE_RESULT )
        return EXIT_PROTOCOL_ERROR;

    CompileResultMsg *crmsg = dynamic_cast<CompileResultMsg*>( msg );
    assert ( crmsg );

    int status = crmsg->status;
    fprintf( stdout, "%s", crmsg->out.c_str() );
    fprintf( stderr, "%s", crmsg->err.c_str() );

    assert( !job.outputFile().empty() );
    int obj_fd = open( job.outputFile().c_str(),
                       O_CREAT|O_TRUNC|O_WRONLY|O_LARGEFILE, 0666 );

    if ( obj_fd == -1 ) {
        log_error() << "open failed\n";
        return EXIT_DISTCC_FAILED;
    }

    while ( 1 ) {
        msg = cserver->get_msg();
        if ( msg->type == M_END )
            break;

        if ( msg->type != M_FILE_CHUNK )
            return EXIT_PROTOCOL_ERROR;

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>( msg );
        if ( write( obj_fd, fcmsg->buffer, fcmsg->len ) != ( ssize_t )fcmsg->len )
            return EXIT_DISTCC_FAILED;
    }

    close( obj_fd );

    log_info() << "got status " << status << endl;
    return status;
}
