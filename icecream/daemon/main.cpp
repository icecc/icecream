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

#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"

int dcc_sockaddr_to_PATHstring(struct sockaddr *sa,
                               char *p_buf)
{
    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sain = (struct sockaddr_in *) sa;

        snprintf(p_buf, PATH_MAX, "%s:%d", inet_ntoa(sain->sin_addr),
                  ntohs(sain->sin_port));
    } else if (sa->sa_family == AF_UNIX) {
        /* NB: The word 'sun' is predefined on Solaris */
        struct sockaddr_un *sa_un = (struct sockaddr_un *) sa;
        snprintf(p_buf, PATH_MAX, "UNIX-DOMAIN %s", sa_un->sun_path);
    } else {
        snprintf(p_buf, PATH_MAX, "UNKNOWN-FAMILY %d", sa->sa_family);
    }

    return 0;
}

/**
 * Listen on a predetermined address (often the passive address).  The way in
 * which we get the address depends on the resolver API in use.
 **/
static int dcc_listen_by_addr(int *listen_fd,
                              struct sockaddr *sa,
                              size_t salen)
{
    int one = 1;
    int fd;
    char sa_buf[PATH_MAX];

    fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd == -1) {
	rs_log_error("socket creation failed: %s\n", strerror(errno));
	return EXIT_BIND_FAILED;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one));

    dcc_sockaddr_to_PATHstring(sa, sa_buf);

    /* now we've got a socket - we need to bind it */
    if (bind(fd, sa, salen) == -1) {
	rs_log_error("bind of %s failed: %s\n", sa_buf, strerror(errno));
	close(fd);
	return EXIT_BIND_FAILED;
    }

    rs_log_info("listening on %s\n", sa_buf);

    if (listen(fd, 10)) {
        rs_log_error("listen failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_BIND_FAILED;
    }

    *listen_fd = fd;
    return 0;

}

/* This version uses inet_aton */
int dcc_socket_listen(int port, int *listen_fd)
{
    struct sockaddr_in sock;

    if (port < 1 || port > 65535) {
        /* htons() will truncate, not check */
        rs_log_error("port number out of range: %d\n", port);
        return EXIT_BAD_ARGUMENTS;
    }

    memset((char *) &sock, 0, sizeof(sock));
    sock.sin_port = htons(port);
    sock.sin_family = PF_INET;
    sock.sin_addr.s_addr = INADDR_ANY;

    return dcc_listen_by_addr(listen_fd, (struct sockaddr *) &sock, sizeof sock);
}

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
        rs_trace("already a process group leader\n");
        return 0;
    }

    if (setpgid(0, 0) == 0) {
        rs_trace("entered process group\n");
        return 0;
    } else {
        rs_trace("setpgid(0, 0) failed: %s\n", strerror(errno));
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

/**
 * Just log, remove pidfile, and exit.
 *
 * Called when a daemon gets a fatal signal.
 *
 * Some cleanup is done only if we're the master/parent daemon.
 **/
static void dcc_daemon_terminate(int whichsig)
{
    int am_parent = getpid() == dcc_master_pid;

    if (am_parent) {
#ifdef HAVE_STRSIGNAL
        rs_log_info("%s\n", strsignal(whichsig));
#else
        rs_log_info("terminated by signal %d\n", whichsig);
#endif
    }

    /* Make sure to remove handler before re-raising signal, or
     * Valgrind gets its kickers in a knot. */
    signal(whichsig, SIG_DFL);

    // dcc_cleanup_tempfiles();

    if (am_parent) {
        // dcc_remove_pid();

        /* kill whole group */
        kill(0, whichsig);
    }

    raise(whichsig);
}


int main( int argc, char **argv )
{
    int listen_fd;
    int n_cpus;
    int ret;

    if ((ret = dcc_socket_listen(7000, &listen_fd)) != 0)
        return ret;

    set_cloexec_flag(listen_fd, 1);

    if (dcc_ncpus(&n_cpus) == 0)
        rs_log_info("%d CPU%s online on this server\n", n_cpus, n_cpus == 1 ? "" : "s");

    /* By default, allow one job per CPU, plus two for the pot.  The extra
     * ones are started to allow for a bit of extra concurrency so that the
     * machine is not idle waiting for disk or network IO. */
    int dcc_max_kids = 2 + n_cpus;

    rs_log_info("allowing up to %d active jobs\n", dcc_max_kids);

    /* Still create a new process group, even if not detached */
    rs_trace("not detaching\n");
    if ((ret = dcc_new_pgrp()) != 0)
        return ret;
    // dcc_save_pid(getpid());

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    while (1) {
        int acc_fd;
        struct sockaddr cli_addr;
        socklen_t cli_len;

        rs_log_info("waiting to accept connection\n");

        cli_len = sizeof cli_addr;
        acc_fd = accept(listen_fd, &cli_addr, &cli_len);
        if (acc_fd == -1 && errno == EINTR) {
            ;
        }  else if (acc_fd == -1) {
            rs_log_error("accept failed: %s\n", strerror(errno));
            return EXIT_CONNECT_FAILED;
        } else {

            /* Log client name and check access if appropriate.  For ssh connections
             * the client comes from a localhost socket. */
            // if ((ret = dcc_check_client(cli_addr, cli_len)) != 0)
            // return ret;

            if ( ( ret = run_job(acc_fd, acc_fd) ) != 0 )
                return ret; // return is most likely not the best :/

            // dcc_cleanup_tempfiles();
            if (close(acc_fd) != 0) {
                rs_log_error("failed to close fd%d: %s\n", acc_fd, strerror(errno));
                return EXIT_IO_ERROR;
            }
        }
    }
}
