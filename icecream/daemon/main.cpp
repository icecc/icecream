/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
 * icecc main daemon file
 *
 * GPL...
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#ifdef HAVE_ARPA_NAMESER_H
#  include <arpa/nameser.h>
#endif

#include <arpa/inet.h>

#ifdef HAVE_RESOLV_H
#  include <resolv.h>
#endif
#include <netdb.h>
#include <queue>
#include <map>
#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"
#include "logging.h"
#include <comm.h>
#include "environment.h"

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



int main( int /*argc*/, char ** /*argv*/ )
{
    list<string> environments = available_environmnents();

    const int START_PORT = 10245;

    // daemon(0, 0);

    int listen_fd;
    if ((listen_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0) {
        perror ("socket()");
        return 1;
    }

    int optval = 1;
    if (setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror ("setsockopt()");
        return 1;
    }

    struct sockaddr_in myaddr;
    int port = START_PORT;
    for ( ; port < START_PORT + 10; port++) {
        myaddr.sin_family = AF_INET;
        myaddr.sin_port = htons (port);
        myaddr.sin_addr.s_addr = INADDR_ANY;
        if (bind (listen_fd, (struct sockaddr *) &myaddr,
                  sizeof (myaddr)) < 0) {
            if (errno == EADDRINUSE && port < START_PORT + 9)
                continue;
            perror ("bind()");
            return 1;
        }
        break;
    }

    if (listen (listen_fd, 20) < 0)
    {
      perror ("listen()");
      return 1;
    }

    set_cloexec_flag(listen_fd, 1);

    int n_cpus;
    if (dcc_ncpus(&n_cpus) == 0)
        log_info() << n_cpus << " CPUs online on this server" << endl;

    /* By default, allow one job per CPU, plus two for the pot.  The extra
     * ones are started to allow for a bit of extra concurrency so that the
     * machine is not idle waiting for disk or network IO. */
    int max_kids = n_cpus + 2;

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

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    const int max_count = 0; // DEBUG
    int count = 0; // DEBUG
    typedef pair<CompileJob*, MsgChannel*> Compile_Request;
    queue<Compile_Request> requests;
    map<pid_t, unsigned int> jobmap;

    while ( 1 ) {
        if ( !scheduler ) {
            scheduler = connect_scheduler ();
            if ( !scheduler ) {
                log_error() << "no scheduler found. Sleeping.\n";
                sleep( 1 );
                continue;
            }
        }

        LoginMsg lmsg( port );
        lmsg.envs = environments;
        lmsg.max_kids = max_kids;
        lmsg.max_load = n_cpus * 1500;
        scheduler->send_msg( lmsg );

        // TODO: clean up the mess from before
        // for now I just hope schedulers don't go up
        // and down that often
        int current_kids = 0;
        time_t last_stat = 0;
        while ( !requests.empty() )
            requests.pop();

        while (1) {
            int acc_fd;
            struct sockaddr cli_addr;
            socklen_t cli_len;

            if ( requests.size() + current_kids )
                log_info() << "requests " << requests.size() << " " << current_kids << " (" << max_kids << ")\n";
            if ( !requests.empty() && current_kids < max_kids ) {
                Compile_Request req = requests.front();
                requests.pop();
                CompileJob *job = req.first;
                pid_t pid = handle_connection( req.first, req.second );
                if ( pid > 0) { // forks away
                    current_kids++;
                    if ( !scheduler || !scheduler->send_msg( JobBeginMsg( job->jobID() ) ) ) {
                        log_error() << "can't reach scheduler to tell him about job start of " << job->jobID() << endl;
                        delete scheduler;
                        scheduler = 0;
                        delete req.first;
                        delete req.second;
                        break;
                    }
                    jobmap[pid] = job->jobID();
                }
                delete req.first;
                delete req.second;
            }
            struct rusage ru;
            int status;
            pid_t child = wait3(&status, WNOHANG, &ru);
            if ( child > 0 ) {
                log_info() << "one child got " << status << endl;
                current_kids--;
                int job_id = jobmap[child];
                if ( job_id && scheduler )
                    scheduler->send_msg( JobDoneMsg( job_id ) );
                continue;
            }

            if ( time( 0 ) - last_stat >= 2 ) {
                StatsMsg msg;
                if (getloadavg (msg.load, 3) == -1) {
                    log_error() << "getloadavg failed: " << strerror( errno ) << endl;
                    msg.load[0] = msg.load[1] = msg.load[2] = 0;
                }
                if ( scheduler->send_msg( msg ) )
                    last_stat = time( 0 );
                else {
                    log_error() << "lost connection to scheduler. Trying again.\n";
                    delete scheduler;
                    scheduler = 0;
                    break;
                }
            }

            fd_set listen_set;
            struct timeval tv;

            FD_ZERO (&listen_set);
            FD_SET (listen_fd, &listen_set);
            FD_SET (listen_fd, &listen_set);
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            ret = select (listen_fd + 1, &listen_set, NULL, NULL, &tv);

            if ( ret > 0 ) {
                cli_len = sizeof cli_addr;
                acc_fd = accept(listen_fd, &cli_addr, &cli_len);
                if (acc_fd == -1 && errno == EINTR) {
                    ;
                }  else if (acc_fd == -1) {
                    log_error() << "accept failed: " << strerror(errno) << endl;
                    return EXIT_CONNECT_FAILED;
                } else {
                    Service *client = new Service ((struct sockaddr*) &cli_addr, cli_len);
                    MsgChannel *c = client->createChannel( acc_fd );

                    Msg *msg = c->get_msg();
                    if ( !msg ) {
                        log_error() << "no message?\n";
                    } else {
                        if ( msg->type == M_GET_SCHEDULER ) {
                            if ( scheduler ) {
                                UseSchedulerMsg m( scheduler->other_end->name, scheduler->other_end->port );
                                c->send_msg( m );
                            } else {
                                c->send_msg( EndMsg() );
                            }
                        } else if ( msg->type == M_COMPILE_FILE ) {
                            CompileJob *job = dynamic_cast<CompileFileMsg*>( msg )->takeJob();
                            requests.push( make_pair( job, c ));
                            client = 0; // forget you saw him
                        } else
                            log_error() << "not compile\n";
                        delete msg;
                    }
                    delete client;

                    if ( max_count && ++count > max_count ) {
                        cout << "I'm closing now. Hoping you used valgrind! :)\n";
                        exit( 0 );
                    }
                }
            }
        }
        delete scheduler;
        scheduler = 0;
    }
}
