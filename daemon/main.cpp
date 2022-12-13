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

//#define ICECC_DEBUG 1
#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <getopt.h>
#include <limits>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>

#ifdef HAVE_ARPA_NAMESER_H
#  include <arpa/nameser.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <arpa/inet.h>

#ifdef HAVE_RESOLV_H
#  include <resolv.h>
#endif
#include <netdb.h>

#ifndef RUSAGE_SELF
#  define RUSAGE_SELF (0)
#endif
#ifndef RUSAGE_CHILDREN
#  define RUSAGE_CHILDREN (-1)
#endif

#ifdef HAVE_LIBCAP_NG
#  include <cap-ng.h>
#endif

#include <archive.h>

#include <deque>
#include <map>
#include <algorithm>
#include <set>
#include <fstream>
#include <string>

#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"
#include "workit.h"
#include "logging.h"
#include <comm.h>
#include "load.h"
#include "environment.h"
#include "platform.h"
#include "util.h"
#include "getifaddrs.h"

static std::string pidFilePath;
static volatile sig_atomic_t exit_main_loop = 0;

#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__
#endif

using namespace std;

struct Client {
public:
    /*
     * UNKNOWN: Client was just created - not supposed to be long term
     * GOTNATIVE: Client asked us for the native env - this is the first step
     * PENDING_USE_CS: We have a CS from scheduler and need to tell the client
     *          as soon as there is a spot available on the local machine
     * JOBDONE: This was compiled by a local client and we got a jobdone - awaiting END
     * LINKJOB: This is a local job (aka link job) by a local client we told the scheduler about
     *          and await the finish of it
     * TOINSTALL: We're receiving an environment transfer and wait for it to complete.
     * WAITINSTALL: Client is waiting for the environment transfer unpacking child to finish.
     * TOCOMPILE: We're supposed to compile it ourselves
     * WAITFORCS: Client asked for a CS and we asked the scheduler - waiting for its answer
     * WAITCOMPILE: Client got a CS and will ask him now (it's not me)
     * CLIENTWORK: Client is busy working and we reserve the spot (job_id is set if it's a scheduler job)
     * WAITFORCHILD: Client is waiting for the compile job to finish.
     * WAITCREATEENV: We're waiting for icecc-create-env to finish.
     */
    enum Status { UNKNOWN, GOTNATIVE, PENDING_USE_CS, JOBDONE, LINKJOB, TOINSTALL, WAITINSTALL, TOCOMPILE,
                  WAITFORCS, WAITCOMPILE, CLIENTWORK, WAITFORCHILD, WAITCREATEENV,
                  LASTSTATE = WAITCREATEENV
                } status;
    Client() {
        job_id = 0;
        channel = nullptr;
        job = nullptr;
        usecsmsg = nullptr;
        client_id = 0;
        niceness = 0;
        status = UNKNOWN;
        pipe_from_child = -1;
        pipe_to_child = -1;
        child_pid = -1;
        fulljob = false;
    }

    static string status_str(Status status) {
        switch (status) {
        case UNKNOWN:
            return "unknown";
        case GOTNATIVE:
            return "gotnative";
        case PENDING_USE_CS:
            return "pending_use_cs";
        case JOBDONE:
            return "jobdone";
        case LINKJOB:
            return "linkjob";
        case TOINSTALL:
            return "toinstall";
        case WAITINSTALL:
            return "waitinstall";
        case TOCOMPILE:
            return "tocompile";
        case WAITFORCS:
            return "waitforcs";
        case CLIENTWORK:
            return "clientwork";
        case WAITCOMPILE:
            return "waitcompile";
        case WAITFORCHILD:
            return "waitforchild";
        case WAITCREATEENV:
            return "waitcreateenv";
        }

        assert(false);
        return string(); // shutup gcc
    }

    ~Client() {
        status = (Status) - 1;
        delete channel;
        channel = nullptr;
        delete usecsmsg;
        usecsmsg = nullptr;
        delete job;
        job = nullptr;

        if (pipe_from_child >= 0) {
            if (-1 == close(pipe_from_child) && (errno != EBADF)){
                log_perror("Failed to close pipe from child process");
            }
        }
        if (pipe_to_child >= 0) {
            if (-1 == close(pipe_to_child) && (errno != EBADF)){
                log_perror("Failed to close pipe to child process");
            }
        }

    }
    uint32_t job_id;
    string outfile; // only useful for LINKJOB or TOINSTALL/WAITINSTALL
    MsgChannel *channel;
    UseCSMsg *usecsmsg;
    CompileJob *job;
    int client_id;
    uint32_t niceness; // nice priority (0-20), for PENDING_USE_CS
    // pipe from child process with end status, only valid if WAITFORCHILD or TOINSTALL/WAITINSTALL
    int pipe_from_child;
    // pipe to child process, only valid if TOINSTALL/WAITINSTALL
    int pipe_to_child;
    pid_t child_pid;
    bool fulljob; // during LINKJOB and CLIENTWORK, reserve all slots if set
    string pending_create_env; // only for WAITCREATEENV

    string dump() const {
        string ret = status_str(status) + " " + channel->dump();

        switch (status) {
        case LINKJOB:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + (fulljob ? " (full)" : "")
                + " PID: " + toString(child_pid);
        case TOINSTALL:
        case WAITINSTALL:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + " PID: " + toString(child_pid);
        case WAITFORCHILD:
            return ret + " ClientID: " + toString(client_id) + " PID: " + toString(child_pid) + " PFD: " + toString(pipe_from_child);
        case WAITCREATEENV:
            return ret + " " + toString(client_id) + " " + pending_create_env;
        default:
            ret += " ClientID: " + toString(client_id);
            if (job_id) {
                ret += " Job ID: " + toString(job_id);
                if (usecsmsg)
                    ret += " CompileServer: " + usecsmsg->hostname;
            }
            if (niceness != 0)
                ret += " Nice: " + toString(niceness);
            return ret;
        }
    }
};

class Clients : public map<MsgChannel*, Client*>
{
public:
    Clients() {
        active_processes = 0;
    }
    unsigned int active_processes;

    Client *find_by_client_id(int id) const {
        for (auto it : *this)
            if (it.second->client_id == id) {
                return it.second;
            }

        return nullptr;
    }

    Client *find_by_pid(pid_t pid) const {
        for (auto it : *this)
            if (it.second->child_pid == pid) {
                return it.second;
            }

        return nullptr;
    }

    Client *first() {
        iterator it = begin();

        if (it == end()) {
            return nullptr;
        }

        Client *cl = it->second;
        return cl;
    }

    string dump_status(Client::Status s) const {
        int count = 0;

        for (auto it : *this) {
            if (it.second->status == s) {
                count++;
            }
        }

        if (count) {
            return toString(count) + " " + Client::status_str(s) + ", ";
        }

        return string();
    }

    string dump_per_status() const {
        string s;

        for (Client::Status i = Client::UNKNOWN; i <= Client::LASTSTATE;
                i = Client::Status(int(i) + 1)) {
            s += dump_status(i);
        }

        return s;
    }
    Client *get_earliest_client(Client::Status s) const {
        // TODO: possibly speed this up in adding some sorted lists
        Client *client = nullptr;
        int min_client_id = 0;
        uint32_t min_niceness = std::numeric_limits<uint32_t>::max();

        for (auto it : *this) {
            if (it.second->status == s && (!min_client_id || min_client_id > it.second->client_id)
                && it.second->niceness < min_niceness ) {
                client = it.second;
                min_client_id = client->client_id;
                min_niceness = client->niceness;
            }
        }

        return client;
    }
};

static int set_new_pgrp()
{
    /* If we're a session group leader, then we are not able to call
     * setpgid().  However, setsid will implicitly have put us into a new
     * process group, so we don't have to do anything. */

    /* Does everyone have getpgrp()?  It's in POSIX.1.  We used to call
     * getpgid(0), but that is not available on BSD/OS. */
    int pgrp_id = getpgrp();

    if (-1 == pgrp_id){
        log_perror("Failed to get process group ID");
        return EXIT_DISTCC_FAILED;
    }

    if (pgrp_id == getpid()) {
        trace() << "already a process group leader\n";
        return 0;
    }

    if (setpgid(0, 0) == 0) {
        trace() << "entered process group\n";
        return 0;
    }

    trace() << "setpgid(0, 0) failed: " << strerror(errno) << endl;
    return EXIT_DISTCC_FAILED;
}

static void dcc_daemon_terminate(int);

/**
 * Catch all relevant termination signals.  Set up in parent and also
 * applies to children.
 **/
void dcc_daemon_catch_signals()
{
    /* SIGALRM is caught to allow for built-in timeouts when running test
     * cases. */

    signal(SIGTERM, &dcc_daemon_terminate);
    signal(SIGINT, &dcc_daemon_terminate);
    signal(SIGALRM, &dcc_daemon_terminate);
}

pid_t dcc_master_pid;

/**
 * Called when a daemon gets a fatal signal.
 *
 * Some cleanup is done only if we're the master/parent daemon.
 **/
static void dcc_daemon_terminate(int whichsig)
{
    /**
     * This is a signal handler. don't do stupid stuff.
     * Don't call printf. and especially don't call the log_*() functions.
     */

    if (exit_main_loop > 1) {
        // The > 1 is because we get one more signal from the kill(0,...) below.
        // hmm, we got killed already twice. try better
        static const char msg[] = "forced exit.\n";
        ignore_result(write(STDERR_FILENO, msg, strlen( msg )));
        _exit(1);
    }

    // make BSD happy
    signal(whichsig, dcc_daemon_terminate);

    bool am_parent = (getpid() == dcc_master_pid);

    if (am_parent && exit_main_loop == 0) {
        /* kill whole group */
        kill(0, whichsig);

        /* Remove pid file */
        unlink(pidFilePath.c_str());
    }

    ++exit_main_loop;
}

void usage(const char *reason = nullptr)
{
    if (reason) {
        cerr << reason << endl;
    }

    cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [--no-remote] [-d|--daemonize] [-l logfile] [-s <schedulerhost[:port]>]"
        " [-v[v[v]]] [-u|--user-uid <user_uid>] [-b <env-basedir>] [--cache-limit <MB>] [-N <node_name>] [-i|--interface <net_interface>] [-p|--port <port>]" << endl;
    exit(1);
}

struct timeval last_stat;

// Initial rlimit for a compile job, measured in megabytes.  Will vary with
// the amount of available memory.
int mem_limit = 100;

// Minimum rlimit for a compile job, measured in megabytes.
const int min_mem_limit = 100;

unsigned int max_kids = 0;

size_t cache_size_limit = 256 * 1024 * 1024;

struct NativeEnvironment {
    string name; // the hash
    // Timestamps for files including compiler binaries, if they have changed since the time
    // the native env was built, it needs to be rebuilt.
    map<string, time_t> filetimes;
    time_t last_use;
    size_t size; // tarball size
    int create_env_pipe; // if in progress of creating the environment
    NativeEnvironment() : last_use( 0 ), size( 0 ), create_env_pipe( 0 ) {}
};

struct ReceivedEnvironment {
    ReceivedEnvironment() : last_use( 0 ), size( 0 ) {}
    time_t last_use;
    size_t size; // directory size
};

struct Daemon {
    Clients clients;
    // Installed environments received from other nodes. The key is
    // (job->targetPlatform() + "/" job->environmentVersion()).
    map<string, ReceivedEnvironment> received_environments;
    // Map of native environments, the basic one(s) containing just the compiler
    // and possibly more containing additional files (such as compiler plugins).
    // The key is the compiler name and a concatenated list of the additional files
    // (or just the compiler name for the basic ones).
    map<string, NativeEnvironment> native_environments;
    string envbasedir;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno;
    int tcp_listen_fd;
    int tcp_listen_local_fd; // if tcp_listen is bound to a specific network interface, this one is bound to lo interface
    int unix_listen_fd;
    string machine_name;
    string nodename;
    bool noremote;
    bool custom_nodename;
    size_t cache_size;
    map<int, Client*> fd2client;
    int new_client_id;
    string remote_name;
    time_t next_scheduler_connect;
    unsigned long icecream_load;
    struct timeval icecream_usage;
    int current_load;
    int num_cpus;
    MsgChannel *scheduler;
    DiscoverSched *discover;
    string netname;
    string schedname;
    int scheduler_port;
    string daemon_interface;
    int daemon_port;
    unsigned int supported_features;

    int max_scheduler_pong;
    int max_scheduler_ping;
    unsigned int current_kids;

    Daemon() {
        warn_icecc_user_errno = 0;
        if (getuid() == 0) {
            struct passwd *pw = getpwnam("icecc");

            if (pw) {
                user_uid = pw->pw_uid;
                user_gid = pw->pw_gid;
            } else {
                warn_icecc_user_errno = errno ? errno : ENOENT; // apparently errno can be 0 on error here
                user_uid = 65534;
                user_gid = 65533;
            }
        } else {
            user_uid = getuid();
            user_gid = getgid();
        }

        envbasedir = "/var/tmp/icecc-envs";
        tcp_listen_fd = -1;
        tcp_listen_local_fd = -1;
        unix_listen_fd = -1;
        new_client_id = 0;
        next_scheduler_connect = 0;
        cache_size = 0;
        noremote = false;
        custom_nodename = false;
        icecream_load = 0;
        icecream_usage.tv_sec = icecream_usage.tv_usec = 0;
        current_load = - 1000;
        num_cpus = 0;
        scheduler = nullptr;
        discover = nullptr;
        scheduler_port = 8765;
        daemon_interface = "";
        daemon_port = 10245;
        max_scheduler_pong = MAX_SCHEDULER_PONG;
        max_scheduler_ping = MAX_SCHEDULER_PING;
        current_kids = 0;
    }

    ~Daemon() {
        delete discover;
    }

    bool reannounce_environments() __attribute_warn_unused_result__;
    void answer_client_requests();
    bool handle_transfer_env(Client *client, EnvTransferMsg *msg) __attribute_warn_unused_result__;
    bool handle_env_install_child_done(Client *client);
    bool finish_transfer_env(Client *client, bool cancel = false);
    bool handle_get_native_env(Client *client, GetNativeEnvMsg *msg) __attribute_warn_unused_result__;
    bool finish_get_native_env(Client *client, string env_key);
    void handle_old_request();
    bool handle_compile_file(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_activity(Client *client) __attribute_warn_unused_result__;
    bool handle_file_chunk_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    void handle_end(Client *client, int exitcode);
    int scheduler_get_internals() __attribute_warn_unused_result__;
    void clear_children();
    int scheduler_use_cs(UseCSMsg *msg) __attribute_warn_unused_result__;
    int scheduler_no_cs(NoCSMsg *msg) __attribute_warn_unused_result__;
    bool handle_get_cs(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_local_job(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_job_done(Client *cl, JobDoneMsg *m) __attribute_warn_unused_result__;
    bool handle_compile_done(Client *client) __attribute_warn_unused_result__;
    bool handle_verify_env(Client *client, VerifyEnvMsg *msg) __attribute_warn_unused_result__;
    bool handle_blacklist_host_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    int handle_cs_conf(ConfCSMsg *msg);
    string dump_internals() const;
    string determine_nodename();
    void determine_system();
    void determine_supported_features();
    bool maybe_stats(bool force_check = false);
    bool send_scheduler(const Msg &msg) __attribute_warn_unused_result__;
    void close_scheduler();
    bool reconnect();
    int working_loop();
    bool setup_listen_fds();
    bool setup_listen_tcp_fd( int& fd, const string& interface );
    bool setup_listen_unix_fd();
    void check_cache_size(const string &new_env);
    void remove_native_environment(const string& env_key);
    void remove_environment(const string& env_key);
    bool create_env_finished(string env_key);
};

bool Daemon::setup_listen_fds()
{
    tcp_listen_fd = -1;
    tcp_listen_local_fd = -1;
    unix_listen_fd = -1;

    if (!noremote) { // if we only listen to local clients, there is no point in going TCP
        if( !setup_listen_tcp_fd( tcp_listen_fd, daemon_interface ))
            return false;
        // We should always listen on the loopback interface, so if we're binding only
        // to a specific interface, bind also to the loopback.
        if( !daemon_interface.empty()) {
            if( !setup_listen_tcp_fd( tcp_listen_local_fd, "lo" ))
                return false;
        }
    }
    if( !setup_listen_unix_fd())
        return false;
    return true;
}

bool Daemon::setup_listen_tcp_fd( int& fd, const string& interface )
{
    if( !interface.empty())
        trace() << "starting to listen on interface " << interface << endl;
    else
        trace() << "starting to listen on all interfaces" << endl;

    if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("Failed to create TCP listen socket.");
        return false;
    }

    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror("Failed to set 'Reuse Address(SO_REUSEADDR)' option on TCP Listen Socket");
        return false;
    }

    struct sockaddr_in myaddr;
    if (!build_address_for_interface(myaddr, interface, daemon_port)) {
        return false;
    }

    int count = 5;
    while (count) {
        if (::bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
            log_perror("Failed to bind address to TCP listen socket");
            sleep(2);
            if (!--count) {
                return false;
            }
            continue;
        } else {
            break;
        }
    }

    if (listen(fd, 1024) < 0) {
        log_perror("Failed to set TCP socket for listening to incoming connections");
        return false;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return true;
}

bool Daemon::setup_listen_unix_fd()
{
    if ((unix_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        log_perror("Failed to create a Unix scoket for listening");
        return false;
    }

    struct sockaddr_un myaddr;

    memset(&myaddr, 0, sizeof(myaddr));

    myaddr.sun_family = AF_UNIX;

    bool reset_umask = false;
    mode_t old_umask = 0;

    if (getenv("ICECC_TEST_SOCKET") == nullptr) {
#ifdef HAVE_LIBCAP_NG
        // We run as system daemon (UID has been already changed).
        if (capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        if (getuid() == 0) {
#endif
            string default_socket = "/var/run/icecc/iceccd.socket";
            strncpy(myaddr.sun_path, default_socket.c_str() , sizeof(myaddr.sun_path) - 1);
            myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
            if(default_socket.length() > sizeof(myaddr.sun_path) - 1) {
                log_error() << "default socket path too long for sun_path" << endl;
            }
            if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
                log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
            }
            old_umask = umask(0);
            reset_umask = true;
        } else { // Started by user.
            if( getenv( "HOME" )) {
                string socket_path = getenv("HOME");
                socket_path.append("/.iceccd.socket");
                strncpy(myaddr.sun_path, socket_path.c_str(), sizeof(myaddr.sun_path) - 1);
                myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
                if(socket_path.length() > sizeof(myaddr.sun_path) - 1) {
                    log_error() << "$HOME/.iceccd.socket path too long for sun_path" << endl;
                }
                if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
                    log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
                }
            } else {
                log_error() << "launched by user, but $HOME not set" << endl;
                return false;
            }
        }
    } else {
        string test_socket = getenv("ICECC_TEST_SOCKET");
        strncpy(myaddr.sun_path, test_socket.c_str(), sizeof(myaddr.sun_path) - 1);
        myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
        if(test_socket.length() > sizeof(myaddr.sun_path) - 1) {
            log_error() << "$ICECC_TEST_SOCKET path too long for sun_path" << endl;
        }
        if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
            log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
        }
        old_umask = umask(0);
        reset_umask = true;
    }

    if (::bind(unix_listen_fd, (struct sockaddr*)&myaddr, sizeof(myaddr)) < 0) {
        log_perror("Failed to bind address to unix listen socket");

        if (reset_umask) {
            umask(old_umask);
        }

        return false;
    }

    if (reset_umask) {
        umask(old_umask);
    }

    if (listen(unix_listen_fd, 1024) < 0) {
        log_perror("Failed to set unix socket for listening");
        return false;
    }

    fcntl(unix_listen_fd, F_SETFD, FD_CLOEXEC);

    return true;
}

void Daemon::determine_system()
{
    struct utsname uname_buf;

    if (uname(&uname_buf)) {
        log_perror("uname call failed. Unable to determine system node name and platform");
        return;
    }

    if (nodename.length() && (nodename != uname_buf.nodename)) {
        custom_nodename  = true;
    }

    if (!custom_nodename) {
        nodename = uname_buf.nodename;
    }

    machine_name = determine_platform();
}

string Daemon::determine_nodename()
{
    if (custom_nodename && !nodename.empty()) {
        return nodename;
    }

    // perhaps our host name changed due to network change?
    struct utsname uname_buf;

    if (!uname(&uname_buf)) {
        nodename = uname_buf.nodename;
    }

    return nodename;
}

void Daemon::determine_supported_features()
{
    supported_features = 0;
    struct archive* a = archive_read_new();
    static bool test_disable = false;
    // Make one of the two remotes in tests say it doesn't support xz/zstd tarballs.
    if( getenv( "ICECC_TESTS" ) != nullptr && nodename == "remoteice2" )
        test_disable = true;
    (void)test_disable;
#ifdef HAVE_LIBARCHIVE_XZ
    if( !test_disable && archive_read_support_filter_xz(a) >= ARCHIVE_WARN ) // includes ARCHIVE_OK
        supported_features = supported_features | NODE_FEATURE_ENV_XZ;
#endif
#ifdef HAVE_LIBARCHIVE_ZSTD
    if( !test_disable && archive_read_support_filter_zstd(a) >= ARCHIVE_WARN ) // includes ARCHIVE_OK
        supported_features = supported_features | NODE_FEATURE_ENV_ZSTD;
#endif
    // sanity checks
    if( archive_read_support_filter_gzip(a) < ARCHIVE_WARN ) // error
        log_error() << "No support for uncompressing gzip available." << endl;
    if( archive_read_support_format_tar(a) < ARCHIVE_WARN ) // error
        log_error() << "No support for unpacking tar available." << endl;
    archive_read_free(a);
}

bool Daemon::send_scheduler(const Msg& msg)
{
    if (!scheduler) {
        log_warning() << "no scheduler" << endl;
        return false;
    }

    if (!scheduler->send_msg(msg)) {
        log_error() << "sending message to scheduler failed.." << endl;
        close_scheduler();
        return false;
    }

    return true;
}

bool Daemon::reannounce_environments()
{
    log_info() << "reannounce_environments " << endl;
    LoginMsg lmsg(0, nodename, "", supported_features);
    lmsg.envs = available_environments(envbasedir);
    return send_scheduler(lmsg);
}

void Daemon::close_scheduler()
{
    if (!scheduler) {
        return;
    }

    delete scheduler;
    scheduler = nullptr;
    delete discover;
    discover = nullptr;
    next_scheduler_connect = time(nullptr) + 20 + (rand() & 31);
    static bool fast_reconnect = getenv( "ICECC_TESTS" ) != nullptr;
    if( fast_reconnect )
        next_scheduler_connect = time(nullptr) + 3;
}

bool Daemon::maybe_stats(bool force_check)
{
    struct timeval now;
    gettimeofday(&now, nullptr);

    time_t diff_sent = (now.tv_sec - last_stat.tv_sec) * 1000 + (now.tv_usec - last_stat.tv_usec) / 1000;

    if (diff_sent >= max_scheduler_pong * 1000 || force_check) {
        StatsMsg msg;
        unsigned int memory_fillgrade;
        unsigned long idleLoad = 0;
        unsigned long niceLoad = 0;

        fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);

        time_t diff_stat = (now.tv_sec - last_stat.tv_sec) * 1000 + (now.tv_usec - last_stat.tv_usec) / 1000;
        last_stat = now;

        /* icecream_load contains time in milliseconds we have used for icecream */
        /* idle time could have been used for icecream, so claim it */
        icecream_load += idleLoad * diff_stat / 1000;

        /* add the time of our childrens, but only the time since the last run */
        struct rusage ru;

        if (!getrusage(RUSAGE_CHILDREN, &ru)) {
            uint32_t ice_msec = ((ru.ru_utime.tv_sec - icecream_usage.tv_sec) * 1000
                                 + (ru.ru_utime.tv_usec - icecream_usage.tv_usec) / 1000) / num_cpus;

            /* heuristics when no child terminated yet: account 25% of total nice as our clients */
            if (!ice_msec && current_kids) {
                ice_msec = (niceLoad * diff_stat) / (4 * 1000);
            }

            icecream_load += ice_msec * diff_stat / 1000;

            icecream_usage.tv_sec = ru.ru_utime.tv_sec;
            icecream_usage.tv_usec = ru.ru_utime.tv_usec;
        }

        unsigned int idle_average = icecream_load;

        if (diff_sent) {
            idle_average = icecream_load * 1000 / diff_sent;
        }

        if (idle_average > 1000)
           idle_average = 1000;

        msg.load = std::max((1000 - idle_average), memory_fillgrade);

#ifdef HAVE_SYS_VFS_H
        struct statfs buf;
        int ret = statfs(envbasedir.c_str(), &buf);

        // Require at least 25MiB of free disk space per build.
        if (!ret && long(buf.f_bavail) < ((long(max_kids + 1 - current_kids) * 25 * 1024 * 1024) / buf.f_bsize)) {
            msg.load = 1000;
        }

#endif

        mem_limit = std::max(int(msg.freeMem / std::min(std::max(max_kids, 1U), 4U)), min_mem_limit);

        if (abs(int(msg.load) - current_load) >= 100
            || (msg.load == 1000 && current_load != 1000)
            || (msg.load != 1000 && current_load == 1000)) {
            if (!send_scheduler(msg)) {
                return false;
            }
        }

        icecream_load = 0;
        current_load = msg.load;
    }

    return true;
}

string Daemon::dump_internals() const
{
    string result;

    result += "Node Name: " + nodename + "\n";
    result += "  Remote name: " + remote_name + "\n";

    for (const auto& it : fd2client)  {
        result += "  fd2client[" + toString(it.first) + "] = " + it.second->dump() + "\n";
    }

    for (const auto& client : clients)  {
        result += "  client " + toString(client.second->client_id) + ": " + client.second->dump() + "\n";
    }

    if (cache_size) {
        result += "  Cache Size: " + toString(cache_size) + "\n";
    }

    result += "  Architecture: " + machine_name + "\n";

    for (const auto & native_environment : native_environments) {
        result += "  NativeEnv (" + native_environment.first + "): " + native_environment.second.name
            + ", size " + toString(native_environment.second.size) + (native_environment.second.create_env_pipe ? " (creating)" : "" ) + "\n";
    }

    if (!received_environments.empty()) {
        result += "  Now: " + toString(time(nullptr)) + "\n";
        for (const auto& it : received_environments )
            result += "  ReceivedEnv[" + it.first  + "] last_use " + toString(it.second.last_use)
                + ", size " + toString(it.second.size) + "\n";
    }

    result += "  Current kids: " + toString(current_kids) + " (max: " + toString(max_kids) + ")\n";

    result += "  Supported features: " + supported_features_to_string(supported_features) + "\n";

    if (scheduler) {
        result += "  Scheduler protocol: " + toString(scheduler->protocol) + "\n";
    }

    StatsMsg msg;
    unsigned int memory_fillgrade = 0;
    unsigned long idleLoad = 0;
    unsigned long niceLoad = 0;

    fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);
    result += "  cpu: " + toString(idleLoad) + " idle, "
              + toString(niceLoad) + " nice\n";
    result += "  load: " + toString(msg.loadAvg1 / 1000.) + ", icecream_load: "
              + toString(icecream_load) + "\n";
    result += "  memory: " + toString(memory_fillgrade)
              + " (free: " + toString(msg.freeMem) + ")\n";

    return result;
}

int Daemon::scheduler_get_internals()
{
    trace() << "handle_get_internals " << dump_internals() << endl;
    return send_scheduler(StatusTextMsg(dump_internals())) ? 0 : 1;
}

int Daemon::scheduler_use_cs(UseCSMsg *msg)
{
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_use_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " << msg->hostname << " " << remote_name <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size()))) {
            return 0;
        }

        return 1;
    }

    if (msg->hostname == remote_name && int(msg->port) == daemon_port) {
        c->usecsmsg = new UseCSMsg(msg->host_platform, "127.0.0.1", daemon_port, msg->job_id, true, 1,
                                   msg->matched_job_id);
        c->status = Client::PENDING_USE_CS;
    } else {
        c->usecsmsg = new UseCSMsg(msg->host_platform, msg->hostname, msg->port,
                                   msg->job_id, true, 1, msg->matched_job_id);

        if (!c->channel->send_msg(*msg)) {
            handle_end(c, 143);
            return 0;
        }

        c->status = Client::WAITCOMPILE;
    }

    c->job_id = msg->job_id;

    return 0;
}

int Daemon::scheduler_no_cs(NoCSMsg *msg)
{
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_no_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size()))) {
            return 0;
        }

        return 1;
    }

    c->usecsmsg = new UseCSMsg(string(), "127.0.0.1", daemon_port, msg->job_id, true, 1, 0);
    c->status = Client::PENDING_USE_CS;

    c->job_id = msg->job_id;

    return 0;

}

bool Daemon::handle_transfer_env(Client *client, EnvTransferMsg *emsg)
{
    log_info() << "handle_transfer_env, client status " << Client::status_str(client->status) <<  endl;

    assert(client->status != Client::TOINSTALL &&
           client->status != Client::WAITINSTALL &&
           client->status != Client::TOCOMPILE &&
           client->status != Client::WAITCOMPILE);
    assert(client->pipe_from_child < 0);
    assert(client->pipe_to_child < 0);

    string target = emsg->target;

    if (target.empty()) {
        target =  machine_name;
    }

    int pipe_from_child = -1;
    int pipe_to_child = -1;
    FileChunkMsg *fmsg = nullptr;

    pid_t pid = start_install_environment(envbasedir, target, emsg->name, client->channel,
                    pipe_to_child, pipe_from_child, fmsg, user_uid, user_gid, nice_level);

    if( pid <= 0 ) {
        delete fmsg;
        remove_environment_files(envbasedir, target + "/" + emsg->name);
        handle_end(client, 144);
        return false;
    }

    client->status = Client::TOINSTALL;
    client->outfile = target + "/" + emsg->name;
    current_kids++;

    trace() << "PID of child thread running untaring environment: " << pid << endl;
    client->pipe_to_child = pipe_to_child;
    client->pipe_from_child = pipe_from_child;
    client->child_pid = pid;

    if (!handle_file_chunk_env(client, fmsg)) {
        delete fmsg;
        return false;
    }

    delete fmsg;
    return true;
}

bool Daemon::handle_file_chunk_env(Client *client, Msg *msg)
{
    /* this sucks, we can block when we're writing
       the file chunk to the child, but we can't let the child
       handle MsgChannel itself due to MsgChannel's stupid
       caching layer inbetween, which causes us to lose partial
       data after the END msg of the env transfer.  */

    assert(client);
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);
    assert(client->pipe_to_child >= 0);

    if (*msg == Msg::FILE_CHUNK) {
        FileChunkMsg *fcmsg = static_cast<FileChunkMsg *>(msg);
        ssize_t len = fcmsg->len;
        off_t off = 0;

        while (len) {
            ssize_t bytes = write(client->pipe_to_child, fcmsg->buffer + off, len);

            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            if (bytes < 0 && errno == EPIPE) {
                // Broken pipe may mean the unpacking has failed, but it also may
                // mean the child has already finished successfully (it seems to happen,
                // maybe some tar implementations add needless trailing bytes?).
                // Wait for the child to finish to find out whether it was ok.
                return true;
            }

            if (bytes == -1) {
                log_perror("write to transfer env pipe failed.");
                handle_end(client, 137);
                return false;
            }

            len -= bytes;
            off += bytes;
        }

        return true;
    }

    if (*msg == Msg::END) {
        trace() << "received end of environment, waiting for child" << endl;
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
        if( client->child_pid >= 0 ) {
            // Transfer done, wait for handle_transfer_env_child_done() to finish the handling.
            client->status = Client::WAITINSTALL; // Ignore further messages until child finishes.
            return true;
        }
        // Transfer done, child done, finish.
        return finish_transfer_env( client );
    }

    // unexpected message type
    log_error() << "protocol error while receiving environment (" << msg->to_string() << ")" << endl;
    handle_end(client, 138);
    return false;
}

bool Daemon::handle_env_install_child_done(Client *client)
{
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);
    assert(client->child_pid >= 0);
    assert(client->pipe_from_child >= 0);
    bool success = false;
    for (;;) {
        char resultByte;
        ssize_t n = ::read(client->pipe_from_child, &resultByte, 1);
        if (n == -1 && errno == EINTR)
            continue;
        // The child at the end of start_install_environment() writes status on success.
        if (n == 1 && resultByte == 0 )
            success = true;
        break;
    }
    log_info() << "handle_env_install_child_done PID " << client->child_pid << " for " << client->outfile
        << " status: " << ( success ? "success" : "failed" ) << endl;
    client->child_pid = -1;
    assert(current_kids > 0);
    current_kids--;
    if (client->pipe_from_child >= 0) {
        close(client->pipe_from_child);
        client->pipe_from_child = -1;
    }
    if( !success )
        return finish_transfer_env( client, true ); // cancel
    if( client->pipe_to_child >= 0 ) {
        // we still haven't received END message, wait for that
        assert( client->status == Client::TOINSTALL );
        return true;
    }
    // Child done, transfer done, finish.
    return finish_transfer_env( client );
}

bool Daemon::finish_transfer_env(Client *client, bool cancel)
{
    log_info() << "finish_transfer_env for " << client->outfile
        << ( cancel ? " (cancel)" : "" ) << endl;

    assert(client->outfile.size());
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);

    if (client->pipe_from_child >= 0) {
        assert( cancel ); // If not cancelled, this is closed by handle_env_install_child_done().
        close(client->pipe_from_child);
        client->pipe_from_child = -1;
    }
    if (client->pipe_to_child >= 0) {
        assert( cancel ); // If not cancelled, this is closed by handle_file_chunk_env().
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
    }
    if (client->child_pid >= 0 ) {
        assert( cancel ); // If not cancelled, this is handled by handle_env_install_child_done().
        kill( client->child_pid, SIGTERM );
        int status;
        trace() << "finish_transfer_env kill and waiting for child PID " << client->child_pid <<endl;
        while (waitpid(client->child_pid, &status, 0) < 0 && errno == EINTR)
            ;
        client->child_pid = -1;
        assert(current_kids > 0);
        current_kids--;
    }

    size_t installed_size = 0;
    if( !cancel ) {
        installed_size = finalize_install_environment(envbasedir, client->outfile,
                            user_uid, user_gid);
        log_info() << "installed_size: " << installed_size << endl;
    }
    if( installed_size == 0 )
        remove_environment_files(envbasedir, client->outfile);

    client->status = Client::UNKNOWN;
    string current = client->outfile;
    client->outfile.clear();

    if (installed_size) {
        cache_size += installed_size;
        received_environments[current].last_use = time(nullptr);
        received_environments[current].size = installed_size;
        log_info() << "installed " << current << " size: " << installed_size
                    << " all: " << cache_size << endl;
    }

    check_cache_size(current);

    bool r = reannounce_environments(); // do that before the file compiles

    if (!maybe_stats(true)) { // update stats in case our disk is too full to accept more jobs
        r = false;
    }

    return r;
}

void Daemon::check_cache_size(const string &new_env)
{
    time_t now = time(nullptr);

    while (cache_size > cache_size_limit) {
        string oldest_received;
        string oldest_native;
        // I don't dare to use (time_t)-1
        time_t oldest_time = time(nullptr) + 90000;

        for (const auto& it : received_environments ) {
            trace() << "considering cached environment: " << it.first << " " << it.second.last_use << " " << oldest_time << endl;

            if (access(string(envbasedir + "/target=" + it.first + "/usr/bin/as").c_str(), X_OK) != 0) {
                trace() << string(envbasedir + "/target=" + it.first + "/usr/bin/as") << " is missing, removing environment" << endl;
                // force removing this one
                oldest_time = 0;
                oldest_received = it.first;
                break;
            }

            // ignore recently used envs (they might be in use _right_ now)
            int keep_timeout = 200;

            if (it.second.last_use < oldest_time && now - it.second.last_use > keep_timeout) {
                bool env_currently_in_use = false;

                for (Clients::const_iterator it2 = clients.begin(); it2 != clients.end(); ++it2)  {
                    if (it2->second->status == Client::TOCOMPILE
                            || it2->second->status == Client::TOINSTALL
                            || it2->second->status == Client::WAITINSTALL
                            || it2->second->status == Client::WAITFORCHILD) {

                        assert(it2->second->job);
                        string envforjob = it2->second->job->targetPlatform() + "/"
                                           + it2->second->job->environmentVersion();

                        if (envforjob == it.first) {
                            env_currently_in_use = true;
                        }
                    }
                }

                if (!env_currently_in_use) {
                    oldest_time = it.second.last_use;
                    oldest_received = it.first;
                }
            }
        }
        for (const auto& it : native_environments ) {
            trace() << "considering native environment: " << it.first << " " << it.second.last_use << " " << oldest_time << endl;

            if (!it.second.name.empty() && access(it.second.name.c_str(), R_OK) != 0) {
                trace() << it.second.name << " is missing, removing environment" << endl;
                // force removing this one
                oldest_time = 0;
                oldest_native = it.first;
                break;
            }

            // ignore recently used envs (they might be in use _right_ now)
            int keep_timeout = 200;

            // Allow removing native environments only after a longer period,
            // unless there are many native environments.
            if (native_environments.size() < 5) {
                keep_timeout = 24 * 60 * 60;    // 1 day
            }

            if (it.second.create_env_pipe) {
                keep_timeout = 365 * 24 * 60 * 60; // do not remove if it's still being created
            }

            if (it.second.last_use < oldest_time && now - it.second.last_use > keep_timeout) {
                oldest_time = it.second.last_use;
                oldest_native = it.first;
            }
        }

        if ((oldest_received.empty() || oldest_received == new_env)
            && (oldest_native.empty() || oldest_native == new_env)) {
            break;
        }

        if (!oldest_native.empty())
            remove_native_environment(oldest_native);
        else
            remove_environment(oldest_received);
    }
}

void Daemon::remove_native_environment(const string& env_key)
{
    assert(!env_key.empty());
    remove_native_environment_files(env_key);
    const NativeEnvironment &env = native_environments[env_key];
    trace() << "removing " << env.name << " " << env.size << endl;
    if (env.create_env_pipe) {
        if ((-1 == close(env.create_env_pipe)) && (errno != EBADF)){
            log_perror("close failed");
        }
        // TODO kill the still running icecc-create-env process?
    }
    assert( cache_size >= env.size );
    cache_size -= env.size;
    native_environments.erase(env_key);
}

void Daemon::remove_environment(const string& env_key)
{
    assert(!env_key.empty());
    remove_environment_files(envbasedir, env_key);
    const ReceivedEnvironment& env = received_environments[env_key];
    trace() << "removing " << envbasedir << "/target=" << env_key << " " << env.size << endl;
    assert( cache_size >= env.size );
    cache_size -= env.size;
    received_environments.erase(env_key);
}

bool Daemon::handle_get_native_env(Client *client, GetNativeEnvMsg *msg)
{
    string env_key;
    map<string, time_t> filetimes;
    struct stat st;

    string compiler = msg->compiler;
    // Older clients passed simply "gcc" or "clang" and not a binary.
    if( !IS_PROTOCOL_VERSION(41, client->channel) && compiler.find('/') == string::npos)
        compiler = "/usr/bin/" + compiler;

    string ccompiler = get_c_compiler(compiler);
    string cppcompiler = get_cpp_compiler(compiler);

    trace() << "get_native_env for " << msg->compiler
        << " (" << ccompiler << "," << cppcompiler << ")" << endl;

    if (stat(ccompiler.c_str(), &st) != 0) {
        log_error() << "Compiler binary " << ccompiler << " for environment not found." << endl;
        client->channel->send_msg(EndMsg());
        handle_end(client, 122);
        return false;
    }
    filetimes[ccompiler] = st.st_mtime;
    if (stat(cppcompiler.c_str(), &st) == 0) {
        // C++ compiler is optional.
        filetimes[cppcompiler] = st.st_mtime;
    }

    env_key = msg->compression + ":" + ccompiler;
    for (list<string>::const_iterator it = msg->extrafiles.begin();
            it != msg->extrafiles.end(); ++it) {
        env_key += ':';
        env_key += *it;

        if (stat(it->c_str(), &st) != 0) {
            log_error() << "Extra file " << *it << " for environment not found." << endl;
            client->channel->send_msg(EndMsg());
            handle_end(client, 122);
            return false;
        }

        filetimes[*it] = st.st_mtime;
    }

    if (native_environments[env_key].name.length()) {
        const NativeEnvironment &env = native_environments[env_key];

        if (env.filetimes != filetimes || access(env.name.c_str(), R_OK) != 0) {
            trace() << "native_env needs rebuild" << endl;
            remove_native_environment(env.name);
        }
    }

    trace() << "get_native_env " << native_environments[env_key].name
            << " (" << env_key << ")" << endl;

    client->status = Client::WAITCREATEENV;
    client->pending_create_env = env_key;

    if (native_environments[env_key].name.length()) { // already available
        return finish_get_native_env(client, env_key);
    } else {
        NativeEnvironment &env = native_environments[env_key]; // also inserts it
        if (!env.create_env_pipe) { // start creating it only if not already in progress
            env.filetimes = filetimes;
            trace() << "start_create_env " << env_key << endl;
            env.create_env_pipe = start_create_env(envbasedir, user_uid, user_gid, ccompiler,
                msg->extrafiles, msg->compression);
        } else {
            trace() << "waiting for already running create_env " << env_key << endl;
        }
    }
    return true;
}

bool Daemon::finish_get_native_env(Client *client, string env_key)
{
    assert(client->status == Client::WAITCREATEENV);
    assert(client->pending_create_env == env_key);
    UseNativeEnvMsg m(native_environments[env_key].name);

    if (!client->channel->send_msg(m)) {
        handle_end(client, 138);
        return false;
    }

    native_environments[env_key].last_use = time(nullptr);
    client->status = Client::GOTNATIVE;
    client->pending_create_env.clear();
    return true;
}

bool Daemon::create_env_finished(string env_key)
{
    assert(native_environments.count(env_key));
    NativeEnvironment &env = native_environments[env_key];

    trace() << "create_env_finished " << env_key << endl;
    assert(env.create_env_pipe);
    size_t installed_size = finish_create_env(env.create_env_pipe, envbasedir, env.name);
    env.create_env_pipe = 0;

    // we only clean out cache on next target install
    cache_size += installed_size;
    trace() << "cache_size = " << cache_size << endl;

    if (!installed_size) {
        bool repeat = true;
        while(repeat) {
            repeat = false;
            for (Clients::const_iterator it = clients.begin(); it != clients.end(); ++it)  {
                if (it->second->pending_create_env == env_key) {
                    it->second->channel->send_msg(EndMsg());
                    handle_end(it->second, 121);
                    // The handle_end call invalidates our iterator, so break out of the loop,
                    // but try again just in case, until there's no match.
                    repeat = true;
                    break;
                }
            }
        }
        return false;
    }

    env.last_use = time(nullptr);
    env.size = installed_size;
    check_cache_size(env.name);

    for (Clients::const_iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second->pending_create_env == env_key)
            finish_get_native_env(it->second, env_key);
    }
    return true;
}

bool Daemon::handle_job_done(Client *cl, JobDoneMsg *m)
{
    if (cl->status == Client::CLIENTWORK) {
        if(cl->fulljob)
            clients.active_processes -= std::max((unsigned int)1, max_kids);
        else
            clients.active_processes--;
    }

    cl->status = Client::JOBDONE;
    JobDoneMsg *msg = static_cast<JobDoneMsg *>(m);
    trace() << "handle_job_done " << msg->job_id << " " << (cl->fulljob ? "(full) " : "")
        << msg->exitcode << endl;
    cl->fulljob = false;

    if (!m->is_from_server()
            && (m->user_msec + m->sys_msec) <= m->real_msec) {
        icecream_load += (m->user_msec + m->sys_msec) / num_cpus;
    }

    assert(msg->job_id == cl->job_id);
    cl->job_id = 0; // the scheduler doesn't have it anymore

    msg->client_count = clients.size();

    return send_scheduler(*msg);
}

void Daemon::handle_old_request()
{
    while ((current_kids + clients.active_processes) < std::max((unsigned int)1, max_kids)) {

        Client *client = clients.get_earliest_client(Client::LINKJOB);

        if (client) {
            trace() << "send JobLocalBeginMsg to client" << endl;

            if (!client->channel->send_msg(JobLocalBeginMsg())) {
                log_warning() << "can't send start message to client" << endl;
                handle_end(client, 112);
            } else {
                client->status = Client::CLIENTWORK;
                if(client->fulljob) { // reserve the entire node
                    clients.active_processes += std::max((unsigned int)1, max_kids);
                    trace() << "pushed full local job " << client->client_id << endl;
                } else {
                    clients.active_processes++;
                    trace() << "pushed local job " << client->client_id << endl;
                }
                if (!send_scheduler(JobLocalBeginMsg(client->client_id, client->outfile,
                        client->fulljob))) {
                    return;
                }
            }

            continue;
        }

        client = clients.get_earliest_client(Client::PENDING_USE_CS);

        if (client) {
            trace() << "pending " << client->dump() << endl;

            if (client->channel->send_msg(*client->usecsmsg)) {
                client->status = Client::CLIENTWORK;
                /* we make sure we reserve a spot and the rest is done if the
                 * client contacts as back with a Compile request */
                clients.active_processes++;
            } else {
                handle_end(client, 129);
            }

            continue;
        }

        /* we don't want to handle TOCOMPILE jobs as long as our load
           is too high */
        if (current_load >= 1000) {
            break;
        }

        client = clients.get_earliest_client(Client::TOCOMPILE);

        if (client) {
            CompileJob *job = client->job;
            assert(job);
            int sock = -1;
            pid_t pid = -1;

            trace() << "request for job " << job->jobID() << endl;

            string envforjob = job->targetPlatform() + "/" + job->environmentVersion();
            received_environments[envforjob].last_use = time(nullptr);
            pid = handle_connection(envbasedir, job, client->channel, sock, mem_limit, user_uid, user_gid);
            trace() << "handle connection returned " << pid << endl;

            if (pid > 0) {
                current_kids++;
                client->status = Client::WAITFORCHILD;
                client->pipe_from_child = sock;
                client->child_pid = pid;

                if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
                    log_info() << "failed sending scheduler about " << job->jobID() << endl;
                }
            } else {
                handle_end(client, 117);
            }

            continue;
        }

        break;
    }
}

bool Daemon::handle_compile_done(Client *client)
{
    assert(client->status == Client::WAITFORCHILD);
    assert(client->child_pid > 0);
    assert(client->pipe_from_child >= 0);

    JobDoneMsg *msg = new JobDoneMsg(client->job->jobID(), -1, JobDoneMsg::FROM_SERVER, clients.size());
    assert(msg);
    assert(current_kids > 0);
    current_kids--;

    unsigned int job_stat[8];
    int end_status = 151;

    if (read(client->pipe_from_child, job_stat, sizeof(job_stat)) == sizeof(job_stat)) {
        msg->in_uncompressed = job_stat[JobStatistics::in_uncompressed];
        msg->in_compressed = job_stat[JobStatistics::in_compressed];
        msg->out_compressed = msg->out_uncompressed = job_stat[JobStatistics::out_uncompressed];
        end_status = msg->exitcode = job_stat[JobStatistics::exit_code];
        msg->real_msec = job_stat[JobStatistics::real_msec];
        msg->user_msec = job_stat[JobStatistics::user_msec];
        msg->sys_msec = job_stat[JobStatistics::sys_msec];
        msg->pfaults = job_stat[JobStatistics::sys_pfaults];
    }

    close(client->pipe_from_child);
    client->pipe_from_child = -1;
    string envforjob = client->job->targetPlatform() + "/" + client->job->environmentVersion();
    received_environments[envforjob].last_use = time(nullptr);
    if(end_status == EXIT_COMPILER_MISSING) { // Environment damaged?
        remove_environment(envforjob);
        if( !reannounce_environments())
            log_warning() << "failed reannounce environments after failed compile " << client->job->jobID() << endl;
    }

    if(!send_scheduler(*msg))
        log_warning() << "failed sending scheduler about compile done " << client->job->jobID() << endl;
    handle_end(client, end_status);
    delete msg;
    return false;
}

bool Daemon::handle_compile_file(Client *client, Msg *msg)
{
    CompileJob *job = dynamic_cast<CompileFileMsg *>(msg)->takeJob();
    assert(client);
    assert(job);
    client->job = job;

    if (client->status == Client::CLIENTWORK) {
        assert(job->environmentVersion() == "__client");

        if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
            trace() << "can't reach scheduler to tell him about compile file job "
                    << job->jobID() << endl;
            return false;
        }

        // no scheduler is not an error case!
    } else {
        client->status = Client::TOCOMPILE;
    }

    return true;
}

bool Daemon::handle_verify_env(Client *client, VerifyEnvMsg *msg)
{
    assert(msg);
    bool ok = verify_env(client->channel, envbasedir, msg->target, msg->environment, user_uid, user_gid);
    trace() << "Verify environment done, " << (ok ? "success" : "failure") << ", environment " << msg->environment
            << " (" << msg->target << ")" << endl;
    VerifyEnvResultMsg resultmsg(ok);

    if (!client->channel->send_msg(resultmsg)) {
        log_error() << "sending verify end result failed.." << endl;
        return false;
    }

    return true;
}

bool Daemon::handle_blacklist_host_env(Client *client, Msg *msg)
{
    // just forward
    assert(dynamic_cast<BlacklistHostEnvMsg *>(msg));
    assert(client);
    (void)client;

    if (!scheduler) {
        return false;
    }

    return send_scheduler(*msg);
}

void Daemon::handle_end(Client *client, int exitcode)
{
    trace() << "handle_end " << client->client_id << " " << client->channel->name << endl;
#ifdef ICECC_DEBUG
    trace() << "handle_end " << client->dump() << endl;
    trace() << dump_internals() << endl;
#endif
    fd2client.erase(client->channel->fd);

    if (client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL) {
        finish_transfer_env(client, true);
    }

    if (client->status == Client::CLIENTWORK) {
        if(client->fulljob)
            clients.active_processes -= std::max((unsigned int)1, max_kids);
        else
            clients.active_processes--;
    }
    client->fulljob = false;

    if (client->status == Client::WAITCOMPILE && exitcode == 119) {
        /* the client sent us a real good bye, so forget about the scheduler */
        client->job_id = 0;
    }

    /* Delete from the clients map before send_scheduler, which causes a
       double deletion. */
    if (!clients.erase(client->channel)) {
        log_error() << "client can't be erased: " << client->channel << endl;
        flush_debug();
        log_error() << dump_internals() << endl;
        flush_debug();
        assert(false);
    }

    if (scheduler && client->status != Client::WAITFORCHILD) {
        int job_id = client->job_id;
        bool use_client_id = false;

        if (client->status == Client::TOCOMPILE) {
            job_id = client->job->jobID();
        }

        if (client->status == Client::WAITFORCS) {
            // We don't know the job id, because we haven't received a reply
            // from the scheduler yet. Use client_id to identify the job,
            // the scheduler will use it for matching.
            use_client_id = true;
            assert( client->client_id > 0 );
        }

        if (job_id > 0 || use_client_id) {
            JobDoneMsg::from_type flag = JobDoneMsg::FROM_SUBMITTER;

            switch (client->status) {
            case Client::TOCOMPILE:
                flag = JobDoneMsg::FROM_SERVER;
                break;
            case Client::UNKNOWN:
            case Client::GOTNATIVE:
            case Client::JOBDONE:
            case Client::WAITFORCHILD:
            case Client::LINKJOB:
            case Client::TOINSTALL:
            case Client::WAITINSTALL:
            case Client::WAITCREATEENV:
                assert(false);   // should not have a job_id
                break;
            case Client::WAITCOMPILE:
            case Client::PENDING_USE_CS:
            case Client::CLIENTWORK:
            case Client::WAITFORCS:
                flag = JobDoneMsg::FROM_SUBMITTER;
                break;
            }

            trace() << "scheduler->send_msg( JobDoneMsg( " << client->dump() << ", " << exitcode << "))\n";

            JobDoneMsg msg(job_id, exitcode, flag, clients.size());
            if( use_client_id ) {
                msg.set_unknown_job_client_id( client->client_id );
            }
            if (!send_scheduler(msg)) {
                trace() << "failed to reach scheduler for remote job done msg!" << endl;
            }
        } else if (client->status == Client::CLIENTWORK) {
            // Clientwork && !job_id == LINK
            trace() << "scheduler->send_msg( JobLocalDoneMsg( " << client->client_id << ") );\n";

            if (!send_scheduler(JobLocalDoneMsg(client->client_id))) {
                trace() << "failed to reach scheduler for local job done msg!" << endl;
            }
        }
    }

    delete client;
}

void Daemon::clear_children()
{
    while (!clients.empty()) {
        Client *cl = clients.first();
        handle_end(cl, 116);
    }

    while (current_kids > 0) {
        int status;
        pid_t child;

        while ((child = waitpid(-1, &status, 0)) < 0 && errno == EINTR) {}

        current_kids--;
    }

    // they should be all in clients too
    assert(fd2client.empty());

    fd2client.clear();
    new_client_id = 0;
    trace() << "cleared children\n";
}

bool Daemon::handle_get_cs(Client *client, Msg *msg)
{
    GetCSMsg *umsg = dynamic_cast<GetCSMsg *>(msg);
    assert(client);
    client->status = Client::WAITFORCS;
    client->niceness = umsg->niceness;
    umsg->client_id = client->client_id;
    trace() << "handle_get_cs " << umsg->client_id << endl;

    if (!scheduler) {
        /* now the thing is this: if there is no scheduler
           there is no point in trying to ask him. So we just
           redefine this as local job */
        client->usecsmsg = new UseCSMsg(umsg->target, "127.0.0.1", daemon_port,
                                        umsg->client_id, true, 1, 0);
        client->status = Client::PENDING_USE_CS;
        client->job_id = umsg->client_id;
        return true;
    }

    umsg->client_count = clients.size();

    return send_scheduler(*umsg);
}

int Daemon::handle_cs_conf(ConfCSMsg *msg)
{
    max_scheduler_pong = msg->max_scheduler_pong;
    max_scheduler_ping = msg->max_scheduler_ping;
    return 0;
}

bool Daemon::handle_local_job(Client *client, Msg *msg)
{
    JobLocalBeginMsg* m = dynamic_cast<JobLocalBeginMsg *>(msg);
    client->status = Client::LINKJOB;
    client->outfile = m->outfile;
    client->fulljob = m->fulljob;
    return true;
}

bool Daemon::handle_activity(Client *client)
{
    assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

    Msg *msg = client->channel->get_msg(0, true);

    if (!msg) {
        handle_end(client, 118);
        return false;
    }

    bool ret = false;

    if (client->status == Client::TOINSTALL) {
        ret = handle_file_chunk_env(client, msg);
        delete msg;
        return ret;
    }

    switch (*msg) {
    case Msg::GET_NATIVE_ENV:
        ret = handle_get_native_env(client, dynamic_cast<GetNativeEnvMsg *>(msg));
        break;
    case Msg::COMPILE_FILE:
        ret = handle_compile_file(client, msg);
        break;
    case Msg::TRANFER_ENV:
        ret = handle_transfer_env(client, dynamic_cast<EnvTransferMsg*>(msg));
        break;
    case Msg::GET_CS:
        ret = handle_get_cs(client, msg);
        break;
    case Msg::END:
        handle_end(client, 119);
        ret = false;
        break;
    case Msg::JOB_LOCAL_BEGIN:
        ret = handle_local_job(client, msg);
        break;
    case Msg::JOB_DONE:
        ret = handle_job_done(client, dynamic_cast<JobDoneMsg *>(msg));
        break;
    case Msg::VERIFY_ENV:
        ret = handle_verify_env(client, dynamic_cast<VerifyEnvMsg *>(msg));
        break;
    case Msg::BLACKLIST_HOST_ENV:
        ret = handle_blacklist_host_env(client, msg);
        break;
    default:
        log_error() << "protocol error " << msg->to_string() << " on client "
                    << client->dump() << endl;
        client->channel->send_msg(EndMsg());
        handle_end(client, 120);
        ret = false;
    }

    delete msg;
    return ret;
}

void Daemon::answer_client_requests()
{
#ifdef ICECC_DEBUG

    if (clients.size() + current_kids) {
        log_info() << dump_internals() << endl;
    }

    log_info() << "clients " << clients.dump_per_status() << " " << current_kids
               << " (" << max_kids << ")" << endl;

#endif

    /* reap zombies */
    int status;

    while (waitpid(-1, &status, WNOHANG) < 0 && errno == EINTR) {}

    handle_old_request();

    /* collect the stats after the children exited icecream_load */
    if (scheduler) {
        maybe_stats();
    }

    vector< pollfd > pollfds;
    pollfds.reserve( fd2client.size() + 6 );
    pollfd pfd; // tmp varible

    if (tcp_listen_fd != -1) {
        pfd.fd = tcp_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }
    if (tcp_listen_local_fd != -1) {
        pfd.fd = tcp_listen_local_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    pfd.fd = unix_listen_fd;
    pfd.events = POLLIN;
    pollfds.push_back(pfd);

    for (auto it = fd2client.begin(); it != fd2client.end();) {
        int i = it->first;
        Client *client = it->second;
        MsgChannel *c = client->channel;
        ++it;
        /* don't select on a fd that we're currently not interested in.
           Avoids that we wake up on an event we're not handling anyway */
        assert(client);
        int current_status = client->status;
        bool ignore_channel = current_status == Client::WAITFORCHILD ||
                              current_status == Client::WAITINSTALL;

        /* when the remote host is full with work, the wait time for it to free up and
           fork a child to compile could be long. If the input is ready to read, we will read
           them and save it for the child; otherwise the write on the client side would be blocked */
        if ( current_status == Client::TOCOMPILE ||
             (!ignore_channel && (!c->has_msg() || handle_activity(client)))) {
            pfd.fd = i;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }

        if ((current_status == Client::WAITFORCHILD
                || current_status == Client::TOINSTALL
                || current_status == Client::WAITINSTALL)
              && client->pipe_from_child != -1) {
            pfd.fd = client->pipe_from_child;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }
    }

    if (scheduler) {
        pfd.fd = scheduler->fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    } else if (discover && discover->listen_fd() >= 0) {
        /* We don't explicitely check for discover->get_fd() being in
        the selected set below.  If it's set, we simply will return
        and our call will make sure we try to get the scheduler.  */
        pfd.fd = discover->listen_fd();
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    for (map<string, NativeEnvironment>::const_iterator it = native_environments.begin();
            it != native_environments.end(); ++it) {
        if (it->second.create_env_pipe) {
            pfd.fd = it->second.create_env_pipe;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }
    }

    int ret = poll(pollfds.data(), pollfds.size(), max_scheduler_pong * 1000);

    if (ret < 0 && errno != EINTR) {
        log_perror("poll");
        close_scheduler();
        return;
    }
    // Reset debug if needed, but only if we aren't waiting for any child processes to finish,
    // otherwise their debug output could end up reset in the middle (and flush log marks used
    // by tests could be written out before debug output from children).
    if( current_kids == 0 ) {
        reset_debug_if_needed();
    }

    if (ret > 0) {
        bool had_scheduler = scheduler;

        if (scheduler && pollfd_is_set(pollfds, scheduler->fd, POLLIN)) {
            while (!scheduler->read_a_bit() || scheduler->has_msg()) {
                Msg *msg = scheduler->get_msg(0, true);

                if (!msg) {
                    log_warning() << "scheduler closed connection" << endl;
                    close_scheduler();
                    clear_children();
                    return;
                }

                ret = 0;

                switch (*msg) {
                case Msg::PING:

                    if (!IS_PROTOCOL_VERSION(27, scheduler)) {
                        ret = !send_scheduler(PingMsg());
                    }

                    break;
                case Msg::USE_CS:
                    ret = scheduler_use_cs(static_cast<UseCSMsg *>(msg));
                    break;
                case Msg::NO_CS:
                    ret = scheduler_no_cs(static_cast<NoCSMsg *>(msg));
                    break;
                case Msg::GET_INTERNALS:
                    ret = scheduler_get_internals();
                    break;
                case Msg::CS_CONF:
                    ret = handle_cs_conf(static_cast<ConfCSMsg *>(msg));
                    break;
                default:
                    log_error() << "unknown scheduler type " << msg->to_string() << endl;
                    ret = 1;
                }

                delete msg;

                if (ret) {
                    close_scheduler();
                    return;
                }
            }
        }

        int listen_fd = -1;

        if (tcp_listen_fd != -1 && pollfd_is_set(pollfds, tcp_listen_fd, POLLIN)) {
            listen_fd = tcp_listen_fd;
        }
        if (tcp_listen_local_fd != -1 && pollfd_is_set(pollfds, tcp_listen_local_fd, POLLIN)) {
            listen_fd = tcp_listen_local_fd;
        }
        if (pollfd_is_set(pollfds, unix_listen_fd, POLLIN)) {
            listen_fd = unix_listen_fd;
        }

        if (listen_fd != -1) {
            struct sockaddr cli_addr;
            socklen_t cli_len = sizeof cli_addr;
            int acc_fd = accept(listen_fd, &cli_addr, &cli_len);

            if (acc_fd < 0) {
                log_perror("accept error");
            }

            if (acc_fd == -1 && errno != EINTR) {
                log_perror("accept failed:");
                return;
            }

            MsgChannel *c = Service::createChannel(acc_fd, &cli_addr, cli_len);

            if (!c) {
                return;
            }

            Client *client = new Client;
            client->client_id = ++new_client_id;
            client->channel = c;
            clients[c] = client;

            fd2client[c->fd] = client;
            trace() << "accepted " << c->fd << " " << c->name << " as " << client->client_id << endl;

            while (!c->read_a_bit() || c->has_msg()) {
                if (!handle_activity(client)) {
                    break;
                }

                if (client->status == Client::TOCOMPILE
                        || client->status == Client::WAITFORCHILD
                        || client->status == Client::WAITINSTALL) {
                    break;
                }
            }
        } else {
            for (auto it = fd2client.begin(); it != fd2client.end();)  {
                int i = it->first;
                Client *client = it->second;
                MsgChannel *c = client->channel;
                assert(client);
                ++it;

                if (client->status == Client::WAITFORCHILD
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_compile_done(client)) {
                        return;
                    }
                }
                if ((client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL)
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_env_install_child_done(client)) {
                        return;
                    }
                }

                if (pollfd_is_set(pollfds, i, POLLIN)) {
                    if( client->status == Client::TOCOMPILE )
                    {
                        /* read as the preprocessed input is ready but don't process it and leave it to the child
                           if we didn't read it now, the client would be blocked and timed out */
                        c->read_a_bit();
                    }
                    else
                    {
                        assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

                        while (!c->read_a_bit() || c->has_msg()) {
                            if (!handle_activity(client)) {
                                break;
                            }

                            if (client->status == Client::TOCOMPILE
                                || client->status == Client::WAITFORCHILD
                                || client->status == Client::WAITINSTALL) {
                                break;
                            }
                        }
                    }
                }
            }

            for (map<string, NativeEnvironment>::iterator it = native_environments.begin();
                 it != native_environments.end(); ) {
                if (it->second.create_env_pipe && pollfd_is_set(pollfds, it->second.create_env_pipe, POLLIN)) {
                    if(!create_env_finished(it->first))
                    {
                        native_environments.erase(it++);
                        continue;
                    }
                }
                ++it;
            }
        }

        if (had_scheduler && !scheduler) {
            clear_children();
            return;
        }

    }
}

bool Daemon::reconnect()
{
    if (scheduler) {
        return true;
    }

    if (!discover && next_scheduler_connect > time(nullptr)) {
        trace() << "Delaying reconnect." << endl;
        return false;
    }

#ifdef ICECC_DEBUG
    trace() << "reconn " << dump_internals() << endl;
#endif

    if (!discover || (nullptr == (scheduler = discover->try_get_scheduler()) && discover->timed_out())) {
        delete discover;
        discover = new DiscoverSched(netname, max_scheduler_pong, schedname, scheduler_port);
    }

    if (!scheduler) {
        log_warning() << "scheduler not yet found/selected." << endl;
        return false;
    }

    delete discover;
    discover = nullptr;
    sockaddr_in name;
    socklen_t len = sizeof(name);
    int error = getsockname(scheduler->fd, (struct sockaddr*)&name, &len);

    if (!error) {
        remote_name = inet_ntoa(name.sin_addr);
    } else {
        remote_name = string();
    }

    log_info() << "Connected to scheduler (I am known as " << remote_name << ")" << endl;
    current_load = -1000;
    gettimeofday(&last_stat, nullptr);
    icecream_load = 0;

    LoginMsg lmsg(daemon_port, determine_nodename(), machine_name, supported_features);
    lmsg.envs = available_environments(envbasedir);
    lmsg.max_kids = max_kids;
    lmsg.noremote = noremote;
    return send_scheduler(lmsg);
}

int Daemon::working_loop()
{
    for (;;) {
        reconnect();
        answer_client_requests();

        if (exit_main_loop) {
            close_scheduler();
            clear_children();
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int max_processes = -1;
    srand(time(nullptr) + getpid());

    Daemon d;

    int debug_level = Error;
    string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h

    while (true) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, nullptr, 'n' },
            { "max-processes", 1, nullptr, 'm' },
            { "help", 0, nullptr, 'h' },
            { "daemonize", 0, nullptr, 'd'},
            { "log-file", 1, nullptr, 'l'},
            { "nice", 1, nullptr, 0},
            { "name", 1, nullptr, 'N'},
            { "scheduler-host", 1, nullptr, 's' },
            { "env-basedir", 1, nullptr, 'b' },
            { "user-uid", 1, nullptr, 'u'},
            { "cache-limit", 1, nullptr, 0},
            { "no-remote", 0, nullptr, 0},
            { "interface", 1, nullptr, 'i'},
            { "port", 1, nullptr, 'p'},
            { nullptr, 0, nullptr, 0 }
        };

        const int c = getopt_long(argc, argv, "N:n:m:l:s:hvdb:u:i:p:", long_options, &option_index);

        if (c == -1) {
            break;    // eoo
        }

        switch (c) {
        case 0: {
            string optname = long_options[option_index].name;

            if (optname == "nice") {
                if (optarg && *optarg) {
                    errno = 0;
                    int tnice = atoi(optarg);

                    if (!errno) {
                        nice_level = tnice;
                    }
                } else {
                    usage("Error: --nice requires argument");
                }
            } else if (optname == "name") {
                if (optarg && *optarg) {
                    d.nodename = optarg;
                } else {
                    usage("Error: --name requires argument");
                }
            } else if (optname == "cache-limit") {
                if (optarg && *optarg) {
                    errno = 0;
                    int mb = atoi(optarg);

                    if (!errno) {
                        cache_size_limit = mb * 1024 * 1024;
                    }
                } else {
                    usage("Error: --cache-limit requires argument");
                }
            } else if (optname == "no-remote") {
                d.noremote = true;
            }

        }
        break;
        case 'd':
            detach = true;
            break;
        case 'N':

            if (optarg && *optarg) {
                d.nodename = optarg;
            } else {
                usage("Error: -N requires argument");
            }

            break;
        case 'l':

            if (optarg && *optarg) {
                logfile = optarg;
            } else {
                usage("Error: -l requires argument");
            }

            break;
        case 'v':

            if (debug_level < MaxVerboseLevel) {
                debug_level++;
            }

            break;
        case 'n':

            if (optarg && *optarg) {
                d.netname = optarg;
            } else {
                usage("Error: -n requires argument");
            }

            break;
        case 'm':

            if (optarg && *optarg) {
                max_processes = atoi(optarg);
            } else {
                usage("Error: -m requires argument");
            }

            break;
        case 's':

            if (optarg && *optarg) {
                string scheduler = optarg;
                size_t colon = scheduler.rfind( ':' );
                if( colon == string::npos ) {
                    d.schedname = scheduler;
                } else {
                    d.schedname = scheduler.substr(0, colon);
                    d.scheduler_port = atoi( scheduler.substr( colon + 1 ).c_str());
                    if( d.scheduler_port == 0 ) {
                        usage("Error: -s requires valid port if hostname includes colon");
                    }
                }
            } else {
                usage("Error: -s requires hostname argument");
            }

            break;
        case 'b':

            if (optarg && *optarg) {
                d.envbasedir = optarg;
            }

            break;
        case 'u':

            if (optarg && *optarg) {
                struct passwd *pw = getpwnam(optarg);

                if (!pw) {
                    usage("Error: -u requires a valid username");
                } else {
                    d.user_uid = pw->pw_uid;
                    d.user_gid = pw->pw_gid;
                    d.warn_icecc_user_errno = 0;

                    if (!d.user_gid || !d.user_uid) {
                        usage("Error: -u <username> must not be root");
                    }
                }
            } else {
                usage("Error: -u requires a valid username");
            }

            break;
        case 'i':

            if (optarg && *optarg) {
                string daemon_interface = optarg;
                if (daemon_interface.empty()) {
                    usage("Error: Invalid network interface specified");
                }

                d.daemon_interface = daemon_interface;
            } else {
                usage("Error: -i requires argument");
            }

            break;
        case 'p':

            if (optarg && *optarg) {
                d.daemon_port = atoi(optarg);

                if (0 == d.daemon_port) {
                    usage("Error: Invalid port specified");
                }
            } else {
                usage("Error: -p requires argument");
            }

            break;
        default:
            usage();
        }
    }

    if (d.warn_icecc_user_errno != 0) {
        log_errno("No icecc user on system. Falling back to nobody.", d.warn_icecc_user_errno);
    }

    umask(022);

    bool remote_disabled = false;
    if (getuid() == 0) {
        if (!logfile.length() && detach) {
            mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            ignore_result(chown("/var/log/icecc", d.user_uid, d.user_gid));
            logfile = "/var/log/icecc/iceccd.log";
        }

        mkdir("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        chmod("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        ignore_result(chown("/var/run/icecc", d.user_uid, d.user_gid));

#ifdef HAVE_LIBCAP_NG
        capng_clear(CAPNG_SELECT_BOTH);
        capng_update(CAPNG_ADD, (capng_type_t)(CAPNG_EFFECTIVE | CAPNG_PERMITTED), CAP_SYS_CHROOT);
        int r = capng_change_id(d.user_uid, d.user_gid,
                                (capng_flags_t)(CAPNG_DROP_SUPP_GRP | CAPNG_CLEAR_BOUNDING));
        if (r) {
            log_error() << "Error: capng_change_id failed: " << r << endl;
            exit(EXIT_SETUID_FAILED);
        }
#endif
    } else {
#ifdef HAVE_LIBCAP_NG
        // It's possible to have the capability even without being root.
        if (!capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        {
#endif
            d.noremote = true;
            remote_disabled = true;
        }
    }

    setup_debug(debug_level, logfile);

    log_info() << "ICECREAM daemon " VERSION " starting up (nice level "
               << nice_level << ") " << endl;
    if (remote_disabled)
        log_warning() << "Cannot use chroot, no remote jobs accepted." << endl;
    if (d.noremote)
        d.daemon_port = 0;

    d.determine_system();

    if (chdir("/") != 0) {
        log_error() << "failed to switch to root directory: "
                    << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (detach)
        if (daemon(0, 0)) {
            log_perror("Failed to run as a daemon.");
            exit(EXIT_DISTCC_FAILED);
        }

    if (dcc_ncpus(&d.num_cpus) == 0) {
        log_info() << d.num_cpus << " CPU(s) online on this server" << endl;
    }

    if (max_processes < 0) {
        max_kids = d.num_cpus;
    } else {
        max_kids = max_processes;
    }

    log_info() << "allowing up to " << max_kids << " active jobs" << endl;

    d.determine_supported_features();
    log_info() << "supported features: " << supported_features_to_string(d.supported_features) << endl;

    int ret;

    /* Still create a new process group, even if not detached */
    trace() << "not detaching\n";

    if ((ret = set_new_pgrp()) != 0) {
        return ret;
    }

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGCHLD) failed: " << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    ofstream pidFile;
    string progName = argv[0];
    progName = find_basename(progName);
    pidFilePath = string(RUNDIR) + string("/") + progName + string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << dcc_master_pid << endl;
    pidFile.close();

    if (!cleanup_cache(d.envbasedir, d.user_uid, d.user_gid)) {
        return 1;
    }

    list<string> nl = get_netnames(200, d.scheduler_port);
    trace() << "Netnames:" << endl;

    for (list<string>::const_iterator it = nl.begin(); it != nl.end(); ++it) {
        trace() << *it << endl;
    }

    if (!d.setup_listen_fds()) { // error
        return 1;
    }

    return d.working_loop();
}
