#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <time.h>
#include <netdb.h>
#include <string>
#include <iostream>
#include "job.h"
#include "comm.h"

using namespace std;
#define START_PORT 10245

class Client : public Service {
public:
  Client (struct sockaddr *addr, socklen_t len) : Service (addr, len) {}
};

Service *scheduler;
MsgChannel *sched_channel;

static void
open_scheduler ()
{
  int remote_fd;
  struct sockaddr_in remote_addr;
  if ((remote_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      exit (1);
    }
  struct hostent *host = gethostbyname ("localhost");
  if (!host)
    {
      fprintf (stderr, "Unknown host\n");
      exit (1);
    }
  if (host->h_length != 4)
    {
      fprintf (stderr, "Invalid address length\n");
      exit (1);
    }
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons (8765);
  memcpy (&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
  if (connect (remote_fd, (struct sockaddr *) &remote_addr, sizeof (remote_addr)) < 0)
    {
      perror ("connect()");
      exit (1);
    }
  scheduler = new Service ((struct sockaddr*) &remote_addr, sizeof (remote_addr));
  sched_channel = new MsgChannel (remote_fd, scheduler);
}

static int
handle_compile_file (MsgChannel *c, Msg *_m)
{
  CompileFileMsg *m = dynamic_cast<CompileFileMsg *>(_m);
  CompileJob *job = m->job;
  cout << "compiling Job " << job->jobID() << ":" << endl;
  const list<string> &l = job->remoteFlags();
  for (list<string>::const_iterator it = l.begin(); it != l.end(); ++it)
    cout << *it << " " << endl;
  const list<string> &l2 = job->restFlags();
  for (list<string>::const_iterator it = l2.begin(); it != l2.end(); ++it)
    cout << *it << " " << endl;
  delete job;
  m->job = 0;
  return 0;
}

static int
handle_connection (MsgChannel *c)
{
  Msg *m;
  int ret = 0;
  while ((m = c->get_msg ()) && m->type != M_END)
    {
      switch (m->type)
        {
	case M_COMPILE_FILE: ret = handle_compile_file (c, m); break;
	case M_TIMEOUT: break;
	default: ret = 1; break;
	}
      delete m;
      if (ret)
        break;
    }
  delete m;
  return ret;
}

int main (int , char *[])
{
  int fd, remote_fd;
  struct sockaddr_in myaddr, remote_addr;
  socklen_t remote_len;
  if ((fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      return 1;
    }
  int optval = 1;
  if (setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
      perror ("setsockopt()");
      return 1;
    }
  for (int port = START_PORT; port < START_PORT + 10; port++)
    {
      myaddr.sin_family = AF_INET;
      myaddr.sin_port = htons (port);
      myaddr.sin_addr.s_addr = INADDR_ANY;
      if (bind (fd, (struct sockaddr *) &myaddr, sizeof (myaddr)) < 0)
        {
	  if (errno == EADDRINUSE && port < START_PORT + 9)
	    continue;
          perror ("bind()");
          return 1;
        }
      break;
    }
  if (listen (fd, 20) < 0)
    {
      perror ("listen()");
      return 1;
    }
  while (1)
    {
      remote_len = sizeof (remote_addr);
      if ((remote_fd = accept (fd, (struct sockaddr *) &remote_addr, &remote_len)) < 0)
	{
	  perror ("accept()");
	  return 1;
	}
      Client *client = new Client ((struct sockaddr*) &remote_addr, remote_len);
      MsgChannel *c = new MsgChannel (remote_fd, client);
      handle_connection (c);
      delete c;
      delete client;
    }
  delete sched_channel;
  delete scheduler;
  return 0;
}
