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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifdef __FreeBSD__
// Grmbl  Why is this needed?  We don't use readv/writev
#include <sys/uio.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <map>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>

#include <comm.h>
#include "client.h"
#include "tempfile.h"
#include "md5.h"
#include "util.h"
#include "services/util.h"
#include "pipes.h"

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

namespace
{

struct CharBufferDeleter {
    char *buf;
    explicit CharBufferDeleter(char *b) : buf(b) {}
    ~CharBufferDeleter() {
        free(buf);
    }
};

}

using namespace std;

std::string remote_daemon;

Environments
parse_icecc_version(const string &target_platform, const string &prefix)
{
    Environments envs;

    string icecc_version = getenv("ICECC_VERSION");
    assert(!icecc_version.empty());

    // free after the C++-Programming-HOWTO
    string::size_type lastPos = icecc_version.find_first_not_of(',', 0);
    string::size_type pos     = icecc_version.find(',', lastPos);
    bool def_targets = icecc_version.find('=') != string::npos;

    list<string> platforms;

    while (pos != string::npos || lastPos != string::npos) {
        string couple = icecc_version.substr(lastPos, pos - lastPos);
        string platform = target_platform;
        string version = couple;
        string::size_type colon = couple.find(':');

        if (colon != string::npos) {
            platform = couple.substr(0, colon);
            version = couple.substr(colon + 1, couple.length());
        }

        // Skip delimiters.  Note the "not_of"
        lastPos = icecc_version.find_first_not_of(',', pos);
        // Find next "non-delimiter"
        pos = icecc_version.find(',', lastPos);

        if (def_targets) {
            colon = version.find('=');

            if (colon != string::npos) {
                if (prefix != version.substr(colon + 1, version.length())) {
                    continue;
                }

                version = version.substr(0, colon);
            } else if (!prefix.empty()) {
                continue;
            }
        }

        if (find(platforms.begin(), platforms.end(), platform) != platforms.end()) {
            log_error() << "there are two environments for platform " << platform << " - ignoring " << version << endl;
            continue;
        }

        if (::access(version.c_str(), R_OK) < 0) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed " << version << endl;
            continue;
        }

        struct stat st;

        if (lstat(version.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 500) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed " << version << endl;
            continue;
        }

        envs.push_back(make_pair(platform, version));
        platforms.push_back(platform);
    }

    return envs;
}

static bool
endswith(const string &orig, const char *suff, string &ret)
{
    size_t len = strlen(suff);

    if (orig.size() > len && orig.substr(orig.size() - len) == suff) {
        ret = orig.substr(0, orig.size() - len);
        return true;
    }

    return false;
}

static Environments
rip_out_paths(const Environments &envs, map<string, string> &version_map, map<string, string> &versionfile_map)
{
    version_map.clear();

    Environments env2;

    static const char *suffs[] = { ".tar.xz", ".tar.zst", ".tar.bz2", ".tar.gz", ".tar", ".tgz", nullptr };

    string versfile;

    // host platform + filename
    for (const std::pair<std::string, std::string> &env : envs) {
        for (int i = 0; suffs[i] != nullptr; i++)
            if (endswith(env.second, suffs[i], versfile)) {
                versionfile_map[env.first] = env.second;
                versfile = find_basename(versfile);
                version_map[env.first] = versfile;
                env2.push_back(make_pair(env.first, versfile));
            }
    }

    return env2;
}


string
get_absfilename(const string &_file)
{
    string file;

    if (_file.empty()) {
        return _file;
    }

    if (_file.at(0) != '/') {
        file = get_cwd() + '/' + _file;
    } else {
        file = _file;
    }

    string dots = "/../";
    string::size_type idx = file.find(dots);

    while (idx != string::npos) {
        if (idx == 0) {
            file.replace(0, dots.length(), "/");
        } else {
          string::size_type slash = file.rfind('/', idx - 1);
          file.replace(slash, idx-slash+dots.length(), "/");
        }
        idx = file.find(dots);
    }

    idx = file.find("/./");

    while (idx != string::npos) {
        file.replace(idx, 3, "/");
        idx = file.find("/./");
    }

    idx = file.find("//");

    while (idx != string::npos) {
        file.replace(idx, 2, "/");
        idx = file.find("//");
    }

    return file;
}

static int get_niceness()
{
    errno = 0;
    int niceness = getpriority( PRIO_PROCESS, getpid());
    if( niceness == -1 && errno != 0 )
        niceness = 0;
    return niceness;
}

static UseCSMsg *get_server(MsgChannel *local_daemon)
{
    int timeout = 4 * 60;
    if( get_niceness() > 0 ) // low priority jobs may take longer to get a slot assigned
        timeout = 60 * 60;
    Msg *umsg = local_daemon->get_msg( timeout );

    if (!umsg || *umsg != Msg::USE_CS) {
        log_warning() << "reply was not expected use_cs " << (umsg ? umsg->to_string() : Msg(Msg::UNKNOWN).to_string())  << endl;
        ostringstream unexpected_msg;
        unexpected_msg << "Error 1 - expected use_cs reply, but got " << (umsg ? umsg->to_string() : Msg(Msg::UNKNOWN).to_string()) << " instead";
        delete umsg;
        throw client_error(1, unexpected_msg.str());
    }

    UseCSMsg *usecs = dynamic_cast<UseCSMsg *>(umsg);
    return usecs;
}

static void check_for_failure(Msg *msg, MsgChannel *cserver)
{
    if (msg && *msg == Msg::STATUS_TEXT) {
        log_error() << "Remote status (compiled on " << cserver->name << "): "
                    << static_cast<StatusTextMsg*>(msg)->text << endl;
        throw client_error(23, "Error 23 - Remote status (compiled on " + cserver->name + ")\n" +
                                 static_cast<StatusTextMsg*>(msg)->text );
    }
}

// 'unlock_sending' = dcc_lock_host() is held when this is called, temporarily yield the lock
// while doing network transfers
static void write_fd_to_server(int fd, MsgChannel *cserver)
{
    unsigned char buffer[100000]; // some random but huge number
    off_t offset = 0;
    size_t uncompressed = 0;
    size_t compressed = 0;

    do {
        ssize_t bytes;

        do {
            bytes = read(fd, buffer + offset, sizeof(buffer) - offset);

            if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }

            if (bytes < 0) {
                log_perror("write_fd_to_server() reading from fd");
                close(fd);
                throw client_error(16, "Error 16 - error reading local file");
            }

            break;
        } while (1);

        offset += bytes;

        if (!bytes || offset == sizeof(buffer)) {
            if (offset) {
                FileChunkMsg fcmsg(buffer, offset);

                if (!cserver->send_msg(fcmsg)) {
                    Msg *m = cserver->get_msg(2);
                    check_for_failure(m, cserver);

                    log_error() << "write of source chunk to host "
                                << cserver->name.c_str() << endl;
                    log_perror("failed ");
                    close(fd);
                    throw client_error(15, "Error 15 - write to host failed");
                }

                uncompressed += fcmsg.len;
                compressed += fcmsg.compressed;
                offset = 0;
            }

            if (!bytes) {
                break;
            }
        }
    } while (1);

    if (compressed)
        trace() << "sent " << compressed << " bytes (" << (compressed * 100 / uncompressed) <<
                "%)" << endl;

    if ((-1 == close(fd)) && (errno != EBADF)){
        log_perror("close failed");
    }
}

static void receive_file(const string& output_file, MsgChannel* cserver)
{
    string tmp_file = output_file + "_icetmp";
    int obj_fd = open(tmp_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_LARGEFILE, 0666);

    if (obj_fd == -1) {
        std::string errmsg("can't create ");
        errmsg += tmp_file + ":";
        log_perror(errmsg.c_str());
        throw client_error(31, "Error 31 - " + errmsg);
    }

    Msg* msg = nullptr;
    size_t uncompressed = 0;
    size_t compressed = 0;

    while (1) {
        delete msg;

        msg = cserver->get_msg(40);

        if (!msg) {   // the network went down?
            unlink(tmp_file.c_str());
            throw client_error(19, "Error 19 - (network failure?)");
        }

        check_for_failure(msg, cserver);

        if (*msg == Msg::END) {
            break;
        }

        if (*msg != Msg::FILE_CHUNK) {
            unlink(tmp_file.c_str());
            delete msg;
            throw client_error(20, "Error 20 - unexpected message");
        }

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>(msg);
        compressed += fcmsg->compressed;
        uncompressed += fcmsg->len;

        if (write(obj_fd, fcmsg->buffer, fcmsg->len) != (ssize_t)fcmsg->len) {
            log_perror("Error writing file: ");
            unlink(tmp_file.c_str());
            delete msg;
            throw client_error(21, "Error 21 - error writing file");
        }
    }

    if (uncompressed)
        trace() << "got " << compressed << " bytes ("
                << (compressed * 100 / uncompressed) << "%)" << endl;

    delete msg;

    if (close(obj_fd) != 0) {
        log_perror("Failed to close temporary file: ");
        if(unlink(tmp_file.c_str()) != 0)
        {
            log_perror("delete temporary file - might be related to close failure above");
        }
        throw client_error(30, "Error 30 - error closing temp file");

    }
    if(rename(tmp_file.c_str(), output_file.c_str()) != 0) {
        log_perror("Failed to rename temporary file: ");
        if(unlink(tmp_file.c_str()) != 0)
        {
            log_perror("delete temporary file - might be related to rename failure above");
        }
        throw client_error(30, "Error 30 - error closing temp file");
    }
}

static int build_remote_int(CompileJob &job, UseCSMsg *usecs, MsgChannel *local_daemon,
                            const string &environment, const string &version_file,
                            const char *preproc_file, bool output)
{
    string hostname = usecs->hostname;
    unsigned int port = usecs->port;
    int job_id = usecs->job_id;
    bool got_env = usecs->got_env;
    job.setJobID(job_id);
    job.setEnvironmentVersion(environment);   // hoping on the scheduler's wisdom
    trace() << "Have to use host " << hostname << ":" << port << " - Job ID: "
            << job.jobID() << " - env: " << usecs->host_platform
            << " - has env: " << (got_env ? "true" : "false")
            << " - match j: " << usecs->matched_job_id
            << "\n";

    int status = 255;

    MsgChannel *cserver = nullptr;

    try {
        cserver = Service::createChannel(hostname, port, 10);

        if (!cserver) {
            log_error() << "no server found behind given hostname " << hostname << ":"
                        << port << endl;
            throw client_error(2, "Error 2 - no server found at " + hostname);
        }

        if (!got_env) {
            log_block b("Transfer Environment");
            // transfer env
            struct stat buf;

            if (stat(version_file.c_str(), &buf)) {
                log_perror("error stat'ing file") << "\t" << version_file << endl;
                throw client_error(4, "Error 4 - unable to stat version file");
            }

            EnvTransferMsg msg(job.targetPlatform(), job.environmentVersion());

            if (!dcc_lock_host()) {
                log_error() << "can't lock for local cpp" << endl;
                return EXIT_DISTCC_FAILED;
            }

            if (!cserver->send_msg(msg)) {
                throw client_error(6, "Error 6 - send environment to remote failed");
            }

            int env_fd = open(version_file.c_str(), O_RDONLY);

            if (env_fd < 0) {
                throw client_error(5, "Error 5 - unable to open version file:\n\t" + version_file);
            }

            write_fd_to_server(env_fd, cserver);

            if (!cserver->send_msg(EndMsg())) {
                log_error() << "write of environment failed" << endl;
                throw client_error(8, "Error 8 - write environment to remote failed");
            }

            dcc_unlock();

            if (IS_PROTOCOL_VERSION(31, cserver)) {
                VerifyEnvMsg verifymsg(job.targetPlatform(), job.environmentVersion());

                if (!cserver->send_msg(verifymsg)) {
                    throw client_error(22, "Error 22 - error sending environment");
                }

                Msg *verify_msg = cserver->get_msg(60);

                if (verify_msg && *verify_msg == Msg::VERIFY_ENV_RESULT) {
                    if (!static_cast<VerifyEnvResultMsg*>(verify_msg)->ok) {
                        // The remote can't handle the environment at all (e.g. kernel too old),
                        // mark it as never to be used again for this environment.
                        log_warning() << "Host " << hostname
                                      << " did not successfully verify environment."
                                      << endl;
                        BlacklistHostEnvMsg blacklist(job.targetPlatform(),
                                                      job.environmentVersion(), hostname);
                        local_daemon->send_msg(blacklist);
                        delete verify_msg;
                        throw client_error(24, "Error 24 - remote " + hostname + " unable to handle environment");
                    } else
                        trace() << "Verified host " << hostname << " for environment "
                                << job.environmentVersion() << " (" << job.targetPlatform() << ")"
                                << endl;
                    delete verify_msg;
                } else {
                    delete verify_msg;
                    throw client_error(25, "Error 25 - other error verifying environment on remote");
                }
            }
        }

        if (!IS_PROTOCOL_VERSION(31, cserver) && ignore_unverified()) {
            log_warning() << "Host " << hostname << " cannot be verified." << endl;
            throw client_error(26, "Error 26 - environment on " + hostname + " cannot be verified");
        }

        // Older remotes don't set properly -x argument.
        if(( job.language() == CompileJob::Lang_OBJC || job.language() == CompileJob::Lang_OBJCXX )
            && !IS_PROTOCOL_VERSION(38, cserver)) {
            job.appendFlag( "-x", Arg_Remote );
            job.appendFlag( job.language() == CompileJob::Lang_OBJC ? "objective-c" : "objective-c++", Arg_Remote );
        }

        if (!dcc_lock_host()) {
            log_error() << "can't lock for local cpp" << endl;
            return EXIT_DISTCC_FAILED;
        }

        CompileFileMsg compile_file(&job);
        {
            log_block b("send compile_file");

            if (!cserver->send_msg(compile_file)) {
                log_warning() << "write of job failed" << endl;
                throw client_error(9, "Error 9 - error sending file to remote");
            }
        }

        if (!preproc_file) {
            int sockets[2];

            if (create_large_pipe(sockets) != 0) {
                log_perror("build_remote_in pipe");
                /* for all possible cases, this is something severe */
                throw client_error(32, "Error 18 - (fork error?)");
            }

            HostUnlock hostUnlock; // automatic dcc_unlock()

            /* This will fork, and return the pid of the child.  It will not
               return for the child itself.  If it returns normally it will have
               closed the write fd, i.e. sockets[1].  */
            pid_t cpp_pid = call_cpp(job, sockets[1], sockets[0]);

            if (cpp_pid == -1) {
                throw client_error(18, "Error 18 - (fork error?)");
            }

            try {
                log_block bl2("write_fd_to_server from cpp");
                write_fd_to_server(sockets[0], cserver);
            } catch (...) {
                kill(cpp_pid, SIGTERM);
                throw;
            }

            log_block wait_cpp("wait for cpp");

            while (waitpid(cpp_pid, &status, 0) < 0 && errno == EINTR) {}

            if (shell_exit_status(status) != 0) {   // failure
                delete cserver;
                cserver = nullptr;
                log_warning() << "call_cpp process failed with exit status " << shell_exit_status(status) << endl;
                // GCC's -fdirectives-only has a number of cases that it doesn't handle properly,
                // so if in such mode preparing the source fails, try again recompiling locally.
                // This will cause double error in case it is a real error, but it'll build successfully if
                // it was just -fdirectives-only being broken. In other cases fail directly, Clang's
                // -frewrite-includes is much more reliable than -fdirectives-only, so is GCC's plain -E.
                if( !compiler_is_clang(job) && compiler_only_rewrite_includes(job))
                    throw remote_error(103, "Error 103 - local cpp invocation failed, trying to recompile locally");
                else
                    return shell_exit_status(status);
            }
        } else {
            int cpp_fd = open(preproc_file, O_RDONLY);

            if (cpp_fd < 0) {
                throw client_error(11, "Error 11 - unable to open preprocessed file");
            }

            log_block cpp_block("write_fd_to_server preprocessed");
            write_fd_to_server(cpp_fd, cserver);
        }

        if (!cserver->send_msg(EndMsg())) {
            log_warning() << "write of end failed" << endl;
            throw client_error(12, "Error 12 - failed to send file to remote");
        }

        dcc_unlock();

        Msg *msg;
        {
            log_block wait_cs("wait for cs");
            msg = cserver->get_msg(12 * 60);

            if (!msg) {
                throw client_error(14, "Error 14 - error reading message from remote");
            }
        }

        check_for_failure(msg, cserver);

        if (*msg != Msg::COMPILE_RESULT) {
            log_warning() << "waited for compile result, but got " << msg->to_string() << endl;
            delete msg;
            throw client_error(13, "Error 13 - did not get compile response message");
        }

        CompileResultMsg *crmsg = dynamic_cast<CompileResultMsg*>(msg);
        assert(crmsg);

        status = crmsg->status;

        if (status && crmsg->was_out_of_memory) {
            delete crmsg;
            log_warning() << "the server ran out of memory, recompiling locally" << endl;
            throw remote_error(101, "Error 101 - the server ran out of memory, recompiling locally");
        }

        if (output) {
            if ((!crmsg->out.empty() || !crmsg->err.empty()) && output_needs_workaround(job)) {
                delete crmsg;
                log_warning() << "command needs stdout/stderr workaround, recompiling locally" << endl;
                log_warning() << "(set ICECC_CARET_WORKAROUND=0 to override)" << endl;
                throw remote_error(102, "Error 102 - command needs stdout/stderr workaround, recompiling locally");
            }

            if (crmsg->err.find("file not found") != string::npos) {
                delete crmsg;
                log_warning() << "remote is missing file, recompiling locally" << endl;
                throw remote_error(104, "Error 104 - remote is missing file, recompiling locally");
            }

            ignore_result(write(STDOUT_FILENO, crmsg->out.c_str(), crmsg->out.size()));

            if (colorify_wanted(job)) {
                colorify_output(crmsg->err);
            } else {
                ignore_result(write(STDERR_FILENO, crmsg->err.c_str(), crmsg->err.size()));
            }

            if (status && (crmsg->err.length() || crmsg->out.length())) {
                log_info() << "Compiled on " << hostname << endl;
            }
        }

        bool have_dwo_file = crmsg->have_dwo_file;
        delete crmsg;

        assert(!job.outputFile().empty());

        if (status == 0) {
            receive_file(job.outputFile(), cserver);
            if (have_dwo_file) {
                string dwo_output = job.outputFile().substr(0, job.outputFile().rfind('.')) + ".dwo";
                receive_file(dwo_output, cserver);
            }
        }

    } catch (...) {
        // Handle pending status messages, if any.
        if(cserver) {
            while(Msg* msg = cserver->get_msg(0, true)) {
                if(*msg == Msg::STATUS_TEXT)
                    log_error() << "Remote status (compiled on " << cserver->name << "): "
                                << static_cast<StatusTextMsg*>(msg)->text << endl;
                delete msg;
            }
            delete cserver;
            cserver = nullptr;
        }

        throw;
    }

    delete cserver;
    return status;
}

static string
md5_for_file(const string & file)
{
    md5_state_t state;
    string result;

    md5_init(&state);
    FILE *f = fopen(file.c_str(), "rb");

    if (!f) {
        return result;
    }

    md5_byte_t buffer[40000];

    while (true) {
        size_t size = fread(buffer, 1, 40000, f);

        if (!size) {
            break;
        }

        md5_append(&state, buffer, size);
    }

    fclose(f);

    md5_byte_t digest[16];
    md5_finish(&state, digest);

    char digest_cache[33];

    for (int di = 0; di < 16; ++di) {
        sprintf(digest_cache + di * 2, "%02x", digest[di]);
    }

    digest_cache[32] = 0;
    result = digest_cache;
    return result;
}

static bool
maybe_build_local(MsgChannel *local_daemon, UseCSMsg *usecs, CompileJob &job,
                  int &ret)
{
    remote_daemon = usecs->hostname;

    if (usecs->hostname == "127.0.0.1") {
        // If this is a test build, do local builds on the local daemon
        // that has --no-remote, use remote building for the remaining ones.
        if (getenv("ICECC_TEST_REMOTEBUILD") && usecs->port != 0 )
            return false;
        trace() << "building myself, but telling localhost\n";
        int job_id = usecs->job_id;
        job.setJobID(job_id);
        job.setEnvironmentVersion("__client");
        CompileFileMsg compile_file(&job);

        if (!local_daemon->send_msg(compile_file)) {
            log_warning() << "write of job failed" << endl;
            throw client_error(29, "Error 29 - write of job failed");
        }

        struct timeval begintv,  endtv;

        struct rusage ru;

        gettimeofday(&begintv, nullptr);

        ret = build_local(job, local_daemon, &ru);

        gettimeofday(&endtv, nullptr);

        // filling the stats, so the daemon can play proxy for us
        JobDoneMsg msg(job_id, ret, JobDoneMsg::FROM_SUBMITTER);

        msg.real_msec = (endtv.tv_sec - begintv.tv_sec) * 1000 + (endtv.tv_usec - begintv.tv_usec) / 1000;

        struct stat st;

        msg.out_uncompressed = 0;
        if (!stat(job.outputFile().c_str(), &st)) {
            msg.out_uncompressed += st.st_size;
        }
        if (!stat((job.outputFile().substr(0, job.outputFile().rfind('.')) + ".dwo").c_str(), &st)) {
            msg.out_uncompressed += st.st_size;
        }

        msg.user_msec = ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000;
        msg.sys_msec = ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
        msg.pfaults = ru.ru_majflt + ru.ru_minflt + ru.ru_nswap;
        msg.exitcode = ret;

        if (msg.user_msec > 50 && msg.out_uncompressed > 1024) {
            trace() << "speed=" << float(msg.out_uncompressed / msg.user_msec) << endl;
        }

        return local_daemon->send_msg(msg);
    }

    return false;
}

// Minimal version of remote host that we want to use for the job.
static int minimalRemoteVersion( const CompileJob& job)
{
    int version = MIN_PROTOCOL_VERSION;
    if (ignore_unverified()) {
        version = max(version, 31);
    }

    if (job.dwarfFissionEnabled()) {
        version = max(version, 35);
    }

    return version;
}

static unsigned int requiredRemoteFeatures()
{
    unsigned int features = 0;
    if (const char* icecc_env_compression = getenv( "ICECC_ENV_COMPRESSION" )) {
        if( strcmp( icecc_env_compression, "xz" ) == 0 )
            features = features | NODE_FEATURE_ENV_XZ;
        if( strcmp( icecc_env_compression, "zstd" ) == 0 )
            features = features | NODE_FEATURE_ENV_ZSTD;
    }
    return features;
}

int build_remote(CompileJob &job, MsgChannel *local_daemon, const Environments &_envs, int permill)
{
    srand(time(nullptr) + getpid());

    int torepeat = 1;
    bool has_split_dwarf = job.dwarfFissionEnabled();

    if (!compiler_is_clang(job)) {
        if (rand() % 1000 < permill) {
            torepeat = 3;
        }
    }

    if( torepeat == 1 ) {
        trace() << "preparing " << job.inputFile() << " to be compiled for "
                << job.targetPlatform() << "\n";
    } else {
        trace() << "preparing " << job.inputFile() << " to be compiled " << torepeat << " times for "
                << job.targetPlatform() << "\n";
    }

    map<string, string> versionfile_map, version_map;
    Environments envs = rip_out_paths(_envs, version_map, versionfile_map);

    if (!envs.size()) {
        log_error() << "$ICECC_VERSION needs to point to .tar files" << endl;
        throw client_error(22, "Error 22 - $ICECC_VERSION needs to point to .tar files");
    }

    const char *preferred_host = getenv("ICECC_PREFERRED_HOST");

    if (torepeat == 1) {
        string fake_filename;
        list<string> args = job.remoteFlags();

        for (list<string>::const_iterator it = args.begin(); it != args.end(); ++it) {
            fake_filename += "/" + *it;
        }

        args = job.restFlags();

        for (list<string>::const_iterator it = args.begin(); it != args.end(); ++it) {
            fake_filename += "/" + *it;
        }

        fake_filename += get_absfilename(job.inputFile());

        GetCSMsg getcs(envs, fake_filename, job.language(), torepeat,
                       job.targetPlatform(), job.argumentFlags(),
                       preferred_host ? preferred_host : string(),
                       minimalRemoteVersion(job), requiredRemoteFeatures(),
                       get_niceness());

        trace() << "asking for host to use" << endl;
        if (!local_daemon->send_msg(getcs)) {
            log_warning() << "asked for CS" << endl;
            throw client_error(24, "Error 24 - asked for CS");
        }

        UseCSMsg *usecs = get_server(local_daemon);
        int ret;

        try {
            if (!maybe_build_local(local_daemon, usecs, job, ret))
                ret = build_remote_int(job, usecs, local_daemon,
                                       version_map[usecs->host_platform],
                                       versionfile_map[usecs->host_platform],
                                       nullptr, true);
        } catch(...) {
            delete usecs;
            throw;
        }

        delete usecs;
        return ret;
    } else {
        char *preproc = nullptr;
        dcc_make_tmpnam("icecc", ".ix", &preproc, 0);
        const CharBufferDeleter preproc_holder(preproc);
        int cpp_fd = open(preproc, O_WRONLY);

        if (!dcc_lock_host()) {
            log_error() << "can't lock for local cpp" << endl;
            return EXIT_DISTCC_FAILED;
        }
        HostUnlock hostUnlock; // automatic dcc_unlock()

        /* When call_cpp returns normally (for the parent) it will have closed
           the write fd, i.e. cpp_fd.  */
        pid_t cpp_pid = call_cpp(job, cpp_fd);

        if (cpp_pid == -1) {
            ::unlink(preproc);
            throw client_error(10, "Error 10 - (unable to fork process?)");
        }

        int status = 255;
        waitpid(cpp_pid, &status, 0);

        if (shell_exit_status(status)) {   // failure
            log_warning() << "call_cpp process failed with exit status " << shell_exit_status(status) << endl;
            ::unlink(preproc);
            return shell_exit_status(status);
        }
        dcc_unlock();

        char rand_seed[400]; // "designed to be oversized" (Levi's)
        sprintf(rand_seed, "-frandom-seed=%d", rand());
        job.appendFlag(rand_seed, Arg_Remote);

        GetCSMsg getcs(envs, get_absfilename(job.inputFile()), job.language(), torepeat,
                       job.targetPlatform(), job.argumentFlags(),
                       preferred_host ? preferred_host : string(),
                       minimalRemoteVersion(job), 0, get_niceness());


        if (!local_daemon->send_msg(getcs)) {
            log_warning() << "asked for CS" << endl;
            throw client_error(0, "Error 0 - asked for CS");
        }

        map<pid_t, int> jobmap;
        CompileJob *jobs = new CompileJob[torepeat];
        UseCSMsg **umsgs = new UseCSMsg*[torepeat];

        bool misc_error = false;
        int *exit_codes = new int[torepeat];

        for (int i = 0; i < torepeat; i++) { // init
            exit_codes[i] = 42;
        }


        for (int i = 0; i < torepeat; i++) {
            jobs[i] = job;
            char *buffer = nullptr;

            if (i) {
                dcc_make_tmpnam("icecc", ".o", &buffer, 0);
                jobs[i].setOutputFile(buffer);
            } else {
                buffer = strdup(job.outputFile().c_str());
            }

            const CharBufferDeleter buffer_holder(buffer);

            umsgs[i] = get_server(local_daemon);

            remote_daemon = umsgs[i]->hostname;

            trace() << "got_server_for_job " << umsgs[i]->hostname << endl;

            flush_debug();

            pid_t pid = fork();

            if (pid == -1) {
                log_perror("failure of fork");
                status = -1;
            }

            if (!pid) {
                int ret = 42;

                try {
                    if (!maybe_build_local(local_daemon, umsgs[i], jobs[i], ret))
                        ret = build_remote_int(
                                  jobs[i], umsgs[i], local_daemon,
                                  version_map[umsgs[i]->host_platform],
                                  versionfile_map[umsgs[i]->host_platform],
                                  preproc, i == 0);
                } catch (std::exception& error) {
                    log_info() << "build_remote_int failed and has thrown " << error.what() << endl;
                    kill(getpid(), SIGTERM);
                    return 0; // shouldn't matter
                }

                _exit(ret);
                return 0; // doesn't matter
            }

            jobmap[pid] = i;
        }

        for (int i = 0; i < torepeat; i++) {
            pid_t pid = wait(&status);

            if (pid < 0) {
                log_perror("wait failed");
                status = -1;
            } else {
                if (WIFSIGNALED(status)) {
                    // there was some misc error in processing
                    misc_error = true;
                    break;
                }

                exit_codes[jobmap[pid]] = shell_exit_status(status);
            }
        }

        if (!misc_error) {
            string first_md5 = md5_for_file(jobs[0].outputFile());

            for (int i = 1; i < torepeat; i++) {
                if (!exit_codes[0]) {   // if the first failed, we fail anyway
                    if (exit_codes[i] == 42) { // they are free to fail for misc reasons
                        continue;
                    }

                    if (exit_codes[i]) {
                        log_error() << umsgs[i]->hostname << " compiled with exit code " << exit_codes[i]
                                    << " and " << umsgs[0]->hostname << " compiled with exit code "
                                    << exit_codes[0] << " - aborting!\n";
                        if (-1 == ::unlink(jobs[0].outputFile().c_str())){
                            log_perror("unlink outputFile failed") << "\t" << jobs[0].outputFile() << endl;
                        }
                        if (has_split_dwarf) {
                            string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().rfind('.')) + ".dwo";
                            if (-1 == ::unlink(dwo_file.c_str())){
                                log_perror("unlink failed") << "\t" << dwo_file << endl;
                            }
                        }
                        exit_codes[0] = -1; // overwrite
                        break;
                    }

                    string other_md5 = md5_for_file(jobs[i].outputFile());

                    if (other_md5 != first_md5) {
                        log_error() << umsgs[i]->hostname << " compiled "
                                    << jobs[0].outputFile() << " with md5 sum " << other_md5
                                    << "(" << jobs[i].outputFile() << ")" << " and "
                                    << umsgs[0]->hostname << " compiled with md5 sum "
                                    << first_md5 << " - aborting!\n";
                        rename(jobs[0].outputFile().c_str(),
                               (jobs[0].outputFile() + ".caught").c_str());
                        rename(preproc, (string(preproc) + ".caught").c_str());
                        if (has_split_dwarf) {
                            string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().rfind('.')) + ".dwo";
                            rename(dwo_file.c_str(), (dwo_file + ".caught").c_str());
                        }
                        exit_codes[0] = -1; // overwrite
                        break;
                    }
                }

                if (-1 == ::unlink(jobs[i].outputFile().c_str())){
                    log_perror("unlink failed") << "\t" << jobs[i].outputFile() << endl;
                }
                if (has_split_dwarf) {
                    string dwo_file = jobs[i].outputFile().substr(0, jobs[i].outputFile().rfind('.')) + ".dwo";
                    if (-1 == ::unlink(dwo_file.c_str())){
                        log_perror("unlink failed") << "\t" << dwo_file << endl;
                    }
                }
                delete umsgs[i];
            }
        } else {
            if (-1 == ::unlink(jobs[0].outputFile().c_str())){
                log_perror("unlink failed") << "\t" << jobs[0].outputFile() << endl;
            }
            if (has_split_dwarf) {
                string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().rfind('.')) + ".dwo";
                if (-1 == ::unlink(dwo_file.c_str())){
                    log_perror("unlink failed") << "\t" << dwo_file << endl;
                }
            }

            for (int i = 1; i < torepeat; i++) {
                if (-1 == ::unlink(jobs[i].outputFile().c_str())){
                    log_perror("unlink failed") << "\t" << jobs[i].outputFile() << endl;
                }
                if (has_split_dwarf) {
                    string dwo_file = jobs[i].outputFile().substr(0, jobs[i].outputFile().rfind('.')) + ".dwo";
                    if (-1 == ::unlink(dwo_file.c_str())){
                        log_perror("unlink failed") << "\t" << dwo_file << endl;
                    }
                }
                delete umsgs[i];
            }
        }

        delete umsgs[0];

        if (-1 == ::unlink(preproc)){
            log_perror("unlink failed") << "\t" << preproc << endl;
        }

        int ret = exit_codes[0];

        delete [] umsgs;
        delete [] jobs;
        delete [] exit_codes;

        if (misc_error) {
            throw client_error(27, "Error 27 - misc error");
        }

        return ret;
    }


    return 0;
}
