/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
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

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
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

#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

#ifndef RUSAGE_SELF
#  define RUSAGE_SELF (0)
#endif
#ifndef RUSAGE_CHILDREN
#  define RUSAGE_CHILDREN (-1)
#endif

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

const int PORT = 10245;
static std::string pidFilePath;

#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__
#endif

using namespace std;
using namespace __gnu_cxx; // for the extensions we like, e.g. hash_set

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
     * TOCOMPILE: We're supposed to compile it ourselves
     * WAITFORCS: Client asked for a CS and we asked the scheduler - waiting for its answer
     * WAITCOMPILE: Client got a CS and will ask him now (it's not me)
     * CLIENTWORK: Client is busy working and we reserve the spot (job_id is set if it's a scheduler job)
     * WAITFORCHILD: Client is waiting for the compile job to finish.
     */
    enum Status { UNKNOWN, GOTNATIVE, PENDING_USE_CS, JOBDONE, LINKJOB, TOINSTALL, TOCOMPILE,
                  WAITFORCS, WAITCOMPILE, CLIENTWORK, WAITFORCHILD, LASTSTATE=WAITFORCHILD } status;
    Client()
    {
        job_id = 0;
        channel = 0;
        job = 0;
        usecsmsg = 0;
        client_id = 0;
        status = UNKNOWN;
	pipe_to_child = -1;
	child_pid = -1;
    }

    static string status_str( Status status )
    {
        switch ( status ) {
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
        }
        assert( false );
        return string(); // shutup gcc
    }

    ~Client()
    {
        status = (Status) -1;
        delete channel;
        channel = 0;
        delete usecsmsg;
        usecsmsg = 0;
        delete job;
        job = 0;
	if (pipe_to_child >= 0)
	    close (pipe_to_child);

    }
    uint32_t job_id;
    string outfile; // only useful for LINKJOB or TOINSTALL
    MsgChannel *channel;
    UseCSMsg *usecsmsg;
    CompileJob *job;
    int client_id;
    int pipe_to_child; // pipe to child process, only valid if WAITFORCHILD or TOINSTALL
    pid_t child_pid;

    string dump() const
    {
        string ret = status_str( status ) + " " + channel->dump();
        switch ( status ) {
        case LINKJOB:
            return ret + " CID: " + toString( client_id ) + " " + outfile;
        case TOINSTALL:
            return ret + " " + toString( client_id ) + " " + outfile;
        case WAITFORCHILD:
            return ret + " CID: " + toString( client_id ) + " PID: " + toString( child_pid ) + " PFD: " + toString( pipe_to_child );
        default:
            if ( job_id ) {
                string jobs;
                if ( usecsmsg )
                {
                    jobs = " CS: " + usecsmsg->hostname;
                }
                return ret + " CID: " + toString( client_id ) + " ID: " + toString( job_id ) + jobs;
            }
            else
                return ret + " CID: " + toString( client_id );
        }
        return ret;
    }
};

class Clients : public map<MsgChannel*, Client*>
{
public:
    Clients() {
        active_processes = 0;
    }
    unsigned int active_processes;

    Client *find_by_client_id( int id ) const
    {
        for ( const_iterator it = begin(); it != end(); ++it )
            if ( it->second->client_id == id )
                return it->second;
        return 0;
    }

    Client *find_by_channel( MsgChannel *c ) const {
        const_iterator it = find( c );
        if ( it == end() )
            return 0;
        return it->second;
    }

    Client *find_by_pid( pid_t pid ) const {
        for ( const_iterator it = begin(); it != end(); ++it )
            if ( it->second->child_pid == pid )
                return it->second;
        return 0;
    }

    Client *first()
    {
        iterator it = begin();
        if ( it == end() )
            return 0;
        Client *cl = it->second;
        return cl;
    }

    string dump_status(Client::Status s) const
    {
        int count = 0;
        for ( const_iterator it = begin(); it != end(); ++it )
        {
            if ( it->second->status == s )
                count++;
        }
        if ( count )
            return toString( count ) + " " + Client::status_str( s ) + ", ";
        else
            return string();
    }

    string dump_per_status() const {
        string s;
        for(Client::Status i = Client::UNKNOWN; i <= Client::LASTSTATE;
                i=Client::Status(int(i)+1))
            s += dump_status(i);
        return s;
    }
    Client *get_earliest_client( Client::Status s ) const
    {
        // TODO: possibly speed this up in adding some sorted lists
        Client *client = 0;
        int min_client_id = 0;
        for ( const_iterator it = begin(); it != end(); ++it )
            if ( it->second->status == s && ( !min_client_id || min_client_id > it->second->client_id ))
            {
                client = it->second;
                min_client_id = client->client_id;
            }
        return client;
    }
};

static int set_new_pgrp(void)
{
    /* If we're a session group leader, then we are not able to call
     * setpgid().  However, setsid will implicitly have put us into a new
     * process group, so we don't have to do anything. */

    /* Does everyone have getpgrp()?  It's in POSIX.1.  We used to call
     * getpgid(0), but that is not available on BSD/OS. */
    if (getpgrp() == getpid()) {
        trace() << "already a process group leader\n";
        return 0;
    }

    if (setpgid(0, 0) == 0) {
        trace() << "entered process group\n";
        return 0;
    } else {
        trace() << "setpgid(0, 0) failed: " << strerror(errno) << endl;
        return EXIT_DISTCC_FAILED;
    }
}

static void dcc_daemon_terminate(int);

/**
 * Catch all relevant termination signals.  Set up in parent and also
 * applies to children.
 **/
void dcc_daemon_catch_signals(void)
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

    bool am_parent = ( getpid() == dcc_master_pid );

    /* Make sure to remove handler before re-raising signal, or
     * Valgrind gets its kickers in a knot. */
    signal(whichsig, SIG_DFL);

    if (am_parent) {
        /* kill whole group */
        kill(0, whichsig);

        /* Remove pid file */
        unlink(pidFilePath.c_str());
    }

    raise(whichsig);
}

void usage(const char* reason = 0)
{
  if (reason)
     cerr << reason << endl;

  cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [--no-remote] [-w] [-d|--daemonize] [-l logfile] [-s <schedulerhost>] [-v[v[v]]] [-r|--run-as-user] [-b <env-basedir>] [-u|--nobody-uid <nobody_uid>] [--cache-limit <MB>] [-N <node_name>]" << endl;
  exit(1);
}

struct timeval last_stat;
int mem_limit = 100;
unsigned int max_kids = 0;

size_t cache_size_limit = 100 * 1024 * 1024;

struct Daemon
{
    Clients clients;
    map<string, time_t> envs_last_use;
    string native_environment;
    string envbasedir;
    uid_t nobody_uid;
    gid_t nobody_gid;
    int tcp_listen_fd;
    int unix_listen_fd;
    string machine_name;
    string nodename;
    bool noremote;
    bool custom_nodename;
    size_t cache_size;
    map<int, MsgChannel *> fd2chan;
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

    int max_scheduler_pong;
    int max_scheduler_ping;
    string bench_source;
    unsigned int current_kids;

    Daemon() {
        envbasedir = "/tmp/icecc-envs";
        nobody_uid = 65534;
        nobody_gid = 65533;
        tcp_listen_fd = -1;
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
        scheduler = 0;
        discover = 0;
        max_scheduler_pong = MAX_SCHEDULER_PONG;
        max_scheduler_ping = MAX_SCHEDULER_PING;
        bench_source = "";
        current_kids = 0;
    }

    bool reannounce_environments() __attribute_warn_unused_result__;
    int answer_client_requests();
    bool handle_transfer_env( Client *client, Msg *msg ) __attribute_warn_unused_result__;
    bool handle_transfer_env_done( Client *client );
    bool handle_get_native_env( Client *client ) __attribute_warn_unused_result__;
    void handle_old_request();
    bool handle_compile_file( Client *client, Msg *msg ) __attribute_warn_unused_result__;
    bool handle_activity( Client *client ) __attribute_warn_unused_result__;
    bool handle_file_chunk_env(Client* client, Msg *msg) __attribute_warn_unused_result__;
    void handle_end( Client *client, int exitcode );
    int scheduler_get_internals( ) __attribute_warn_unused_result__;
    void clear_children();
    int scheduler_use_cs( UseCSMsg *msg ) __attribute_warn_unused_result__;
    bool handle_get_cs( Client *client, Msg *msg ) __attribute_warn_unused_result__;
    bool handle_local_job( Client *client, Msg *msg ) __attribute_warn_unused_result__;
    bool handle_job_done( Client *cl, JobDoneMsg *m ) __attribute_warn_unused_result__;
    bool handle_compile_done (Client* client) __attribute_warn_unused_result__;
    int handle_cs_conf( ConfCSMsg *msg);
    string dump_internals() const;
    string determine_nodename();
    void determine_system();
    bool maybe_stats(bool force = false);
    bool send_scheduler(const Msg& msg) __attribute_warn_unused_result__;
    void close_scheduler();
    bool reconnect();
    int working_loop();
    bool setup_listen_fds();
};

bool Daemon::setup_listen_fds()
{
    tcp_listen_fd = -1;
    if (!noremote) { // if we only listen to local clients, there is no point in going TCP
	if ((tcp_listen_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0) {
            log_perror ("socket()");
            return false;
        }

        int optval = 1;
        if (setsockopt (tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
            log_perror ("setsockopt()");
            return false;
        }
        
        int count = 5;
        while ( count ) {
            struct sockaddr_in myaddr;
            myaddr.sin_family = AF_INET;
            myaddr.sin_port = htons (PORT);
            myaddr.sin_addr.s_addr = INADDR_ANY;
            if (bind (tcp_listen_fd, (struct sockaddr *) &myaddr,
                      sizeof (myaddr)) < 0) {
                log_perror ("bind()");
                sleep( 2 );
                if ( !--count )
                    return false;
                continue;
            } else
                break;
        }
        
        if (listen (tcp_listen_fd, 20) < 0) {
            log_perror ("listen()");
            return false;
        }
        
        fcntl(tcp_listen_fd, F_SETFD, FD_CLOEXEC);
    }
    
    if ((unix_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        log_perror ("socket()");
        return false;
    }
    struct sockaddr_un myaddr;
    memset(&myaddr, 0, sizeof(myaddr));
    myaddr.sun_family = AF_UNIX;
    if (getuid()==0) {
        strncpy(myaddr.sun_path, "/var/run/iceccd.socket", sizeof(myaddr.sun_path)-1);
    } else {
        strncpy(myaddr.sun_path, getenv("HOME"), sizeof(myaddr.sun_path)-1);
        strncat(myaddr.sun_path, "/.iceccd.socket", sizeof(myaddr.sun_path)-1-strlen(myaddr.sun_path));
        unlink(myaddr.sun_path);
    }

    if (bind(unix_listen_fd, (struct sockaddr*)&myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return false;
    }

    if (listen (unix_listen_fd, 20) < 0) {
        log_perror ("listen()");
        return false;
    }
    
    fcntl(unix_listen_fd, F_SETFD, FD_CLOEXEC);
    
    return true;
}

void Daemon::determine_system()
{
    struct utsname uname_buf;
    if ( uname( &uname_buf ) ) {
        log_perror( "uname call failed" );
        return;
    }

    if ( nodename.length() && nodename != uname_buf.nodename )
        custom_nodename  = true;

    if (!custom_nodename)
        nodename = uname_buf.nodename;

    machine_name = determine_platform();
}

string Daemon::determine_nodename()
{
    if (custom_nodename && !nodename.empty())
        return nodename;

    // perhaps our host name changed due to network change?
    struct utsname uname_buf;
    if ( !uname( &uname_buf ) )
        nodename = uname_buf.nodename;

    return nodename;
}

bool Daemon::send_scheduler(const Msg& msg)
{
    if (!scheduler) {
        log_error() << "scheduler dead ?!" << endl;
        return false;
    }

    if (!scheduler->send_msg(msg)) {
        log_error() << "sending to scheduler failed.." << endl;
        close_scheduler();
        return false;
    }

    return true;
}

bool Daemon::reannounce_environments()
{
    log_error() << "reannounce_environments " << endl;
    LoginMsg lmsg( 0, nodename, "");
    lmsg.envs = available_environmnents(envbasedir);
    return send_scheduler( lmsg );
}

void Daemon::close_scheduler()
{
    if ( !scheduler )
        return;

    delete scheduler;
    scheduler = 0;
    delete discover;
    discover = 0;
    next_scheduler_connect = time(0) + 20 + (rand() & 31);
}

bool Daemon::maybe_stats(bool send_ping)
{
    struct timeval now;
    gettimeofday( &now, 0 );

    time_t diff_sent = ( now.tv_sec - last_stat.tv_sec ) * 1000 + ( now.tv_usec - last_stat.tv_usec ) / 1000;
    if ( diff_sent >= max_scheduler_pong * 1000 ) {
        StatsMsg msg;
        unsigned int memory_fillgrade;
        unsigned long idleLoad = 0;
        unsigned long niceLoad = 0;

        if ( !fill_stats( idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes ) )
            return false;

        time_t diff_stat = ( now.tv_sec - last_stat.tv_sec ) * 1000 + ( now.tv_usec - last_stat.tv_usec ) / 1000;
        last_stat = now;

        /* icecream_load contains time in milliseconds we have used for icecream */
        /* idle time could have been used for icecream, so claim it */
        icecream_load += idleLoad * diff_stat / 1000;

        /* add the time of our childrens, but only the time since the last run */
        struct rusage ru;
        if (!getrusage(RUSAGE_CHILDREN, &ru)) {
            uint32_t ice_msec = ( ( ru.ru_utime.tv_sec - icecream_usage.tv_sec ) * 1000 +
                ( ru.ru_utime.tv_usec - icecream_usage.tv_usec ) / 1000) / num_cpus;

            /* heuristics when no child terminated yet: account 25% of total nice as our clients */
            if ( !ice_msec && current_kids )
                ice_msec = (niceLoad * diff_stat) / (4 * 1000);

            icecream_load += ice_msec * diff_stat / 1000;

            icecream_usage.tv_sec = ru.ru_utime.tv_sec;
            icecream_usage.tv_usec = ru.ru_utime.tv_usec;
        }

        int idle_average = icecream_load;

        if (diff_sent)
            idle_average = icecream_load * 1000 / diff_sent;

        if (idle_average > 1000)
            idle_average = 1000;

        msg.load = ( 700 * (1000 - idle_average) + 300 * memory_fillgrade ) / 1000;
        if ( memory_fillgrade > 600 )
            msg.load = 1000;
        if ( idle_average < 100 )
            msg.load = 1000;

#ifdef HAVE_SYS_VFS_H
        struct statfs buf;
        int ret = statfs(envbasedir.c_str(), &buf);
        if (!ret && long(buf.f_bavail) < long(max_kids + 1 - current_kids) * 4 * 1024 * 1024 / buf.f_bsize)
            msg.load = 1000;
#endif

        // Matz got in the urine that not all CPUs are always feed
        mem_limit = std::max( int( msg.freeMem / std::min( std::max( max_kids, 1U ), 4U ) ), int( 100U ) );

        if ( abs(int(msg.load)-current_load) >= 100 || send_ping ) {
            if (!send_scheduler( msg ) )
                return false;
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
    for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
         it != fd2chan.end(); ++it)  {
        result += "  fd2chan[" + toString( it->first ) + "] = " + it->second->dump() + "\n";
    }
    for (Clients::const_iterator it = clients.begin();
         it != clients.end(); ++it)  {
        result += "  client " + toString( it->second->client_id ) + ": " +
                  it->second->dump() + "\n";
    }
    if ( cache_size )
        result += "  Cache Size: " + toString( cache_size ) + "\n";
    result += "  Architecture: " + machine_name + "\n";
    if ( !native_environment.empty() )
        result += "  NativeEnv: " + native_environment + "\n";

    if ( !envs_last_use.empty() )
        result += "  Now: " + toString( time( 0 ) ) + "\n";
    for (map<string, time_t>::const_iterator it = envs_last_use.begin();
         it != envs_last_use.end(); ++it)  {
        result += "  envs_last_use[" + it->first  + "] = " +
                  toString( it->second ) + "\n";
    }

    result += "  Current kids: " + toString( current_kids ) + " (max: " + toString( max_kids ) + ")\n";
    if ( scheduler )
        result += "  Scheduler protocol: " + toString( scheduler->protocol ) + "\n";

    StatsMsg msg;
    unsigned int memory_fillgrade = 0;
    unsigned long idleLoad = 0;
    unsigned long niceLoad = 0;

    if ( fill_stats( idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes ) )
    {
        result += "  cpu: " + toString( idleLoad ) + " idle, " +
                  toString( niceLoad ) + " nice\n";
        result += "  load: " + toString( msg.loadAvg1 / 1000. ) + ", icecream_load: " +
                  toString( icecream_load ) + "\n";
        result += "  memory: " + toString( memory_fillgrade ) + " (free: " + toString( msg.freeMem ) + ")\n";
    }

    return result;
}

int Daemon::scheduler_get_internals( )
{
    trace() << "handle_get_internals " << dump_internals() << endl;
    return send_scheduler( StatusTextMsg( dump_internals() ) ) ? 0 : 1;
}

int Daemon::scheduler_use_cs( UseCSMsg *msg )
{
    Client *c = clients.find_by_client_id( msg->client_id );
    trace() << "handle_use_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " << msg->hostname << " " << remote_name <<  endl;
    if ( !c ) {
        if (send_scheduler( JobDoneMsg( msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER ) ))
            return 1;
        return 1;
    }
    if ( msg->hostname == remote_name ) {
        c->usecsmsg = new UseCSMsg( msg->host_platform, "127.0.0.1", PORT, msg->job_id, true, 1,
                msg->matched_job_id );
        c->status = Client::PENDING_USE_CS;
    } else {
        c->usecsmsg = new UseCSMsg( msg->host_platform, msg->hostname, msg->port, 
                msg->job_id, true, 1, msg->matched_job_id );
        if (!c->channel->send_msg( *msg )) {
            handle_end(c, 143);
            return 0;
        }
        c->status = Client::WAITCOMPILE;
    }
    c->job_id = msg->job_id;
    return 0;
}

bool Daemon::handle_transfer_env( Client *client, Msg *_msg )
{
    log_error() << "handle_transfer_env" << endl;

    assert(client->status != Client::TOINSTALL &&
           client->status != Client::TOCOMPILE &&
           client->status != Client::WAITCOMPILE);
    assert(client->pipe_to_child < 0);

    EnvTransferMsg *emsg = static_cast<EnvTransferMsg*>( _msg );
    string target = emsg->target;
    if ( target.empty() )
        target =  machine_name;

    int sock_to_stdin = -1;
    FileChunkMsg* fmsg = 0;

    pid_t pid = start_install_environment( envbasedir, emsg->target,
        emsg->name, client->channel, sock_to_stdin, fmsg, nobody_uid, nobody_gid );

    client->status = Client::TOINSTALL;
    client->outfile = emsg->target + "/" + emsg->name;

    if ( pid > 0) {
        log_error() << "got pid " << pid << endl;
        current_kids++;
        client->pipe_to_child = sock_to_stdin;
        client->child_pid = pid;
        if (!handle_file_chunk_env(client, fmsg))
            pid = 0;
    }
    if (pid <= 0)
        handle_transfer_env_done (client);

    delete fmsg;
    return pid > 0;
}

bool Daemon::handle_transfer_env_done( Client *client )
{
    log_error() << "handle_transfer_env_done" << endl;

    assert(client->outfile.size());
    assert(client->status == Client::TOINSTALL);

    size_t installed_size = finalize_install_environment(envbasedir, client->outfile,
                              client->child_pid, nobody_gid);

    if (client->pipe_to_child >= 0) {
        installed_size = 0;
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
    }

    client->status = Client::UNKNOWN;
    string current = client->outfile;
    client->outfile.clear();
    client->child_pid = -1;
    assert( current_kids > 0 );
    current_kids--;

    log_error() << "installed_size: " << installed_size << endl;

    if (installed_size) {
        cache_size += installed_size;
        envs_last_use[current] = time( NULL );
        log_error() << "installed " << current << " size: " << installed_size
            << " all: " << cache_size << endl;
    }

    time_t now = time( NULL );
    while ( cache_size > cache_size_limit ) {
        string oldest;
        // I don't dare to use (time_t)-1
        time_t oldest_time = time( NULL ) + 90000;
        for ( map<string, time_t>::const_iterator it = envs_last_use.begin();
                it != envs_last_use.end(); ++it ) {
            trace() << "das ist jetzt so: " << it->first << " " << it->second << " " << oldest_time << endl;
            // ignore recently used envs (they might be in use _right_ now)
            if ( it->second < oldest_time && now - it->second > 200 ) {
                bool env_currently_in_use = false;
                for (Clients::const_iterator it2 = clients.begin(); it2 != clients.end(); ++it2)  {
                    if (it2->second->status == Client::TOCOMPILE ||
                            it2->second->status == Client::TOINSTALL ||
                            it2->second->status == Client::WAITFORCHILD) {

                        assert( it2->second->job );
                        string envforjob = it2->second->job->targetPlatform() + "/"
                            + it2->second->job->environmentVersion();
                        if (envforjob == it->first)
                            env_currently_in_use = true;
                    }
                }
                if (!env_currently_in_use) {
                    oldest_time = it->second;
                    oldest = it->first;
                }
            }
        }
        if ( oldest.empty() || oldest == current )
            break;
        size_t removed = remove_environment( envbasedir, oldest );
        trace() << "removing " << envbasedir << "/" << oldest << " " << oldest_time << " " << removed << endl;
        cache_size -= min( removed, cache_size );
        envs_last_use.erase( oldest );
    }

    bool r = reannounce_environments(); // do that before the file compiles
    // we do that here so we're not given out in case of full discs
    if ( !maybe_stats(true) )
        r = false;
    return r;
}

bool Daemon::handle_get_native_env( Client *client )
{
    trace() << "get_native_env " << native_environment << endl;

    if ( !native_environment.length() ) {
        size_t installed_size = setup_env_cache( envbasedir, native_environment,
                                                 nobody_uid, nobody_gid );
        // we only clean out cache on next target install
        cache_size += installed_size;
        trace() << "cache_size = " << cache_size << endl;
        if ( ! installed_size ) {
            client->channel->send_msg( EndMsg() );
            handle_end( client, 121 );
            return false;
        }
    }
    UseNativeEnvMsg m( native_environment );
    if (!client->channel->send_msg( m )) {
        handle_end(client, 138);
        return false;
    }
    client->status = Client::GOTNATIVE;
    return true;
}

bool Daemon::handle_job_done( Client *cl, JobDoneMsg *m )
{
    if ( cl->status == Client::CLIENTWORK )
        clients.active_processes--;
    cl->status = Client::JOBDONE;
    JobDoneMsg *msg = static_cast<JobDoneMsg*>( m );
    trace() << "handle_job_done " << msg->job_id << " " << msg->exitcode << endl;

    if(!m->is_from_server()
       && ( m->user_msec + m->sys_msec ) <= m->real_msec)
        icecream_load += (m->user_msec + m->sys_msec) / num_cpus;

    assert(msg->job_id == cl->job_id);
    cl->job_id = 0; // the scheduler doesn't have it anymore
    return send_scheduler( *msg );
}

void Daemon::handle_old_request()
{
    while ( current_kids + clients.active_processes < max_kids ) {

        Client *client = clients.get_earliest_client(Client::LINKJOB);
        if ( client ) {
            trace() << "send JobLocalBeginMsg to client" << endl;
            if (!client->channel->send_msg (JobLocalBeginMsg())) {
                log_warning() << "can't send start message to client" << endl;
                handle_end (client, 112);
            } else {
                client->status = Client::CLIENTWORK;
                clients.active_processes++;
                trace() << "pushed local job " << client->client_id << endl;
                if (!send_scheduler( JobLocalBeginMsg( client->client_id, client->outfile ) ))
                    return;
            }
            continue;
        }

        client = clients.get_earliest_client( Client::PENDING_USE_CS );
        if ( client ) {
            trace() << "pending " << client->dump() << endl;
            if(client->channel->send_msg( *client->usecsmsg )) {
                client->status = Client::CLIENTWORK;
                /* we make sure we reserve a spot and the rest is done if the
                 * client contacts as back with a Compile request */
                clients.active_processes++;
            }
            else
                handle_end(client, 129);

            continue;
        }

        /* we don't want to handle TOCOMPILE jobs as long as our load
           is too high */
        if ( current_load >= 1000)
            break;

        client = clients.get_earliest_client( Client::TOCOMPILE );
        if ( client ) {
            CompileJob *job = client->job;
            assert( job );
            int sock = -1;
            pid_t pid = -1;

            trace() << "requests--" << job->jobID() << endl;

            string envforjob = job->targetPlatform() + "/" + job->environmentVersion();
            envs_last_use[envforjob] = time( NULL );
            pid = handle_connection( envbasedir, job, client->channel, sock, mem_limit, nobody_uid, nobody_gid );
            trace() << "handle connection returned " << pid << endl;

            if ( pid > 0) {
                current_kids++;
                client->status = Client::WAITFORCHILD;
                client->pipe_to_child = sock;
                client->child_pid = pid;
                if ( !send_scheduler( JobBeginMsg( job->jobID() ) ) )
                    log_info() << "failed sending scheduler about " << job->jobID() << endl;
            }
            else
                handle_end(client, 117);
            continue;
        }
        break;
    }
}

bool Daemon::handle_compile_done (Client* client)
{
    assert(client->status == Client::WAITFORCHILD);
    assert(client->child_pid > 0);
    assert(client->pipe_to_child >= 0);

    JobDoneMsg *msg = new JobDoneMsg(client->job->jobID(), -1, JobDoneMsg::FROM_SERVER);
    assert(msg);
    assert(current_kids > 0);
    current_kids--;

    unsigned int job_stat[8];
    int end_status = 151;

    if(read(client->pipe_to_child, job_stat, sizeof(job_stat)) == sizeof(job_stat)) {
        msg->in_uncompressed = job_stat[JobStatistics::in_uncompressed];
        msg->in_compressed = job_stat[JobStatistics::in_compressed];
        msg->out_compressed = msg->out_uncompressed = job_stat[JobStatistics::out_uncompressed];
        end_status = msg->exitcode = job_stat[JobStatistics::exit_code];
        msg->real_msec = job_stat[JobStatistics::real_msec];
        msg->user_msec = job_stat[JobStatistics::user_msec];
        msg->sys_msec = job_stat[JobStatistics::sys_msec];
        msg->pfaults = job_stat[JobStatistics::sys_pfaults];
        end_status = job_stat[JobStatistics::exit_code];
    }

    close(client->pipe_to_child);
    client->pipe_to_child = -1;
    string envforjob = client->job->targetPlatform() + "/" + client->job->environmentVersion();
    envs_last_use[envforjob] = time( NULL );

    bool r = send_scheduler( *msg );
    handle_end(client, end_status);
    delete msg;
    return r;
}

bool Daemon::handle_compile_file( Client *client, Msg *msg )
{
    CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->takeJob();
    assert( client );
    assert( job );
    client->job = job;
    if ( client->status == Client::CLIENTWORK )
    {
        assert( job->environmentVersion() == "__client" );
        if ( !send_scheduler( JobBeginMsg( job->jobID() ) ) )
        {
            trace() << "can't reach scheduler to tell him about compile file job "
                    << job->jobID() << endl;
            return false;
        }
        // no scheduler is not an error case!
    } else
        client->status = Client::TOCOMPILE;
    return true;
}

void Daemon::handle_end( Client *client, int exitcode )
{
#ifdef ICECC_DEBUG
    trace() << "handle_end " << client->dump() << endl;
    trace() << dump_internals() << endl;
#endif
    fd2chan.erase (client->channel->fd);

    if (client->status == Client::TOINSTALL && client->pipe_to_child >= 0)
    {
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
        handle_transfer_env_done(client);
    }

    if ( client->status == Client::CLIENTWORK )
        clients.active_processes--;

    if ( client->status == Client::WAITCOMPILE && exitcode == 119 ) {
        /* the client sent us a real good bye, so forget about the scheduler */
 	client->job_id = 0;
    }

    /* Delete from the clients map before send_scheduler, which causes a
       double deletion. */
    if (!clients.erase( client->channel ))
    {
        log_error() << "client can't be erased: " << client->channel << endl;
        flush_debug();
        log_error() << dump_internals() << endl;
        flush_debug();
        assert(false);
    }

    if ( scheduler && client->status != Client::WAITFORCHILD ) {
        int job_id = client->job_id;
        if ( client->status == Client::TOCOMPILE )
            job_id = client->job->jobID();
        if ( client->status == Client::WAITFORCS ) {
	    job_id = client->client_id; // it's all we have
	    exitcode = CLIENT_WAS_WAITING_FOR_CS; // this is the message
        }

        if ( job_id > 0 ) {
            JobDoneMsg::from_type flag = JobDoneMsg::FROM_SUBMITTER;
            switch ( client->status ) {
            case Client::TOCOMPILE:
                flag = JobDoneMsg::FROM_SERVER;
                break;
            case Client::UNKNOWN:
            case Client::GOTNATIVE:
            case Client::JOBDONE:
            case Client::WAITFORCHILD:
            case Client::LINKJOB:
            case Client::TOINSTALL:
                assert( false ); // should not have a job_id
                break;
            case Client::WAITCOMPILE:
            case Client::PENDING_USE_CS:
            case Client::CLIENTWORK:
            case Client::WAITFORCS:
                flag = JobDoneMsg::FROM_SUBMITTER;
                break;
            }
            trace() << "scheduler->send_msg( JobDoneMsg( " << client->dump() << ", " << exitcode << "))\n";
            if (!send_scheduler( JobDoneMsg( job_id, exitcode, flag) ))
                trace() << "failed to reach scheduler for remote job done msg!" << endl;
        } else if ( client->status == Client::CLIENTWORK ) {
            // Clientwork && !job_id == LINK
            trace() << "scheduler->send_msg( JobLocalDoneMsg( " << client->client_id << ") );\n";
            if (!send_scheduler( JobLocalDoneMsg( client->client_id ) ))
                trace() << "failed to reach scheduler for local job done msg!" << endl;
        }
    }

    delete client;
}

void Daemon::clear_children()
{
    while ( !clients.empty() ) {
        Client *cl = clients.first();
        handle_end( cl, 116 );
    }

    while ( current_kids > 0 ) {
        int status;
        pid_t child;
        while ( (child = waitpid( -1, &status, 0 )) < 0 && errno == EINTR )
            ;
        current_kids--;
    }

    // they should be all in clients too
    assert( fd2chan.empty() );

    fd2chan.clear();
    new_client_id = 0;
    trace() << "cleared children\n";
}

bool Daemon::handle_get_cs( Client *client, Msg *msg )
{
    GetCSMsg *umsg = dynamic_cast<GetCSMsg*>( msg );
    assert( client );
    client->status = Client::WAITFORCS;
    umsg->client_id = client->client_id;
    trace() << "handle_get_cs " << umsg->client_id << endl;
    if ( !scheduler )
    {
        /* now the thing is this: if there is no scheduler
           there is no point in trying to ask him. So we just
           redefine this as local job */
        client->usecsmsg = new UseCSMsg( umsg->target, "127.0.0.1", PORT, 
                umsg->client_id, true, 1, 0 );
        client->status = Client::PENDING_USE_CS;
        client->job_id = umsg->client_id;
        return true;
    }

    return send_scheduler( *umsg );
}

int Daemon::handle_cs_conf(ConfCSMsg* msg)
{
    max_scheduler_pong = msg->max_scheduler_pong;
    max_scheduler_ping = msg->max_scheduler_ping;
    bench_source = msg->bench_source;

    return 0;
}

bool Daemon::handle_local_job( Client *client, Msg *msg )
{
    client->status = Client::LINKJOB;
    client->outfile = dynamic_cast<JobLocalBeginMsg*>( msg )->outfile;
    return true;
}

bool Daemon::handle_file_chunk_env(Client *client, Msg *msg)
{
    /* this sucks, we can block when we're writing
       the file chunk to the child, but we can't let the child
       handle MsgChannel itself due to MsgChannel's stupid
       caching layer inbetween, which causes us to loose partial
       data after the M_END msg of the env transfer.  */

    assert (client && client->status == Client::TOINSTALL);

    if (msg->type == M_FILE_CHUNK && client->pipe_to_child >= 0)
    {
        FileChunkMsg *fcmsg = static_cast<FileChunkMsg*>( msg );
        ssize_t len = fcmsg->len;
        off_t off = 0;
        while ( len ) {
            ssize_t bytes = write( client->pipe_to_child, fcmsg->buffer + off, len );
            if ( bytes < 0 && errno == EINTR )
                continue;

            if ( bytes == -1 ) {
                log_perror("write to transfer env pipe failed. ");

                delete msg;
                msg = 0;
                handle_end(client, 137);
                return false;
            }

            len -= bytes;
            off += bytes;
        }
        return true;
    }

    if (msg->type == M_END) {
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
        return handle_transfer_env_done(client);
    }

    if (client->pipe_to_child >= 0)
        handle_end(client, 138);

    return false;
}

bool Daemon::handle_activity( Client *client )
{
    assert(client->status != Client::TOCOMPILE);

    Msg *msg = client->channel->get_msg();
    if ( !msg ) {
        handle_end( client, 118 );
        return false;
    }

    bool ret = false;
    if (client->status == Client::TOINSTALL && client->pipe_to_child >= 0)
        ret = handle_file_chunk_env(client, msg);

    if (ret) {
        delete msg;
        return ret;
    }

    switch ( msg->type ) {
    case M_GET_NATIVE_ENV: ret = handle_get_native_env( client ); break;
    case M_COMPILE_FILE: ret = handle_compile_file( client, msg ); break;
    case M_TRANFER_ENV: ret = handle_transfer_env( client, msg ); break;
    case M_GET_CS: ret = handle_get_cs( client, msg ); break;
    case M_END: handle_end( client, 119 ); ret = false; break;
    case M_JOB_LOCAL_BEGIN: ret = handle_local_job (client, msg); break;
    case M_JOB_DONE: ret = handle_job_done( client, dynamic_cast<JobDoneMsg*>(msg) ); break;
    default:
        log_error() << "not compile: " << ( char )msg->type << "protocol error on client " << client->dump() << endl;
        client->channel->send_msg( EndMsg() );
        handle_end( client, 120 );
        ret = false;
    }
    delete msg;
    return ret;
}

int Daemon::answer_client_requests()
{
#ifdef ICECC_DEBUG
    if ( clients.size() + current_kids )
        log_info() << dump_internals() << endl;
    log_info() << "clients " << clients.dump_per_status() << " " << current_kids << " (" << max_kids << ")" << endl;

#endif

    /* reap zombis */
    int status;
    while (waitpid(-1, &status, WNOHANG) < 0 && errno == EINTR)
        ;

    handle_old_request();

    /* collect the stats after the children exited icecream_load */
    if ( scheduler )
        maybe_stats();

    fd_set listen_set;
    struct timeval tv;

    FD_ZERO( &listen_set );
    int max_fd = 0;
    if (tcp_listen_fd != -1) {
      FD_SET( tcp_listen_fd, &listen_set );
      max_fd = tcp_listen_fd;
    }
    FD_SET( unix_listen_fd, &listen_set );
    if (unix_listen_fd > max_fd) // very likely
      max_fd = unix_listen_fd;

    for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
         it != fd2chan.end();) {
        int i = it->first;
        MsgChannel *c = it->second;
        ++it;
        /* don't select on a fd that we're currently not interested in.
           Avoids that we wake up on an event we're not handling anyway */
        Client* client = clients.find_by_channel(c);
        assert(client);
        int current_status = client->status;
        bool ignore_channel = current_status == Client::TOCOMPILE ||
                              current_status == Client::WAITFORCHILD;
        if (!ignore_channel && (!c->has_msg() || handle_activity(client))) {
            if (i > max_fd)
                max_fd = i;
            FD_SET (i, &listen_set);
        }

        if (current_status == Client::WAITFORCHILD
            && client->pipe_to_child != -1) {
            if (client->pipe_to_child > max_fd)
                max_fd = client->pipe_to_child;
            FD_SET (client->pipe_to_child, &listen_set);
        }
    }

    if ( scheduler ) {
        FD_SET( scheduler->fd, &listen_set );
        if ( max_fd < scheduler->fd )
            max_fd = scheduler->fd;
    } else if ( discover && discover->listen_fd() >= 0) {
        /* We don't explicitely check for discover->get_fd() being in
	   the selected set below.  If it's set, we simply will return
	   and our call will make sure we try to get the scheduler.  */
        FD_SET( discover->listen_fd(), &listen_set);
	if ( max_fd < discover->listen_fd() )
	    max_fd = discover->listen_fd();
    }

    tv.tv_sec = max_scheduler_pong;
    tv.tv_usec = 0;

    int ret = select (max_fd + 1, &listen_set, NULL, NULL, &tv);
    if ( ret < 0 && errno != EINTR ) {
        log_perror( "select" );
        return 5;
    }

    if ( ret > 0 ) {
        bool had_scheduler = scheduler;
        if ( scheduler && FD_ISSET( scheduler->fd, &listen_set ) ) {
            while (!scheduler->read_a_bit() || scheduler->has_msg()) {
                Msg *msg = scheduler->get_msg();
                if ( !msg ) {
                    log_error() << "scheduler closed connection\n";
                    close_scheduler();
                    clear_children();
                    return 1;
                } else {
                    ret = 0;
                    switch ( msg->type )
                    {
                    case M_PING:
                        if (!IS_PROTOCOL_27(scheduler))
                            ret = !send_scheduler(PingMsg());
                        break;
                    case M_USE_CS:
                        ret = scheduler_use_cs( static_cast<UseCSMsg*>( msg ) );
                        break;
                    case M_GET_INTERNALS:
                        ret = scheduler_get_internals( );
                        break;
                    case M_CS_CONF:
                        ret = handle_cs_conf(static_cast<ConfCSMsg*>( msg ));
                        break;
                    default:
                        log_error() << "unknown scheduler type " << ( char )msg->type << endl;
                        ret = 1;
                    }
                }
                delete msg;
                if (ret)
                    return ret;
            }
        }

	int listen_fd = -1;
	if ( tcp_listen_fd != -1 && FD_ISSET( tcp_listen_fd, &listen_set ) )
	  listen_fd = tcp_listen_fd;
	if ( FD_ISSET( unix_listen_fd, &listen_set ) )
	  listen_fd = unix_listen_fd;

        if ( listen_fd != -1) {
            struct sockaddr cli_addr;
            socklen_t cli_len = sizeof cli_addr;
            int acc_fd = accept(listen_fd, &cli_addr, &cli_len);
            if (acc_fd < 0)
                log_perror("accept error");
            if (acc_fd == -1 && errno != EINTR) {
                log_perror("accept failed:");
                return EXIT_CONNECT_FAILED;
            } else {
                MsgChannel *c = Service::createChannel( acc_fd, &cli_addr, cli_len );
                if ( !c )
                    return 0;
                trace() << "accepted " << c->fd << " " << c->name << endl;

                Client *client = new Client;
                client->client_id = ++new_client_id;
                client->channel = c;
                clients[c] = client;

                fd2chan[c->fd] = c;
                while (!c->read_a_bit() || c->has_msg()) {
                    if (!handle_activity(client))
                        break;
                    if (client->status == Client::TOCOMPILE ||
                            client->status == Client::WAITFORCHILD)
                        break;
                }
            }
        } else {
            for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
                 max_fd && it != fd2chan.end();)  {
                int i = it->first;
                MsgChannel *c = it->second;
                Client* client = clients.find_by_channel(c);
                assert(client);
                ++it;
                if (client->status == Client::WAITFORCHILD
                    && client->pipe_to_child >= 0
                    && FD_ISSET(client->pipe_to_child, &listen_set) )
                {
                    max_fd--;
                    if (!handle_compile_done(client))
                        return 1;
                }

                if (FD_ISSET (i, &listen_set)) {
                    assert(client->status != Client::TOCOMPILE);
                    while (!c->read_a_bit() || c->has_msg()) {
                        if (!handle_activity(client))
                            break;
                        if (client->status == Client::TOCOMPILE ||
                                client->status == Client::WAITFORCHILD)
                            break;
                    }
                    max_fd--;
                }
            }
        }
        if ( had_scheduler && !scheduler ) {
            clear_children();
            return 2;
        }

    }
    return 0;
}

bool Daemon::reconnect()
{
    if ( scheduler )
        return true;

    if (!discover &&
        next_scheduler_connect > time(0)) {
        trace() << "timeout.." << endl;
        return false;
    }

#ifdef ICECC_DEBUG
    trace() << "reconn " << dump_internals() << endl;
#endif
    if (!discover
	|| discover->timed_out())
    {
        delete discover;
	discover = new DiscoverSched (netname, max_scheduler_pong, schedname);
    }

    scheduler = discover->try_get_scheduler ();
    if ( !scheduler ) {
        log_warning() << "scheduler not yet found.\n";
        return false;
    }
    delete discover;
    discover = 0;
    sockaddr_in name;
    socklen_t len = sizeof(name);
    int error = getsockname(scheduler->fd, (struct sockaddr*)&name, &len);
    if ( !error )
        remote_name = inet_ntoa( name.sin_addr );
    else
        remote_name = string();
    log_info() << "Connected to scheduler (I am known as " << remote_name << ")\n";
    current_load = -1000;
    gettimeofday( &last_stat, 0 );
    icecream_load = 0;
    
    LoginMsg lmsg( PORT, determine_nodename(), machine_name );
    lmsg.envs = available_environmnents(envbasedir);
    lmsg.max_kids = max_kids;
    lmsg.noremote = noremote;
    return send_scheduler ( lmsg );
}

int Daemon::working_loop()
{
    for (;;) {
        reconnect();

        int ret = answer_client_requests();
        if ( ret ) {
            trace() << "answer_client_requests returned " << ret << endl;
            close_scheduler();
        }
    }
    // never really reached
    return 0;
}

int main( int argc, char ** argv )
{
    int max_processes = -1;
    srand( time( 0 ) + getpid() );

    Daemon d;

    int debug_level = Error;
    string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h

    while ( true ) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, NULL, 'n' },
            { "max-processes", 1, NULL, 'm' },
            { "help", 0, NULL, 'h' },
            { "daemonize", 0, NULL, 'd'},
            { "log-file", 1, NULL, 'l'},
            { "nice", 1, NULL, 0},
            { "name", 1, NULL, 'n'},
            { "scheduler-host", 1, NULL, 's' },
            { "env-basedir", 1, NULL, 'b' },
            { "nobody-uid", 1, NULL, 'u'},
            { "cache-limit", 1, NULL, 0},
            { "no-remote", 0, NULL, 0},
            { 0, 0, 0, 0 }
        };

        const int c = getopt_long( argc, argv, "N:n:m:l:s:whvdrb:u:", long_options, &option_index );
        if ( c == -1 ) break; // eoo

        switch ( c ) {
        case 0:
        {
            string optname = long_options[option_index].name;
            if ( optname == "nice" ) {
                if ( optarg && *optarg ) {
                    errno = 0;
                    int tnice = atoi( optarg );
                    if ( !errno )
                        nice_level = tnice;
                } else
                    usage("Error: --nice requires argument");
            } else if ( optname == "name" ) {
                if ( optarg && *optarg )
                    d.nodename = optarg;
                else
                    usage("Error: --name requires argument");
            } else if ( optname == "cache-limit" ) {
                if ( optarg && *optarg ) {
                    errno = 0;
                    int mb = atoi( optarg );
                    if ( !errno )
                        cache_size_limit = mb * 1024 * 1024;
                }
                else
                    usage("Error: --cache-limit requires argument");
            } else if ( optname == "no-remote" ) {
                d.noremote = true;
            }

        }
        break;
        case 'd':
            detach = true;
            break;
        case 'N':
            if ( optarg && *optarg )
                d.nodename = optarg;
            else
                usage("Error: -N requires argument");
            break;
        case 'l':
            if ( optarg && *optarg )
                logfile = optarg;
            else
                usage( "Error: -l requires argument" );
            break;
        case 'v':
            if ( debug_level & Warning )
                if ( debug_level & Info ) // for second call
                    debug_level |= Debug;
                else
                    debug_level |= Info;
            else
                debug_level |= Warning;
            break;
        case 'n':
            if ( optarg && *optarg )
                d.netname = optarg;
            else
                usage("Error: -n requires argument");
            break;
        case 'm':
            if ( optarg && *optarg )
                max_processes = atoi(optarg);
            else
                usage("Error: -m requires argument");
            break;
        case 's':
            if ( optarg && *optarg )
                d.schedname = optarg;
            else
                usage("Error: -s requires hostname argument");
            break;
        case 'b':
            if ( optarg && *optarg )
                d.envbasedir = optarg;
            break;
        case 'u':
            if ( optarg && *optarg )
            {
                struct passwd *pw = getpwnam( optarg );
                if ( !pw ) {
                    usage( "Error: -u requires a valid username" );
                } else {
                    d.nobody_uid = pw->pw_uid;
                    d.nobody_gid = pw->pw_gid;
                    if (!d.nobody_gid || !d.nobody_uid) {
                        usage( "Error: -u <username> must not be root");
                    }
                }
            } else
                usage( "Error: -u requires a valid username" );
            break;

        default:
            usage();
        }
    }

    umask(022);

    if ( !logfile.length() && detach)
        logfile = "/var/log/iceccd";

    setup_debug( debug_level, logfile );

    if ((getuid()!=0))
        d.noremote = true;

    log_info() << "ICECREAM daemon " VERSION " starting up (nice level "
               << nice_level << ") " << endl;

    d.determine_system();

    chdir( "/" );

    if ( detach )
        if (daemon(0, 0)) {
            log_perror("daemon()");
            exit (EXIT_DISTCC_FAILED);
        }

    if (dcc_ncpus(&d.num_cpus) == 0)
        log_info() << d.num_cpus << " CPU(s) online on this server" << endl;

    if ( max_processes < 0 )
        max_kids = d.num_cpus;
    else
        max_kids = max_processes;

    log_info() << "allowing up to " << max_kids << " active jobs\n";

    int ret;

    /* Still create a new process group, even if not detached */
    trace() << "not detaching\n";
    if ((ret = set_new_pgrp()) != 0)
        return ret;

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        exit( EXIT_DISTCC_FAILED );
    }

    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGCHLD) failed: " << strerror(errno) << endl;
        exit( EXIT_DISTCC_FAILED );
    }

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    ofstream pidFile;
    string progName = argv[0];
    progName = progName.substr(progName.rfind('/')+1);
    pidFilePath = string(RUNDIR)+string("/")+progName+string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << dcc_master_pid << endl;
    pidFile.close();

    if ( !cleanup_cache( d.envbasedir ) )
        return 1;

    list<string> nl = get_netnames (200);
    trace() << "Netnames:" << endl;
    for (list<string>::const_iterator it = nl.begin(); it != nl.end(); ++it)
        trace() << *it << endl;

    if ( !d.setup_listen_fds() ) // error
        return 1;

    return d.working_loop();
}

