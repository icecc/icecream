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


#include <queue>
#include <map>
#include <algorithm>

#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"
#include "logging.h"
#include <comm.h>
#include "load.h"
#include "environment.h"
#include "findmyself.h"

const int PORT = 10245;

using namespace std;

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

  cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [-w] [-d|--daemonize] [-l logfile] [-s <schedulerhost>] [-v[v[v]]] [-u|--run-as-user] [-b <env-basedir>]" << endl;
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
        perror ("socket()");
        return -1;
    }

    int optval = 1;
    if (setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror ("setsockopt()");
        return -1;
    }

    struct sockaddr_in myaddr;
    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons (PORT);
    myaddr.sin_addr.s_addr = INADDR_ANY;
    if (bind (listen_fd, (struct sockaddr *) &myaddr,
              sizeof (myaddr)) < 0) {
        perror ("bind()");
        return -1;
    }

    if (listen (listen_fd, 20) < 0)
    {
      perror ("listen()");
      return -1;
    }

    set_cloexec_flag(listen_fd, 1);

    return listen_fd;
}

time_t last_stat = 0;
int mem_limit = 100;
int max_kids = 0;

bool maybe_stats() {
    if ( time( 0 ) - last_stat >= 6 ) {
        trace() << "sending stats\n";

        StatsMsg msg;
        if ( !fill_stats( msg ) ) {
            log_error() << "can't find out stats" << endl;
        } else { // Matz got in the urin that not all CPUs are always feed
            mem_limit = std::max( msg.freeMem / std::max( max_kids, 4 ), 100U );
        }

        if ( scheduler->send_msg( msg ) )
            last_stat = time( 0 );
        else {
            return false;
        }

    }
    return true;
}

int main( int argc, char ** argv )
{
    int max_processes = -1;

    string netname;
    bool watch_binary = false;
    string envbasedir = "/tmp/icecc-envs";
    int debug_level = Error;
    string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h
    string nodename;
    string schedname;
    bool runasuser = false;

    while ( true ) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, NULL, 'n' },
            { "max-processes", 1, NULL, 'm' },
            { "watch", 0, NULL, 'w' },
            { "help", 0, NULL, 'h' },
            { "daemonize", 0, NULL, 'd'},
            { "log-file", 1, NULL, 'l'},
            { "nice", 1, NULL, 0},
            { "name", 1, NULL, 'n'},
            { "scheduler-host", 1, NULL, 's' },
            { "run-as-user", 1, NULL, 'u' },
            { "env-basedir", 1, NULL, 'b' },
            { 0, 0, 0, 0 }
        };

        const int c = getopt_long( argc, argv, "n:m:l:s:whvdub:", long_options, &option_index );
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
                           nodename = optarg;
                       else
                           usage("Error: --name requires argument");
                   }
               }
               break;
            case 'd':
                detach = true;
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
            case 'w':
                watch_binary = true;
                break;
            case 's':
                if ( optarg && *optarg )
                    schedname = optarg;
                else
                    usage("Error: -s requires hostname argument");
                break;
            case 'b':
                if ( optarg && *optarg )
                    envbasedir = optarg;
                break;
            case 'u':
                runasuser = true;
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
        perror( "uname call failed" );
        return 1;
    }

    if ( !nodename.length() )
        nodename = uname_buf.nodename;

    std::string binary_path = argv[0];
    time_t binary_on_startup = 0;
    if ( watch_binary ) {
        // important to do before chdir ;/
        if ( !findmyself( binary_path ) ) {
            log_error() << "can't find binary " << argv[0] << endl;
            return 1;
        }
        trace() << "watching " << binary_path << endl;
        struct stat st;
        stat( binary_path.c_str(), &st );
        binary_on_startup = st.st_mtime;
    }

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

    const int max_count = 0; // DEBUG
    int count = 0; // DEBUG
    typedef pair<CompileJob*, MsgChannel*> Compile_Request;
    queue<Compile_Request> requests;
    map<pid_t, JobDoneMsg*> jobmap;
    typedef map<pid_t, int> Pidmap;
    Pidmap pidmap;

    string native_environment;
    if ( !cleanup_cache( envbasedir ) )
        return 1;

    list<string> nl = get_netnames (200);
    trace() << "Netnames:" << endl;
    for (list<string>::const_iterator it = nl.begin(); it != nl.end(); ++it)
      trace() << *it << endl;

    int listen_fd = -1;
    int tosleep = 0;

    while ( 1 ) {
        if ( listen_fd > -1 ) {
            // as long as we have no scheduler, don't listen for clients
            close( listen_fd );
            listen_fd = -1;
        }

        if ( tosleep )
            sleep( tosleep );

        tosleep = 0;

        if ( !scheduler ) {
            trace() << "connect_scheduler\n";
            scheduler = connect_scheduler (netname, 2000, schedname);
            if ( !scheduler ) {
                log_warning() << "no scheduler found. Sleeping.\n";
                tosleep = 1;
                continue;
            }
            jobmap.clear();
        }

        listen_fd = setup_listen_fd();
        if ( listen_fd == -1 ) // error
            return 1;

        trace() << "login as " << uname_buf.machine << endl;
        LoginMsg lmsg( PORT, nodename, uname_buf.machine );
        lmsg.envs = available_environmnents(envbasedir);
        lmsg.max_kids = max_kids;
        scheduler->send_msg( lmsg );

        // TODO: clean up the mess from before
        // for now I just hope schedulers don't go up
        // and down that often
        int current_kids = 0;

        while ( !requests.empty() )
            requests.pop();

        while (1) {
            int acc_fd;
            struct sockaddr cli_addr;
            socklen_t cli_len;

            if ( requests.size() + current_kids )
                log_info() << "requests " << requests.size() << " "
                           << current_kids << " (" << max_kids << ")\n";
            if ( !requests.empty() && current_kids < max_kids ) {
                Compile_Request req = requests.front();
                requests.pop();
                CompileJob *job = req.first;
                int sock = -1;
                pid_t pid = -1;

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
                        write( sockets[1], &jdmsg->maxrss, sizeof( unsigned int ) );
                        write( sockets[1], &jdmsg->idrss, sizeof( unsigned int ) );
                        write( sockets[1], &jdmsg->majflt, sizeof( unsigned int ) );
                        write( sockets[1], &jdmsg->nswap, sizeof( unsigned int ) );

                        ::exit( jdmsg->exitcode );
                    } else
                        close( sockets[1] );
                } else
                    pid = handle_connection( envbasedir, req.first, req.second, sock, mem_limit );


                if ( pid > 0) { // forks away
                    current_kids++;
                    if ( !scheduler || !scheduler->send_msg( JobBeginMsg( job->jobID() ) ) ) {
                        log_warning() << "can't reach scheduler to tell him about job start of "
                                    << job->jobID() << endl;
                        tosleep = 2;
                        delete req.first;
                        delete req.second;
                        break;
                    }
                    jobmap[pid] = new JobDoneMsg;
                    jobmap[pid]->job_id = job->jobID();
                    if ( sock > -1 )
                        pidmap[pid] = sock;
                }
                delete req.first;
                delete req.second;
            }
            struct rusage ru;
            int status;
            pid_t child = wait3(&status, WNOHANG, &ru);
            if ( child > 0 ) {
                JobDoneMsg *msg = jobmap[child];
                if ( msg )
                    current_kids--;
                jobmap.erase( child );
                Pidmap::iterator pid_it = pidmap.find( child );
                if ( pid_it != pidmap.end() ) {
                    close( pid_it->second );
                    pidmap.erase( pid_it );
                }
                if ( msg && scheduler ) {
                    msg->exitcode = status;
                    if ( !msg->user_msec ) { // if not already set
                        msg->user_msec = ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000;
                        msg->sys_msec = ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
                        msg->maxrss = (ru.ru_maxrss + 1023) / 1024;
                        msg->idrss = (ru.ru_idrss + 1023) / 1024;
                        msg->majflt = ru.ru_majflt;
                        msg->nswap = ru.ru_nswap;
                    }
                    scheduler->send_msg( *msg );
                }
                delete msg;
                continue;
            }

            if (  time( 0 ) - last_stat >= 10 && watch_binary ) {
                struct stat st;
                ::stat( binary_path.c_str(), &st );
                if ( binary_on_startup != st.st_mtime ) {
                    log_info() << "binary changed. Going to restart it" << endl;
                    char **new_argv = new char *[netname.empty() ? 3 : 5];
                    int argi = 0;
                    new_argv[argi++] = strdup( binary_path.c_str() );
                    new_argv[argi++] = strdup( "-w" );
                    if ( !netname.empty() ) {
                        new_argv[argi++] = strdup( "-n" );
                        new_argv[argi++] = strdup( netname.c_str() );
                    }
                    new_argv[argi++] = 0;
                    execv( binary_path.c_str(), new_argv );
                }
            }

            if ( !maybe_stats() ) {
                log_error() << "lost connection to scheduler. Trying again.\n";
                delete scheduler;
                scheduler = 0;
                tosleep = 2;
                break;
            }


            fd_set listen_set;
            struct timeval tv;

            FD_ZERO (&listen_set);
            FD_SET (listen_fd, &listen_set);
            int max_fd = listen_fd;

            for ( Pidmap::const_iterator it = pidmap.begin(); it != pidmap.end(); ++it ) {
                FD_SET( it->second, &listen_set );
                if ( max_fd < it->second )
                    max_fd = it->second;
            }

            FD_SET( scheduler->fd, &listen_set );
            if ( max_fd < scheduler->fd )
                max_fd = scheduler->fd;

            tv.tv_sec = 2;
            tv.tv_usec = 0;

            ret = select (max_fd + 1, &listen_set, NULL, NULL, &tv);

            if ( ret > 0 ) {
                if ( FD_ISSET( scheduler->fd, &listen_set ) ) {
                    Msg *msg = scheduler->get_msg();
                    if ( !msg ) {
                        log_error() << "no message from scheduler\n";
                        delete scheduler;
                        scheduler = 0;
                        tosleep = 1;
                        break;
                     } else {
                        if ( msg->type == M_PING ) {
                            StatsMsg smsg;
                            if ( !fill_stats( smsg ) )
                                continue;

                            if ( scheduler->send_msg( smsg ) )
                                last_stat = time( 0 );
                        } else
                            log_error() << "unknown scheduler type " << ( char )msg->type << endl;
                    }
                    continue;
                }

                if ( FD_ISSET( listen_fd, &listen_set ) ) {
                    cli_len = sizeof cli_addr;
                    acc_fd = accept(listen_fd, &cli_addr, &cli_len);
                    if (acc_fd == -1 && errno != EINTR) {
                        log_error() << "accept failed: " << strerror(errno) << endl;
                        return EXIT_CONNECT_FAILED;
                    } else {
                        Service *client = new Service ((struct sockaddr*) &cli_addr, cli_len);
                        MsgChannel *c = client->createChannel( acc_fd );
                        if ( !c->protocol ) { // protocol mismatch
                            delete client;
                            sleep( 1 );
                            continue;
                        }

                        Msg *msg = c->get_msg();
                        if ( !msg ) {
                            log_error() << "no message?\n";
                        } else {
                            if ( msg->type == M_GET_SCHEDULER ) {
                                GetSchedulerMsg *gsm = dynamic_cast<GetSchedulerMsg*>( msg );
                                if ( scheduler && gsm ) {
                                    if ( gsm->wants_native && !native_environment.length() )
                                        if ( !setup_env_cache( envbasedir, native_environment ) ) {
                                            c->send_msg( EndMsg() );
                                            delete scheduler;
                                            return 1;
                                        }
                                    UseSchedulerMsg m( scheduler->other_end->name,
                                                       scheduler->other_end->port,
                                                       native_environment );
                                    c->send_msg( m );
                                } else {
                                    c->send_msg( EndMsg() );
                                }
                            } else if ( msg->type == M_COMPILE_FILE ) {
                                CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->takeJob();
                                requests.push( make_pair( job, c ));
                                client = 0; // forget you saw him
                            } else if ( msg->type == M_TRANFER_ENV ) {
                                EnvTransferMsg *emsg = dynamic_cast<EnvTransferMsg*>( msg );
                                string target = emsg->target;
                                if ( target.empty() )
                                    target =  uname_buf.machine;
                                if (!install_environment( envbasedir, emsg->target, emsg->name, c )) {
                                    trace() << "install environment failed" << endl;
                                    c->send_msg(EndMsg()); // shut up, we had an error
                                    reannounce_environments(envbasedir, nodename);
				} else {
                                    reannounce_environments(envbasedir, nodename); // do that before the file compiles
                                    delete msg;
                                    msg = c->get_msg();
                                    if ( msg->type == M_COMPILE_FILE ) { // we sure hope so
                                        CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->takeJob();
                                        requests.push( make_pair( job, c ));
                                        client = 0; // forget you saw him
                                    } else {
                                        log_error() << "not compile file\n";
                                    }
                                }
                            } else
                                log_error() << "not compile: " << ( char )msg->type << endl;
                            delete msg;
                        }
                        delete client;

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
                                read( it->second, &msg->in_compressed, sizeof( unsigned int ) );
                                read( it->second, &msg->in_uncompressed, sizeof( unsigned int ) );
                                read( it->second, &msg->out_compressed, sizeof( unsigned int ) );
                                read( it->second, &msg->out_uncompressed, sizeof( unsigned int ) );
                                read( it->second, &msg->real_msec, sizeof( unsigned int ) );
                                if ( msg->out_uncompressed && !msg->in_uncompressed ) { // this is typical for client jobs
                                    unsigned int job_id;
                                    read( it->second, &job_id, sizeof( unsigned int ) );
                                    if ( job_id != msg->job_id )
                                        log_error() << "the job ids for the client job do not match: " << job_id << " " << msg->job_id << endl;
                                    read( it->second, &msg->user_msec, sizeof( unsigned int ) );
                                    read( it->second, &msg->sys_msec, sizeof( unsigned int ) );
                                    read( it->second, &msg->maxrss, sizeof( unsigned int ) );
                                    read( it->second, &msg->idrss, sizeof( unsigned int ) );
                                    read( it->second, &msg->majflt, sizeof( unsigned int ) );
                                    read( it->second, &msg->nswap, sizeof( unsigned int ) );
                                }
                                close( it->second );
                                pidmap.erase( it );
                                break;
                            }
                        }
                    }
                }
            }
        }
        delete scheduler;
        scheduler = 0;
    }
}
