#include "workit.h"
#include "tempfile.h"

int work_it( CompileJob &j )
{
    CompileJob::ArgumentsList list = j.compileFlags();
    int ret;

     // teambuilder needs faking
    const char *dot = ".c";
    list.push_back( "-fpreprocessing" );

    // if using gcc: dot = dcc_preproc_exten( dot );
    char tmp_input[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", dot, tmp_input ) ) != 0 )
        return ret;
    char tmp_output[PATH_MAX];
    if ( ( ret = dcc_make_tmpnam("icecc", ".o", tmp_output ) ) != 0 )
        return ret;

    FILE *ti = fopen( tmp_input, "wt" );
    // TODO! fwrite( preproc, 1, preproc_length, ti );
    fclose( ti );

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
