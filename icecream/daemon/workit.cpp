#include "workit.h"
#include "tempfile.h"
#include "assert.h"
#include "exitcode.h"

#include <sys/select.h>

/* According to earlier standards */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string>

using namespace std;

volatile bool must_reap = false;

void theSigCHLDHandler( int )
{
    must_reap = true;
}

int work_it( CompileJob &j,
             const string& infilename,
             string &str_out, string &str_err,
             int &status, string &outfilename )
{
    str_out.clear();
    str_out.clear();

    CompileJob::ArgumentsList list = j.remoteFlags();
    appendList( list, j.restFlags() );
    int ret;

    char tmp_output[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", ".o", tmp_output ) ) != 0 )
        return ret;
    outfilename = tmp_output;

    int sock_err[2];
    int sock_out[2];
    int main_sock[2];

    pipe( sock_err );
    pipe( sock_out );
    pipe( main_sock );

    fcntl( sock_out[0], F_SETFL, O_NONBLOCK );
    fcntl( sock_err[0], F_SETFL, O_NONBLOCK );

    fcntl( sock_out[0], F_SETFD, FD_CLOEXEC );
    fcntl( sock_err[0], F_SETFD, FD_CLOEXEC );
    fcntl( sock_out[1], F_SETFD, FD_CLOEXEC );
    fcntl( sock_err[1], F_SETFD, FD_CLOEXEC );

    must_reap = true;

    /* Testing */
    struct sigaction act;
    sigemptyset( &act.sa_mask );

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigaction( SIGPIPE, &act, 0L );

    act.sa_handler = theSigCHLDHandler;
    act.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction( SIGCHLD, &act, 0 );

    sigaddset( &act.sa_mask, SIGCHLD );
    // Make sure we don't block this signal. gdb tends to do that :-(
    sigprocmask( SIG_UNBLOCK, &act.sa_mask, 0 );

    pid_t pid = fork();
    if ( pid == -1 ) {
        unlink( tmp_output );
        return EXIT_OUT_OF_MEMORY;
    } else if ( pid == 0 ) {

        close( main_sock[0] );
        fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);

        int argc = list.size();
        argc++; // the program
        argc += 4; // file.i -o file.o
        char **argv = new char*[argc + 1];
        if (j.language() == CompileJob::Lang_C)
            argv[0] = strdup( "/opt/teambuilder/bin/gcc" );
        else if (j.language() == CompileJob::Lang_CXX)
            argv[0] = strdup( "/opt/teambuilder/bin/g++" );
        else
            assert(0);
        int i = 1;
        for ( CompileJob::ArgumentsList::const_iterator it = list.begin();
              it != list.end(); ++it) {
            argv[i++] = strdup( it->c_str() );
        }
        argv[i++] = strdup( infilename.c_str() );
        argv[i++] = strdup( "-o" );
        argv[i++] = tmp_output;
        argv[i] = 0;
        printf( "forking2 " );
        for ( int index = 0; argv[index]; index++ )
            printf( "%s ", argv[index] );
        printf( "\n" );

        close( STDOUT_FILENO );
        close( sock_out[0] );
        dup2( sock_out[1], STDOUT_FILENO );
        close( STDERR_FILENO );
        close( sock_err[0] );
        dup2( sock_err[1], STDERR_FILENO );

        ret = execvp( argv[0], const_cast<char *const*>( argv ) ); // no return
        printf( "all failed\n" );

        char resultByte = 1;
        write(main_sock[1], &resultByte, 1);
        exit(-1);
    } else {
        char buffer[4096];

        close( main_sock[1] );

        // idea borrowed from kprocess
        for(;;)
        {
            char resultByte;
            int n = ::read(main_sock[0], &resultByte, 1);
            if (n == 1)
            {
                status = resultByte;
                // exec() failed
                close(main_sock[0]);
                waitpid(pid, 0, 0);
                unlink( tmp_output );
                return EXIT_COMPILER_MISSING; // most likely cause
            }
            if (n == -1)
            {
                if (errno == EINTR)
                    continue; // Ignore
            }
            break; // success
        }
        close( main_sock[0] );

        for(;;)
        {
            fd_set rfds;
            FD_ZERO( &rfds );
            FD_SET( sock_out[0], &rfds );
            FD_SET( sock_err[0], &rfds );

            struct timeval tv;
            /* Wait up to five seconds. */
            tv.tv_sec = 5;
            tv.tv_usec = 0;

            ret =  select( std::max( sock_out[0], sock_err[0] )+1, &rfds, 0, 0, &tv );
            printf( "selected %d %d\n", ret, errno );
            switch( ret )
            {
            case -1:
                // fall through; should happen if tvp->tv_sec < 0
            case 0:
                struct rusage ru;
                if (wait4(pid, &status, must_reap ? 0 : WNOHANG, &ru) != 0) // error finishes, too
                {
                    printf( "has exited: %d %d\n", pid, status );
                    return 0;
                }
                break;
            default:
                if ( FD_ISSET(sock_out[0], &rfds) ) {
                    ssize_t bytes = read( sock_out[0], buffer, 4096 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        str_out.append( buffer );
                    }
                }
                if ( FD_ISSET(sock_err[0], &rfds) ) {
                    ssize_t bytes = read( sock_err[0], buffer, 4096 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        str_err.append( buffer );
                    }
                }
            }
        }
    }
    assert( false );
    return 0;
}
