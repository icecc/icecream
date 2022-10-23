/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

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
#include <unistd.h>

#include <job.h>
#include <comm.h>

#include "environment.h"
#include "exitcode.h"
#include "tempfile.h"
#include "workit.h"
#include "logging.h"
#include "serve.h"
#include "util.h"
#include "file_util.h"

#include <sys/time.h>

#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#ifndef _PATH_TMP
#define _PATH_TMP "/tmp"
#endif

using namespace std;

int nice_level = 5;

static void
error_client(MsgChannel *client, string error)
{
    if (IS_PROTOCOL_VERSION(22, client)) {
        client->send_msg(StatusTextMsg(error));
    }
}

static void write_output_file( const string& file, MsgChannel* client )
{
    int obj_fd = -1;
    try {
        obj_fd = open(file.c_str(), O_RDONLY | O_LARGEFILE);

        if (obj_fd == -1) {
            log_error() << "open failed" << endl;
            error_client(client, "open of object file failed");
            throw myexception(EXIT_DISTCC_FAILED);
        }

        unsigned char buffer[100000];

        do {
            ssize_t bytes = read(obj_fd, buffer, sizeof(buffer));

            if (bytes < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw myexception(EXIT_DISTCC_FAILED);
            }

            if (!bytes) {
                if( !client->send_msg(EndMsg())) {
                    log_info() << "write of obj end failed " << endl;
                    throw myexception(EXIT_DISTCC_FAILED);
                }
                break;
            }

            FileChunkMsg fcmsg(buffer, bytes);

            if (!client->send_msg(fcmsg)) {
                log_info() << "write of obj chunk failed " << bytes << endl;
                throw myexception(EXIT_DISTCC_FAILED);
            }
        } while (1);

    } catch(...) {
        if( obj_fd != -1 )
            if ((-1 == close( obj_fd )) && (errno != EBADF)){
                log_perror("close failed");
            }
        throw;
    }
}

/**
 * Read a request, run the compiler, and send a response.
 **/
int handle_connection(const string &basedir, CompileJob *job,
                      MsgChannel *client, int &out_fd,
                      unsigned int mem_limit, uid_t user_uid, gid_t user_gid)
{
    int socket[2];

    if (pipe(socket) == -1) {
        log_perror("pipe failed");
        return -1;
    }

    flush_debug();
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid > 0) {  // parent
        if ((-1 == close(socket[1])) && (errno != EBADF)){
            log_perror("close failure");
        }
        out_fd = socket[0];
        fcntl(out_fd, F_SETFD, FD_CLOEXEC);
        return pid;
    }

    reset_debug();
    if ((-1 == close(socket[0])) && (errno != EBADF)){
        log_perror("close failed");
    }
    out_fd = socket[1];

    /* internal communication channel, don't inherit to gcc */
    fcntl(out_fd, F_SETFD, FD_CLOEXEC);

    int niceval = nice(nice_level);
    if (niceval == -1) {
        log_warning() << "failed to set nice value: " << strerror(errno)
                      << endl;
    }

    string tmp_path, obj_file, dwo_file;
    int exit_code = 0;
    unsigned int job_stat[8];
    memset(job_stat, 0, sizeof(job_stat));

    try {
        if (job->environmentVersion().size()) {
            string dirname = basedir + "/target=" + job->targetPlatform() + "/" + job->environmentVersion();

            if (::access(string(dirname + "/usr/bin/as").c_str(), X_OK) < 0) {
                error_client(client, dirname + "/usr/bin/as is not executable, installed environment removed?");
                log_error() << "I don't have environment " << job->environmentVersion() << "(" << job->targetPlatform() << ") " << job->jobID() << endl;
                // The scheduler didn't listen to us, or maybe something has removed the files.
                throw myexception(EXIT_COMPILER_MISSING);
            }

            chdir_to_environment(client, dirname, user_uid, user_gid);
        } else {
            error_client(client, "empty environment");
            log_error() << "Empty environment (" << job->targetPlatform() << ") " << job->jobID() << endl;
            throw myexception(EXIT_DISTCC_FAILED);
        }

        if (::access(&_PATH_TMP[1], W_OK) < 0) {
            error_client(client, "can't write to " _PATH_TMP);
            log_error() << "can't write into " << _PATH_TMP << " " << strerror(errno) << endl;
            throw myexception(-1);
        }

        int ret;
        CompileResultMsg rmsg;
        unsigned int job_id = job->jobID();

        char *tmp_output = nullptr;
        char prefix_output[32]; // 20 for 2^64 + 6 for "icecc-" + 1 for trailing NULL
        sprintf(prefix_output, "icecc-%u", job_id);

        if (job->dwarfFissionEnabled() && (ret = dcc_make_tmpdir(&tmp_output)) == 0) {
            tmp_path = tmp_output;
            free(tmp_output);

            // dwo information is embedded in the final object file, but the compiler
            // hard codes the path to the dwo file based on the given path to the
            // object output file. In every case, we must recreate the directory structure of
            // the client system inside our tmp directory, including both the working
            // directory the compiler will be run from as well as the relative path from
            // that directory to the specified output file.
            //
            // the work_it() function will rewrite the tmp build directory as root, effectively
            // letting us set up a "chroot"ed environment inside the build folder and letting
            // us set up the paths to mimic the client system

            string job_output_file = job->outputFile();
            string job_working_dir = job->workingDirectory();

            size_t slash_index = job_output_file.rfind('/');
            string file_dir, file_name;
            if (slash_index != string::npos) {
                file_dir = job_output_file.substr(0, slash_index);
                file_name = job_output_file.substr(slash_index+1);
            }
            else {
                file_name = job_output_file;
            }

            string output_dir, relative_file_path;
            if (!file_dir.empty() && file_dir[0] == '/') { // output dir is absolute, convert to relative
                relative_file_path = get_relative_path(get_canonicalized_path(job_output_file), get_canonicalized_path(job_working_dir));
                output_dir = tmp_path + get_canonicalized_path(file_dir);
            }
            else { // output file is already relative, canonicalize in relation to working dir
                string canonicalized_dir = get_canonicalized_path(job_working_dir + '/' + file_dir);
                relative_file_path = get_relative_path(canonicalized_dir + '/' + file_name, get_canonicalized_path(job_working_dir));
                output_dir = tmp_path + canonicalized_dir;
            }

            if (!mkpath(output_dir)) {
                error_client(client, "could not create object file location in tmp directory");
                throw myexception(EXIT_IO_ERROR);
            }
            if (!mkpath(tmp_path + job_working_dir))  {
                error_client(client, "could not create compiler working directory in tmp directory");
                throw myexception(EXIT_IO_ERROR);
            }

            obj_file = output_dir + '/' + file_name;
            dwo_file = obj_file.substr(0, obj_file.rfind('.')) + ".dwo";

            ret = work_it(*job, job_stat, client, rmsg, tmp_path, job_working_dir, relative_file_path, mem_limit, client->fd);
        }
        else if (!job->dwarfFissionEnabled() && (ret = dcc_make_tmpnam(prefix_output, ".o", &tmp_output, 0)) == 0) {
            obj_file = tmp_output;
            free(tmp_output);
            string build_path = obj_file.substr(0, obj_file.rfind('/'));
            string file_name = obj_file.substr(obj_file.rfind('/')+1);

            ret = work_it(*job, job_stat, client, rmsg, build_path, "", file_name, mem_limit, client->fd);
        }

        if (ret) {
            if (ret == EXIT_OUT_OF_MEMORY) {   // we catch that as special case
                rmsg.was_out_of_memory = true;
            } else if (ret == EXIT_IO_ERROR) {
                // This was probably running out of disk space.
                // Fake that as running out of memory, since it's in practice
                // a very similar problem.
                rmsg.was_out_of_memory = true;
            } else {
                throw myexception(ret);
            }
        }

        struct stat st;
        if (stat(obj_file.c_str(), &st) == 0) {
            job_stat[JobStatistics::out_uncompressed] += st.st_size;
        }
        if (stat(dwo_file.c_str(), &st) == 0) {
            job_stat[JobStatistics::out_uncompressed] += st.st_size;
            rmsg.have_dwo_file = true;
        } else
            rmsg.have_dwo_file = false;

        if (!client->send_msg(rmsg)) {
            log_info() << "write of result failed" << endl;
            throw myexception(EXIT_DISTCC_FAILED);
        }

        /* wake up parent and tell him that compile finished */
        /* if the write failed, well, doesn't matter */
        ignore_result(write(out_fd, job_stat, sizeof(job_stat)));
        if ((-1 == close(out_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }

        if (rmsg.status == 0) {
            write_output_file(obj_file, client);
            if (rmsg.have_dwo_file) {
                write_output_file(dwo_file, client);
            }
        }

        exit_code = rmsg.status;

    } catch (const myexception& e) {
        exit_code = e.exitcode();
        assert(exit_code != 0);
        // There is nothing that would actually collect and care about the exit
        // status of this process. Make sure to send the exit code (i.e. error code)
        // using the pipe where it will be read and acted upon if needed.
        job_stat[JobStatistics::exit_code] = exit_code;
        if(out_fd != -1)
        {
            ignore_result(write(out_fd, job_stat, sizeof(job_stat)));
            close(out_fd);
        }
    }

    delete client;
    client = nullptr;

    if (!obj_file.empty()) {
        if (-1 == unlink(obj_file.c_str()) && errno != ENOENT){
            log_perror("unlink failure") << "\t" << obj_file << endl;
        }
    }
    if (!dwo_file.empty()) {
        if (-1 == unlink(dwo_file.c_str()) && errno != ENOENT){
            log_perror("unlink failure") << "\t" << dwo_file << endl;
        }
    }
    if (!tmp_path.empty()) {
        rmpath(tmp_path.c_str());
    }

    delete job;

    _exit(exit_code);
}
