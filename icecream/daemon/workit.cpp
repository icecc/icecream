#include "workit.h"
#include "tempfile.h"
#include "assert.h"
#include "exitcode.h"

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

    pipe( sock_err );
    pipe( sock_out );

    pid_t pid = fork();
    if ( pid == -1 )
        return EXIT_OUT_OF_MEMORY;
    else if ( pid == 0 ) {

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
        dup2( sock_out[0], STDOUT_FILENO );
        close( STDERR_FILENO );
        dup2( sock_err[0], STDERR_FILENO );

        execvp( argv[0], const_cast<char *const*>( argv ) ); // no return
        exit( 255 );
    } else {
    }
#if 0
    if ((compile_ret = dcc_spawn_child(argv, &cc_pid,
                                       "/dev/null", out_fname, err_fname))
        || (compile_ret = dcc_collect_child("cc", cc_pid, &status))) {
        /* We didn't get around to finding a wait status from the actual compiler */
        status = W_EXITCODE(compile_ret, 0);
    }

    if ((ret = dcc_check_compiler_masq(argv[0])))
        goto out_cleanup;

    if ((compile_ret = dcc_spawn_child(argv, &cc_pid,
                                       "/dev/null", out_fname, err_fname))
        || (compile_ret = dcc_collect_child("cc", cc_pid, &status))) {
        /* We didn't get around to finding a wait status from the actual compiler */
        status = W_EXITCODE(compile_ret, 0);
    }

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

    // dcc_critique_status(status, argv[0], dcc_hostdef_local, 0);
    // tcp_cork_sock(out_fd, 0);

    // rs_log(RS_LOG_INFO|RS_LOG_NONAME, "job complete");
#endif

    return 0;
}
