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

// the pipe is needed to sync the child reaping with our event processing,
// as otherwise there are race conditions, locking requirements, and things
// generally get harder
void theSigCHLDHandler( int arg )
{
  int saved_errno = errno;

  char dummy = 0;
  printf( "theSig %d", arg );

  errno = saved_errno;
}


int work_it( CompileJob &j, const char *preproc, size_t preproc_length )
{
    CompileJob::ArgumentsList list = j.compileFlags();
    int ret;

     // teambuilder needs faking
    const char *dot;
    if (j.language() == CompileJob::Lang_C)
        dot = ".c";
    else if (j.language() == CompileJob::Lang_CXX)
        dot = ".cpp";
    else
        assert(0);
    list.push_back( "-fpreprocessing" );

    // if using gcc: dot = dcc_preproc_exten( dot );
    char tmp_input[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input ) ) != 0 )
        return ret;
    char tmp_output[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", ".o", tmp_output ) ) != 0 )
        return ret;

    FILE *ti = fopen( tmp_input, "wt" );
    fwrite( preproc, 1, preproc_length, ti );
    fclose( ti );

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

    /* Testing */
    struct sigaction act;
    sigemptyset( &act.sa_mask );

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigaction( SIGPIPE, &act, 0L );

    act.sa_handler = theSigCHLDHandler;
    act.sa_flags = SA_NOCLDSTOP;
    // CC: take care of SunOS which automatically restarts interrupted system
    // calls (and thus does not have SA_RESTART)
#ifdef SA_RESTART
    act.sa_flags |= SA_RESTART;
#endif
    sigaction( SIGCHLD, &act, 0 );

    sigaddset( &act.sa_mask, SIGCHLD );
    // Make sure we don't block this signal. gdb tends to do that :-(
    sigprocmask( SIG_UNBLOCK, &act.sa_mask, 0 );

    pid_t pid = fork();
    if ( pid == -1 )
        return EXIT_OUT_OF_MEMORY;
    else if ( pid == 0 ) {

        close( main_sock[0] );
        fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);

        int argc = list.size();
        argc++; // the program
        argc += 4; // -c file.i -o file.o
        const char **argv = new const char*[argc + 1];
        if (j.language() == CompileJob::Lang_C)
            argv[0] = "/opt/teambuilder/bin/gcc";
        else if (j.language() == CompileJob::Lang_CXX)
            argv[0] = "/opt/teambuilder/bin/g++";
        else
            assert(0);
        int i = 1;
        for ( CompileJob::ArgumentsList::const_iterator it = list.begin();
              it != list.end(); ++it) {
            argv[i++] = it->c_str();
        }
        argv[i++] = "-c";
        argv[i++] = tmp_input;
        argv[i++] = "-o";
        argv[i++] = tmp_output;
        argv[i] = 0;
        printf( "forking " );
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

        close( main_sock[1] );

        // idea borrowed from kprocess
        for(;;)
        {
            char resultByte;
            int n = ::read(main_sock[0], &resultByte, 1);
            if (n == 1)
            {
                // exec() failed
                close(main_sock[0]);
                waitpid(pid, 0, 0);
                return EXIT_COMPILER_CRASHED;
            }
            if (n == -1)
            {
                if (errno == EINTR)
                    continue; // Ignore
            }
            break; // success
        }
        close( main_sock[0] );

        int status;

        for(;;)
        {
            fd_set rfds;
            FD_ZERO( &rfds );
            FD_SET( sock_out[0], &rfds );
            FD_SET( sock_err[0], &rfds );
            fd_set efds;
            FD_ZERO( &efds );
            FD_SET( sock_out[0], &efds );
            FD_SET( sock_err[0], &efds );

            struct timeval tv;
            /* Wait up to five seconds. */
            tv.tv_sec = 5;
            tv.tv_usec = 0;

            ret =  select( std::max( sock_out[0], sock_err[0] )+1, &rfds, 0, &efds, &tv );
            printf( "ret %d\n", ret );
            switch( ret )
            {
            case -1:
                printf( "errno %s\n", strerror( errno ) );
                if( errno == EINTR )
                    break;
                // fall through; should happen if tvp->tv_sec < 0
            case 0:
                continue;
            default:
                if ( FD_ISSET(sock_out[0], &rfds) ) {
                    char buffer[4096];
                    printf( "stdout:\n" );
                    ssize_t bytes = read( sock_out[0], buffer, 4096 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        printf( "%s\n", buffer );
                    }
                }
                if ( FD_ISSET(sock_err[0], &rfds) ) {
                    char buffer[4096];
                    printf( "stderr:\n" );
                    ssize_t bytes = read( sock_err[0], buffer, 4096 );
                    if ( bytes > 0 ) {
                        buffer[bytes] = 0;
                        printf( "%s\n", buffer );
                    }
                }
                if ( FD_ISSET(sock_out[0], &efds) || FD_ISSET( sock_err[0], &efds ) ) {
                    printf( "error\n" );
                }
                struct rusage ru;
                if (wait4(pid, &status, WNOHANG, &ru) != 0) // error finishes, too
                {
                    printf( "has exited: %d %d\n", pid, status );
                    return true;
                } else {
                    printf( "not yet exited\n" );
                }
            }
        }
    }

#if 0
    if ((ret = dcc_x_result_header(out_fd, protover))
        || (ret = dcc_x_cc_status(out_fd, status))
        || (ret = dcc_x_file(out_fd, err_fname, "SERR", compr))
        || (ret = dcc_x_file(out_fd, out_fname, "SOUT", compr))
        || WIFSIGNALED(status)
        || WEXITSTATUS(status)) {
        /* Something went wrong, so send DOTO 0 */
        dcc_x_token_int(out_fd, "DOTO", 0);
    } else {
        ret = dcc_x_file(out_fd, temp_o, "DOTO", compr);
    }
#endif

    return 0;
}
