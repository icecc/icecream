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
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

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

#include <arpa/inet.h>

#ifdef HAVE_RESOLV_H
#  include <resolv.h>
#endif
#include <netdb.h>

#ifdef __FreeBSD__
#include <signal.h> // for kill(2)
#include <sys/time.h>
#include <sys/resource.h>
#define RUSAGE_SELF (0)
#define RUSAGE_CHILDREN (-1)
#endif

#include <deque>
#include <map>
#include <algorithm>
#include <ext/hash_set>
#include <set>

#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"
#include "logging.h"
#include <comm.h>
#include "load.h"
#include "environment.h"

const int PORT = 10245;

using namespace std;
using namespace __gnu_cxx; // for the extensions we like, e.g. hash_set

typedef pair<CompileJob*, MsgChannel*> Compile_Request;
// the pidmap maps the PID to the socket to the server child
typedef map<pid_t, int> Pidmap;

struct UseCsCache {
    UseCSMsg *msg;
    MsgChannel *client;
};

struct LocalJobCache {
    int job_id;
    string outfile;
    MsgChannel *client;
};

int set_cloexec_flag (int desc, int value)
{
    int oldflags = fcntl (desc, F_GETFD, 0);
    /* If reading the flags failed, return error indication now. */
    if (oldflags < 0)
        return oldflags;
    /* Set just the flag we want to set. */
    if (value != 0)
        oldflags |= FD_CLOEXEC;
    else
        oldflags &= ~FD_CLOEXEC;
    /* Store modified flag word in the descriptor. */
    return fcntl (desc, F_SETFD, oldflags);
}

int dcc_new_pgrp(void)
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
    signal(SIGHUP, &dcc_daemon_terminate);
    signal(SIGALRM, &dcc_daemon_terminate);
}

pid_t dcc_master_pid;

MsgChannel *scheduler = 0;

/**
 * Just log, remove pidfile, and exit.
 *
 * Called when a daemon gets a fatal signal.
 *
 * Some cleanup is done only if we're the master/parent daemon.
 **/
static void dcc_daemon_terminate(int whichsig)
{
    bool am_parent = ( getpid() == dcc_master_pid );
    printf( "term %d %d %p\n", whichsig, am_parent, scheduler );

    if (am_parent) {
#ifdef HAVE_STRSIGNAL
        log_info() << strsignal(whichsig) << endl;
#else
        log_info() << "terminated by signal " << whichsig << endl;
#endif
    }

    /* Make sure to remove handler before re-raising signal, or
     * Valgrind gets its kickers in a knot. */
    signal(whichsig, SIG_DFL);

    // dcc_cleanup_tempfiles();

    if (am_parent) {
        if ( scheduler ) {
            scheduler->send_msg( EndMsg() ); /// TODO: what happens if it's already in send_msg?
            scheduler = 0;
        }

        /* kill whole group */
        kill(0, whichsig);
    }

    raise(whichsig);
}

void empty_func( int )
{
}

void usage(const char* reason = 0)
{
  if (reason)
     cerr << reason << endl;

  cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [-w] [-d|--daemonize] [-l logfile] [-s <schedulerhost>] [-v[v[v]]] [-r|--run-as-user] [-b <env-basedir>] [-u|--nobody-uid <nobody_uid>] [--cache-limit <MB>] [-N <node_name>]" << endl;
  exit(1);
}

void reannounce_environments(const string &envbasedir, const string &nodename)
{
    LoginMsg lmsg( 0, nodename, "");
    lmsg.envs = available_environmnents(envbasedir);
    scheduler->send_msg( lmsg );
}

int setup_listen_fd()
{
    int listen_fd;
    if ((listen_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror ("socket()");
        return -1;
    }

    int optval = 1;
    if (setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror ("setsockopt()");
        return -1;
    }

    int count = 5;
    while ( count ) {
        struct sockaddr_in myaddr;
        myaddr.sin_family = AF_INET;
        myaddr.sin_port = htons (PORT);
        myaddr.sin_addr.s_addr = INADDR_ANY;
        if (bind (listen_fd, (struct sockaddr *) &myaddr,
                  sizeof (myaddr)) < 0) {
            log_perror ("bind()");
            sleep( 2 );
            if ( !--count )
                return -1;
            continue;
        } else
            break;
    }

    if (listen (listen_fd, 20) < 0)
    {
      log_perror ("listen()");
      return -1;
    }

    set_cloexec_flag(listen_fd, 1);

    return listen_fd;
}


struct timeval last_stat;
struct timeval last_sent;
int mem_limit = 100;
unsigned int max_kids = 0;
int current_kids = 0;
unsigned long myniceload = 0;
unsigned long myidleload = 0;

size_t cache_size_limit = 100 * 1024 * 1024;

bool maybe_stats(bool force = false) {
    struct timeval now;
    gettimeofday( &now, 0 );

    time_t diff_stat = ( now.tv_sec - last_stat.tv_sec ) * 1000 + ( now.tv_usec - last_stat.tv_usec ) / 1000;
    time_t diff_sent = ( now.tv_sec - last_sent.tv_sec ) * 1000 + ( now.tv_usec - last_sent.tv_usec ) / 1000;

    unsigned int memory_fillgrade;
    unsigned long niceLoad = 0;
    unsigned long idleLoad = 0;

    if ( diff_sent > 6000 || force ) {
        StatsMsg msg;

        if ( !fill_stats( niceLoad, idleLoad, memory_fillgrade, &msg ) )
            return false;

        gettimeofday( &last_stat, 0 );

        myniceload += niceLoad * diff_stat;
        myidleload += idleLoad * diff_stat;

        unsigned int realLoad = diff_sent ? ( 1000 - myidleload / diff_sent ) : 1000;
        msg.load = ( 700 * realLoad + 300 * memory_fillgrade ) / 1000;
        if ( memory_fillgrade > 600 )
            msg.load = 1000;
        if ( realLoad > 800 )
            msg.load = 1000;

        //trace() << "load load=" << realLoad << " mem=" << memory_fillgrade << endl;

        // Matz got in the urine that not all CPUs are always feed
        mem_limit = std::max( msg.freeMem / std::min( std::max( max_kids, 1U ), 4U ), 100U );

        if ( !scheduler->send_msg( msg ) )
            return false;
        last_sent = now;
        myidleload = 0;
        myniceload = 0;
    } else {

        fill_stats( niceLoad, idleLoad, memory_fillgrade, 0);
        myniceload += niceLoad * diff_stat;
        if ( max_kids )
            myidleload += max( 0LU, niceLoad * diff_stat * current_kids / max_kids );
        myidleload += idleLoad * diff_stat;

        gettimeofday( &last_stat, 0 );
    }

    return true;
}

void fill_msg(int fd, JobDoneMsg *msg)
{
    read( fd, &msg->in_compressed, sizeof( unsigned int ) );
    read( fd, &msg->in_uncompressed, sizeof( unsigned int ) );
    read( fd, &msg->out_compressed, sizeof( unsigned int ) );
    read( fd, &msg->out_uncompressed, sizeof( unsigned int ) );
    read( fd, &msg->real_msec, sizeof( unsigned int ) );
    if ( msg->out_uncompressed && !msg->in_uncompressed ) { // this is typical for client jobs
        unsigned int job_id;
        read( fd, &job_id, sizeof( unsigned int ) );
        if ( job_id != msg->job_id )
            log_error() << "the job ids for the client job do not match: " << job_id << " " << msg->job_id << endl;
        read( fd, &msg->user_msec, sizeof( unsigned int ) );
        read( fd, &msg->sys_msec, sizeof( unsigned int ) );
        read( fd, &msg->pfaults, sizeof( unsigned int ) );
    }
}

struct Daemon
{
    deque<Compile_Request> requests;
    map<pid_t, JobDoneMsg*> jobmap;
    map<pid_t, string> envmap;
    Pidmap pidmap;
    map<string, time_t> envs_last_use;
    string native_environment;
    string envbasedir;
    uid_t nobody_uid;
    int listen_fd;
    string machine_name;
    string nodename;
    size_t cache_size;
    map<int, MsgChannel *> fd2chan;
    map<int, MsgChannel *> pending_clients;
    deque<LocalJobCache> waiting_local_jobs;
    // like link jobs.  Exactly those which start
    // with a JobLocalBeginMsg
    map<MsgChannel*,LocalJobCache> active_local_jobs;
    int new_client_id;
    string remote_name;
    deque<UseCsCache> pending_use_cs;

    Daemon() {
        envbasedir = "/tmp/icecc-envs";
        nobody_uid = 65534;
        listen_fd = -1;
        new_client_id = 0;
    }
    int answer_client_requests();
    void transfer_env( MsgChannel *c, Msg *msg );
    void get_native_env( MsgChannel *c );
    int handle_old_request();
    void compile_file( MsgChannel *c, Msg *msg );
    int handle_activity( MsgChannel *c );
    void handle_end( MsgChannel *&c );
    int handle_get_internals( MsgChannel *c );
    void clear_children();
    int handle_use_cs( UseCSMsg *msg );
    void handle_get_cs( MsgChannel *c, Msg *msg );
    void handle_local_job( MsgChannel *c, Msg *msg );
    string dump_internals() const;
};

string Daemon::dump_internals() const
{
    string result;
    result += "Node Name: " + nodename + "\n";
    for (deque<Compile_Request>::const_iterator it = requests.begin();
	 it != requests.end(); ++it) {
        CompileJob *job = it->first;
        MsgChannel *c = it->second;
        result += "  Request from " + c->dump() + ": " + job->inputFile() + " - " + toString( job->jobID() ) + "\n";
    }
    result += "  Remote name: " + remote_name + "\n";
    for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
         it != fd2chan.end(); ++it)  {
        result += "  fd2chan[" + toString( it->first ) + "] =" + it->second->dump() + "\n";
    }
    for (map<int, MsgChannel *>::const_iterator it = pending_clients.begin();
         it != pending_clients.end(); ++it)  {
        result += "pending_clients[" + toString( it->first ) + "] =" +
                  it->second->dump() + "\n";
    }
    if ( cache_size )
        result += "  Cache Size: " + toString( cache_size ) + "\n";
    result += "  Architecture: " + machine_name + "\n";
    if ( !native_environment.empty() )
        result += "  NativeEnv: " + native_environment + "\n";

    for (map<MsgChannel*, LocalJobCache>::const_iterator it = active_local_jobs.begin();
         it != active_local_jobs.end(); ++it)  {
        const LocalJobCache ljc = it->second;
        result += "  active_local_jobs[" + it->first->dump() + "]=" +
                  ljc.outfile + "(" + toString( ljc.job_id ) + ")\n";
    }
    if ( !envs_last_use.empty() )
        result += "  Now: " + toString( time( 0 ) ) + "\n";
    for (map<string, time_t>::const_iterator it = envs_last_use.begin();
         it != envs_last_use.end(); ++it)  {
        result += "  envs_last_use[" + it->first  + "] =" +
                  toString( it->second ) + "\n";
    }
    for ( deque<LocalJobCache>::const_iterator it = waiting_local_jobs.begin();
          it != waiting_local_jobs.end(); ++it ) {
        result += "  Waiting LJ: " + it->outfile + "(" + toString( it->job_id ) + ")\n";
    }
    for ( map<pid_t, JobDoneMsg*>::const_iterator it = jobmap.begin();
          it != jobmap.end(); ++it ) {
        result += string( "jobmap[" ) + toString( it->first ) + "] = " + toString( it->second ) + "\n";
        if ( pidmap.count( it->first ) > 0 ) {
            result += "  pidmap[" + toString( it->first ) + "] = ";
            // pidmap[it->first] is non-const
            result += toString( pidmap.find( it->first )->second ) + "\n";
        }
        if ( envmap.count( it->first ) > 0 ) {
            result += "  envmap[" + toString( it->first ) + "] = " + envmap.find( it->first )->second + "\n";
        }
    }

    for ( Pidmap::const_iterator it = pidmap.begin();
          it != pidmap.end(); ++it ) {
        if ( jobmap.count( it->first ) > 0 )
            continue;
        result += "  pidmap[" + toString( it->first ) + "] = " + toString( it->second ) + "\n";
    }

    for ( map<pid_t, string>::const_iterator it = envmap.begin();
          it != envmap.end(); ++it ) {
        if ( jobmap.count( it->first ) > 0 )
            continue;
        result += "  envmap[" + toString( it->first ) + "] = " + it->second + "\n";
    }

    return result;
}

int Daemon::handle_get_internals( MsgChannel *c )
{
    trace() << "handle_get_internals " << dump_internals() << endl;
    c->send_msg( StatusTextMsg( dump_internals() ) );
    return 0;
}

int Daemon::handle_use_cs( UseCSMsg *msg )
{
    MsgChannel *c = pending_clients[msg->client_id];
    trace() << "handle_use_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " << msg->hostname << " " << remote_name <<  endl;
    if ( !c ) {
        scheduler->send_msg( JobDoneMsg( msg->job_id ) );
        return 1;
    }
    if ( msg->hostname == remote_name ) {
        UseCsCache ucc;
        ucc.msg = new UseCSMsg( msg->host_platform, "127.0.0.1", msg->port, msg->job_id, true, 1 );
        ucc.client = c;
        pending_use_cs.push_back( ucc );
    } else
        c->send_msg( *msg, true );
    return 0;
}

void Daemon::transfer_env( MsgChannel *c, Msg *msg )
{
    EnvTransferMsg *emsg = static_cast<EnvTransferMsg*>( msg );
    string target = emsg->target;
    if ( target.empty() )
        target =  machine_name;
    size_t installed_size = install_environment( envbasedir, emsg->target, emsg->name, c, nobody_uid );
    if (!installed_size) {
        trace() << "install environment failed" << endl;
        c->send_msg(EndMsg()); // shut up, we had an error
        reannounce_environments(envbasedir, nodename);
    } else {
        cache_size += installed_size;
        string current = emsg->target + "/" + emsg->name;
        envs_last_use[current] = time( NULL );
        trace() << "installed " << emsg->name << " size: " << installed_size
                << " all: " << cache_size << endl;

        time_t now = time( NULL );
        while ( cache_size > cache_size_limit ) {
            string oldest;
            // I don't dare to use (time_t)-1
            time_t oldest_time = time( NULL ) + 90000;
            for ( map<string, time_t>::const_iterator it = envs_last_use.begin();
                  it != envs_last_use.end(); ++it ) {
                trace() << "das ist jetzt so: " << it->first << " " << it->second << " " << oldest_time << endl;
                // ignore recently used envs (they might be in use _right_ now
                if ( it->second < oldest_time && now - it->second < 100 ) {
                    bool found = false;
                    for (map<pid_t,string>::const_iterator it2 = envmap.begin(); it2 != envmap.end(); ++it2)
                        if (it2->second == it->first)
                            found = true;
                    if (!found) {
                        oldest_time = it->second;
                        oldest = it->first;
                    }
                }
            }
            if ( oldest.empty() || oldest == current )
                break;
            cache_size -= min( remove_environment( envbasedir, oldest, nobody_uid ), cache_size );
            envs_last_use.erase( oldest );
        }

        reannounce_environments(envbasedir, nodename); // do that before the file compiles
        msg = c->get_msg();
        if ( msg && msg->type == M_COMPILE_FILE ) { // we sure hope so
            CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->takeJob();
            requests.push_back( make_pair( job, c ));
        } else {
            log_error() << "not compile file\n";
        }
	delete msg;
    }
}

void Daemon::get_native_env( MsgChannel *c )
{
    trace() << "get_native_env " << native_environment << endl;

    if ( !native_environment.length() ) {
        size_t installed_size = setup_env_cache( envbasedir, native_environment, nobody_uid );
        // we only clean out cache on next target install
        cache_size += installed_size;
        if ( ! installed_size ) {
            c->send_msg( EndMsg() );
            throw( 1 );
        }
    }
    UseNativeEnvMsg m( native_environment );
    c->send_msg( m );
}

int Daemon::handle_old_request()
{
    if ( !waiting_local_jobs.empty() && (current_kids+active_local_jobs.size()) < max_kids)
    {
        LocalJobCache ljc = *waiting_local_jobs.begin();
        waiting_local_jobs.erase( waiting_local_jobs.begin() );
        if (!ljc.client->send_msg (JobLocalBeginMsg())) {
	    log_warning() << "can't send start message to client" << endl;
	    handle_end (ljc.client);
        } else {
            active_local_jobs[ljc.client] = ljc;
            trace() << "pushed local job " << ljc.job_id << endl;
            scheduler->send_msg( JobLocalBeginMsg( ljc.job_id, ljc.outfile ) );
	}
    }
    if ( !pending_use_cs.empty() && (current_kids+active_local_jobs.size()) < max_kids )
    {
        UseCsCache ucc = pending_use_cs.front();
        pending_use_cs.pop_front();
        ucc.client->send_msg( *ucc.msg, true );
        delete ucc.msg;
        /* in this time the client has to find the msg and hit the maybe_build_local, so
         * he's better quick */
        Msg *compile = ucc.client->get_msg( 5 );
        trace() << "pending_use_cs-- " << ucc.msg->job_id <<  " " << compile << endl;
        if ( !compile || compile->type != M_COMPILE_FILE )
        {
            handle_end( ucc.client );
            return 0;
        }
        compile_file( ucc.client, compile );
	delete compile;
    }

    if ( !requests.empty() && (current_kids+active_local_jobs.size()) < max_kids ) {
        Compile_Request req = requests.front();
        requests.pop_front();
        CompileJob *job = req.first;
        int sock = -1;
        pid_t pid = -1;

        trace() << "requests--" << job->jobID() << endl;

        if ( job->environmentVersion() == "__client" ) {
            int sockets[2];
            if (pipe(sockets)) {
                log_error() << "pipe can't be created " << strerror( errno ) << endl;
                exit( 1 );
            }
            sock = sockets[0];
            // if the client compiles, we fork off right away
            pid = fork();
            if ( pid == 0 ) {
                close( sockets[0] );
                Msg *msg = req.second->get_msg(12 * 60); // wait forever
                if ( !msg )
                    ::exit( 1 );
                if ( msg->type != M_JOB_DONE )
                    ::exit( 0 ); // without further notice
                JobDoneMsg *jdmsg = static_cast<JobDoneMsg*>( msg );
                unsigned int dummy = 0;
                write( sockets[1], &dummy, sizeof( unsigned int ) ); // in_compressed
                write( sockets[1], &dummy, sizeof( unsigned int ) ); // in_uncompressed
                write( sockets[1], &dummy, sizeof( unsigned int ) ); // out_compressed
                write( sockets[1], &jdmsg->out_uncompressed, sizeof( unsigned int ) );
                write( sockets[1], &jdmsg->real_msec, sizeof( unsigned int ) );
                // the rest are additional information for client jobs
                write( sockets[1], &jdmsg->job_id, sizeof( unsigned int ) );
                write( sockets[1], &jdmsg->user_msec, sizeof( unsigned int ) );
                write( sockets[1], &jdmsg->sys_msec, sizeof( unsigned int ) );
                write( sockets[1], &jdmsg->pfaults, sizeof( unsigned int ) );
                ::exit( jdmsg->exitcode );
            } else
                close( sockets[1] );
        } else {
            string envforjob = job->targetPlatform() + "/" + job->environmentVersion();
            envs_last_use[envforjob] = time( NULL );
            pid = handle_connection( envbasedir, req.first, req.second, sock, mem_limit, nobody_uid );
            envmap[pid] = envforjob;
        }

        if ( pid > 0) { // forked away
            current_kids++;
            trace() << "sending scheduler about " << job->jobID() << endl;
            if ( !scheduler || !scheduler->send_msg( JobBeginMsg( job->jobID() ) ) ) {
                log_warning() << "can't reach scheduler to tell him about job start of "
                              << job->jobID() << endl;
                delete req.first;
                handle_end( req.second );
                return 2;
            }
            jobmap[pid] = new JobDoneMsg;
            jobmap[pid]->job_id = job->jobID();
            if ( sock > -1 )
                pidmap[pid] = sock;
        }
        delete req.first;
        handle_end( req.second );
    }
    struct rusage ru;
    int status;
    pid_t child = wait3(&status, WNOHANG, &ru);
    if ( child > 0 ) {
        JobDoneMsg *msg = jobmap[child];
        if ( msg )
            current_kids--;
        else
            log_error() << "catched child pid " << child << " not in my map\n";
        jobmap.erase( child );
        Pidmap::iterator pid_it = pidmap.find( child );
        if ( pid_it != pidmap.end() ) {
            fill_msg( pid_it->second, msg );
            close( pid_it->second );
            pidmap.erase( pid_it );
        }
        if ( msg && scheduler ) {
            msg->exitcode = WEXITSTATUS( status );
            if ( !msg->user_msec ) { // if not already set
                msg->user_msec = ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000;
                msg->sys_msec = ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
                msg->pfaults = ru.ru_majflt + ru.ru_nswap + ru.ru_minflt;
            }
            scheduler->send_msg( *msg );
        }
        envs_last_use[envmap[child]] = time( NULL );
        envmap.erase(child);
        delete msg;
    }
    return 0;
}

void Daemon::compile_file( MsgChannel *c, Msg *msg )
{
    CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->takeJob();
    requests.push_back( make_pair( job, c ));
}

void Daemon::handle_end( MsgChannel *&c )
{
    trace() << "handle_end " << c << endl;
    trace() << dump_internals() << endl;
    fd2chan.erase (c->fd);
    for (map<int, MsgChannel *>::iterator it = pending_clients.begin();
         it != pending_clients.end(); ++it) {
        if ( it->second == c ) {
            pending_clients.erase( it );
            break;
        }
    }
    for ( deque<LocalJobCache>::iterator it = waiting_local_jobs.begin();
	 it != waiting_local_jobs.end(); ++it)
	if (it->client == c) {
		waiting_local_jobs.erase(it);
		break;
	}

    for ( deque<Compile_Request>::iterator it = requests.begin();
         it != requests.end(); ++it)
        if (it->second == c) {
                requests.erase(it);
                if ( scheduler )
                    scheduler->send_msg( JobDoneMsg( it->first->jobID() ) );
                break;
        }

    if (active_local_jobs.count (c) > 0) {
        LocalJobCache ljc = active_local_jobs[c];
	trace() << "was a local job" << endl;
	active_local_jobs.erase (c);
        scheduler->send_msg( JobLocalDoneMsg( ljc.job_id ) );
    }

    delete c;
    c = 0;
}

void Daemon::clear_children()
{
    while ( !requests.empty() ) {
        Compile_Request req = requests.front();
        requests.pop_front();
        delete req.first;
        handle_end( req.second );
    }

    while ( current_kids > 0 ) {
        int status;
        pid_t child = wait(&status);
        current_kids--;
        if ( child > 0 ) {
            jobmap.erase( child );
            Pidmap::iterator pid_it = pidmap.find( child );
            if ( pid_it != pidmap.end() ) {
                close( pid_it->second );
                pidmap.erase( pid_it );
            }
        }
    }

    jobmap.clear();
    pidmap.clear();
    pending_clients.clear();

    while ( !fd2chan.empty() )
        handle_end( fd2chan.begin()->second );

    fd2chan.clear();
    new_client_id = 0;
    trace() << "cleared children\n";
}

void Daemon::handle_get_cs( MsgChannel *c, Msg *msg )
{
    GetCSMsg *umsg = dynamic_cast<GetCSMsg*>( msg );
    umsg->client_id = ++new_client_id;
    trace() << "handle_get_cs " << umsg->client_id << endl;
    scheduler->send_msg( *umsg, false );
    pending_clients[new_client_id] = c;
}

void Daemon::handle_local_job( MsgChannel *c, Msg *msg )
{
    trace() << "handle_local_job " << c << endl;
    LocalJobCache ljc;
    ljc.outfile = dynamic_cast<JobLocalBeginMsg*>( msg )->outfile;
    ljc.job_id = ++new_client_id;
    ljc.client = c;
    waiting_local_jobs.push_back( ljc );
}

int Daemon::handle_activity( MsgChannel *c )
{
    Msg *msg = c->get_msg();
    if ( !msg ) {
        log_error() << "no message\n";
        handle_end( c );
        return 0;
    }
    try {
        log_warning() << "msg " << ( char )msg->type << endl;
        switch ( msg->type ) {
        case M_GET_NATIVE_ENV: get_native_env( c ); break;
        case M_COMPILE_FILE: compile_file( c, msg ); break;
        case M_TRANFER_ENV: transfer_env( c, msg ); break;
        case M_GET_CS: handle_get_cs( c, msg ); break;
        case M_END: handle_end( c ); break;
	case M_JOB_LOCAL_BEGIN: handle_local_job (c, msg); break;
        default:
            log_error() << "not compile: " << ( char )msg->type << endl;
            c->send_msg( EndMsg() );
            handle_end( c );
        }
    } catch ( int ret ) {
        delete msg;
        return ret;
    }
    delete msg;
    return 0;
}

int Daemon::answer_client_requests()
{
    int acc_fd;
    struct sockaddr cli_addr;
    socklen_t cli_len;

    if ( requests.size() + current_kids + active_local_jobs.size() + waiting_local_jobs.size() )  {
        log_info() << "puscs " << pending_use_cs.size()
                   << " requests " << requests.size() << " kids "
                   << current_kids << " locals " << active_local_jobs.size()
		   << " waiting " << waiting_local_jobs.size()
		   << " (max " << max_kids << ")\n";
    }

    cache_size = 0;

    const int max_count = 0; // DEBUG
    int count = 0; // DEBUG

    int ret = handle_old_request();
    if ( ret )
        return ret;

    if ( !maybe_stats() ) {
        log_error() << "lost connection to scheduler. Trying again.\n";
        return 2;
    }

    fd_set listen_set;
    struct timeval tv;

    FD_ZERO( &listen_set );
    FD_SET( listen_fd, &listen_set );
    int max_fd = listen_fd;

    for ( Pidmap::const_iterator it = pidmap.begin(); it != pidmap.end(); ++it ) {
        FD_SET( it->second, &listen_set );
        if ( max_fd < it->second )
            max_fd = it->second;
    }

    for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
         it != fd2chan.end();)  {
        int i = it->first;
        MsgChannel *c = it->second;
        bool ok = true;
        ++it;
        /* handle_activity() can delete c and make the iterator
           invalid.  XXX handle_activity and friends were copied from
	   the scheduler.  This needs a big cleanup for the return values,
	   currently this either leaks or segfaults.  That's why we need
	   to check 'ok' and 'c'.  The latter test must go away.  */
        while (ok && c && c->has_msg ())
            if (handle_activity (c) != 0)
                ok = false;
        if (ok && c)  {
            if (i > max_fd)
                max_fd = i;
            FD_SET (i, &listen_set);
        }
    }

    FD_SET( scheduler->fd, &listen_set );
    if ( max_fd < scheduler->fd )
        max_fd = scheduler->fd;

    tv.tv_sec = 0;
    tv.tv_usec = 400000;

    ret = select (max_fd + 1, &listen_set, NULL, NULL, &tv);
    if ( ret == -1 && errno != EINTR ) {
        log_perror( "select" );
        return 5;
    }

    if ( ret > 0 ) {
        if ( FD_ISSET( scheduler->fd, &listen_set ) ) {
            Msg *msg = scheduler->get_msg();
            if ( !msg ) {
                log_error() << "no message from scheduler\n";
                return 1;
            } else {
                ret = 0;
                switch ( msg->type )
                {
                case M_PING:
                    if ( !maybe_stats(true) )
                        ret = 1;
                    break;
                case M_USE_CS:
                    ret = handle_use_cs( dynamic_cast<UseCSMsg*>( msg ) );
		    break;
                case M_GET_INTERNALS:
                    ret = handle_get_internals( scheduler );
                    break;
                default:
                    log_error() << "unknown scheduler type " << ( char )msg->type << endl;
                }
            }
            delete msg;
            return ret;
        }

        if ( FD_ISSET( listen_fd, &listen_set ) ) {
            cli_len = sizeof cli_addr;
            acc_fd = accept(listen_fd, &cli_addr, &cli_len);
            if (acc_fd == -1 && errno != EINTR) {
                log_perror("accept failed:");
                return EXIT_CONNECT_FAILED;
            } else {
                MsgChannel *c = Service::createChannel( acc_fd, (struct sockaddr*) &cli_addr, cli_len );
                if ( !c )
                    return 0;
                trace() << "accept " << c->fd << " " << c->name << endl;

                fd2chan[c->fd] = c;
                if (!c->read_a_bit () || c->has_msg ())
                    handle_activity (c);

                if ( max_count && ++count > max_count ) {
                    cout << "I'm closing now. Hoping you used valgrind! :)\n";
                    exit( 0 );
                }
            }
        } else {
            for ( Pidmap::iterator it = pidmap.begin(); it != pidmap.end(); ++it ) {
                if ( FD_ISSET( it->second, &listen_set ) ) {
                    JobDoneMsg *msg = jobmap[it->first];
                    if ( msg ) {
                        fill_msg( it->second, msg );
                        close( it->second );
                        pidmap.erase( it );
                        break;
                    }
                }
            }
            for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
                 max_fd && it != fd2chan.end();)  {
                int i = it->first;
                MsgChannel *c = it->second;

                /* handle_activity can delete the channel from the fd2chan list,
                   hence advance the iterator right now, so it doesn't become
                   invalid.  */
                ++it;
                if (FD_ISSET (i, &listen_set)) {
                    if (!c->read_a_bit () || c->has_msg ())
                        handle_activity (c);
                    max_fd--;
                }
            }
        }
    }
    return 0;
}


int main( int argc, char ** argv )
{
    int max_processes = -1;

    Daemon d;
    string netname;

    int debug_level = Error;
    string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h
    string schedname;
    bool runasuser = false;

    gettimeofday( &last_stat, 0 );
    last_sent = last_stat;
    last_sent.tv_sec -= 6;

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
            { "run-as-user", 1, NULL, 'r' },
            { "env-basedir", 1, NULL, 'b' },
            { "nobody-uid", 1, NULL, 'u'},
            { "cache-limit", 1, NULL, 0},
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
                    netname = optarg;
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
                    schedname = optarg;
                else
                    usage("Error: -s requires hostname argument");
                break;
            case 'b':
                if ( optarg && *optarg )
                    d.envbasedir = optarg;
                break;
            case 'r':
                runasuser = true;
                break;
            case 'u':
                if ( optarg && *optarg )
                {
                    struct passwd *pw = getpwnam( optarg );
                    if ( !pw ) {
                        usage( "Error: -u requires a valid username" );
                    } else
                        d.nobody_uid = pw->pw_uid;
                } else
                    usage( "Error: -u requires a valid username" );
                break;

            default:
                usage();
        }
    }

    if ( !logfile.length() && detach)
        logfile = "/var/log/iceccd";

    setup_debug( debug_level, logfile );

    if ((geteuid()!=0) && !runasuser)
    {
        log_error() << "Please run iceccd with root privileges" << endl;
        return 1;
    }

    log_info() << "will use nice level " << nice_level << endl;

    struct utsname uname_buf;
    if ( uname( &uname_buf ) ) {
        log_perror( "uname call failed" );
        return 1;
    }

    if ( !d.nodename.length() )
        d.nodename = uname_buf.nodename;

    d.machine_name = uname_buf.machine;

    chdir( "/" );

    if ( detach )
        daemon(0, 0);

    int n_cpus;
    if (dcc_ncpus(&n_cpus) == 0)
        log_info() << n_cpus << " CPU(s) online on this server" << endl;

    if ( max_processes < 0 )
      max_kids = n_cpus;
    else
      max_kids = max_processes;

    log_info() << "allowing up to " << max_kids << " active jobs\n";

    int ret;

    /* Still create a new process group, even if not detached */
    trace() << "not detaching\n";
    if ((ret = dcc_new_pgrp()) != 0)
        return ret;

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        exit( EXIT_DISTCC_FAILED );
    }

    /* Setup the SIGCHLD handler.  Make sure we mark it as not restarting
       some syscalls.  The loop below depends on the fact, that select
       returns when a SIGCHLD arrives.  */
    struct sigaction act;
    sigemptyset( &act.sa_mask );

    act.sa_handler = empty_func;
    act.sa_flags = SA_NOCLDSTOP;
    sigaction( SIGCHLD, &act, 0 );

    sigaddset( &act.sa_mask, SIGCHLD );
    // Make sure we don't block this signal. gdb tends to do that :-(
    sigprocmask( SIG_UNBLOCK, &act.sa_mask, 0 );

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    if ( !cleanup_cache( d.envbasedir, d.nobody_uid ) )
        return 1;

    list<string> nl = get_netnames (200);
    trace() << "Netnames:" << endl;
    for (list<string>::const_iterator it = nl.begin(); it != nl.end(); ++it)
      trace() << *it << endl;

    int tosleep = 0;

    while ( 1 ) {
        if ( d.listen_fd > -1 ) {
            // as long as we have no scheduler, don't listen for clients
            shutdown( d.listen_fd, SHUT_RDWR ); // Dirk's suggestion
            close( d.listen_fd );
            d.listen_fd = -1;
        }

        if ( tosleep )
            sleep( tosleep );

        tosleep = 0;

        if ( !scheduler ) {
            d.clear_children();

            trace() << "connect_scheduler\n";
            scheduler = connect_scheduler (netname, 2000, schedname);
            if ( !scheduler ) {
                log_warning() << "no scheduler found. Sleeping.\n";
                tosleep = 1;
                continue;
            }
            sockaddr_in name;
            socklen_t len = sizeof(name);
            int error = getsockname(scheduler->fd, (struct sockaddr*)&name, &len);
            if ( !error )
                d.remote_name = inet_ntoa( name.sin_addr );
            else
                d.remote_name = string();
        }

        d.listen_fd = setup_listen_fd();
        if ( d.listen_fd == -1 ) // error
            return 1;

        trace() << "login as " << uname_buf.machine << endl;
        LoginMsg lmsg( PORT, d.nodename, d.machine_name );
        lmsg.envs = available_environmnents(d.envbasedir);
        lmsg.max_kids = max_kids;
        scheduler->send_msg( lmsg );

        while (true) {
            int ret = d.answer_client_requests();
            if ( ret ) {
                trace() << "answer_client_requests returned " << ret << endl;
                tosleep = ret;
                break;
            }
        }

        delete scheduler;
        scheduler = 0;

    }
}
