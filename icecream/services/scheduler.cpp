#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <string>
#include <list>
#include <map>
#include <algorithm>
#include "comm.h"

using namespace std;

class CS;

class Job {
public:
  unsigned int id;
  enum {PENDING, COMPILING} state;
  const CS *server;
  time_t starttime;  // _local_ to the compiler server
  time_t start_on_scheduler;  // starttime local to scheduler
  Job (const CS *cs, unsigned int _id) : id(_id), state(PENDING), server(cs),
    starttime(0), start_on_scheduler(0) {}
};

/* One compile server (receiver, compile daemon)  */
class CS : public Service {
public:
  unsigned int id;
  double load;
  unsigned int jobs_done;
  unsigned long long rcvd_kb, sent_kb;
  unsigned int ms_per_job;
  unsigned int bytes_per_ms;
  unsigned int max_jobs;
  time_t uptime;  // time connected with scheduler
  // Hmm, mutable is necessary, so that Job->server->joblist.remove() works
  // Maybe a compiler error?  Although server ist "const CS*", that doesn't
  // make server->joblist const, does it?
  mutable list<Job*> joblist;
  list<string> compiler_versions;  // Available compilers
  enum {AVAILABLE, DISCONNECTED} state;
  CS (struct sockaddr *_addr, socklen_t _len) : Service (_addr, _len) {}
};

// A subset of connected_hosts representing the compiler servers
list<CS*> css;
unsigned int new_job_id;
map<unsigned int, Job*> jobs;
map<int, MsgChannel *> fd2chan;

static bool
create_new_job (CS *cs)
{
  ++new_job_id;
  if (jobs.find(new_job_id) != jobs.end())
    return false;
  Job *job = new Job (cs, new_job_id);
  jobs[new_job_id] = job;
  cs->joblist.push_back (job);
  return true;
}

static int
handle_cs_request (MsgChannel *c, Msg *_m)
{
  GetCSMsg *m = dynamic_cast<GetCSMsg *>(_m);
  if (!m)
    return 1;
  // XXX select a nice CS
  // For now: compile it yourself
  list<CS*>::iterator it;
  for (it = css.begin(); it != css.end(); ++it)
    if (c->other_end->eq_ip (**it))
      break;
  if (it == css.end())
    {
      fprintf (stderr, "Asking host not connected\n");
      return 1;
    }
  CS *cs = *it;
  if (!create_new_job (cs))
    return 1;
  UseCSMsg m2(cs->name, 10245, new_job_id);
  EndMsg m3;
  if (!c->send_msg (m2)
      || !c->send_msg (m3))
    return 1;
  return 0;
}

static int
handle_login (MsgChannel *c, Msg *_m)
{
  LoginMsg *m = dynamic_cast<LoginMsg *>(_m);
  if (!m)
    return 1;
  CS *cs = static_cast<CS *>(c->other_end);
  css.push_back (cs);
  fd2chan[c->fd] = c;
  return 0;
}

static int
handle_job_begin (MsgChannel *c, Msg *_m)
{
  JobBeginMsg *m = dynamic_cast<JobBeginMsg *>(_m);
  if (jobs.find(m->job_id) == jobs.end())
    return 1;
  if (jobs[m->job_id]->server != c->other_end)
    return 1;
  jobs[m->job_id]->state = Job::COMPILING;
  jobs[m->job_id]->starttime = m->stime;
  jobs[m->job_id]->start_on_scheduler = time(0);
  return 0;
}

static int
handle_job_done (MsgChannel *c, Msg *_m)
{
  JobDoneMsg *m = dynamic_cast<JobDoneMsg *>(_m);
  if (jobs.find(m->job_id) == jobs.end())
    return 1;
  if (jobs[m->job_id]->server != c->other_end)
    return 1;
  Job *j = jobs[m->job_id];
  j->server->joblist.remove (j);
  jobs.erase (m->job_id);
  delete j;
  return 0;
}

static int
handle_ping (MsgChannel * /*c*/, Msg * /*_m*/)
{
  return 0;
}

static int
handle_stats (MsgChannel * /*c*/, Msg * /*_m*/)
{
  return 0;
}

static int
handle_timeout (MsgChannel * /*c*/, Msg * /*_m*/)
{
  return 1;
}

// Return 1 if some error occured, leaves C open.  */
static int
handle_new_connection (MsgChannel *c)
{
  Msg *m;
  int ret = 0;
  m = c->get_msg ();
  if (!m)
    return 1;
  switch (m->type)
    {
    case M_GET_CS:
      ret = handle_cs_request (c, m);
      delete c->other_end;
      delete c;
      break;
    case M_LOGIN: ret = handle_login (c, m); break;
    default:
      ret = 1;
      delete c->other_end;
      delete c;
      break;
    }
  delete m;
  return ret;
}

static int
handle_end (MsgChannel *c, Msg *)
{
  fd2chan.erase (c->fd);
  css.remove (static_cast<CS*>(c->other_end));
  delete c->other_end;
  delete c;
  return 0;
}

static int
handle_activity (MsgChannel *c)
{
  Msg *m;
  int ret = 0;
  m = c->get_msg ();
  if (!m)
    {
      handle_end (c, m);
      return 1;
    }
  switch (m->type)
    {
    case M_JOB_BEGIN: ret = handle_job_begin (c, m); break;
    case M_JOB_DONE: ret = handle_job_done (c, m); break;
    case M_PING: ret = handle_ping (c, m); break;
    case M_STATS: ret = handle_stats (c, m); break;
    case M_END: ret = handle_end (c, m); break;
    case M_TIMEOUT: ret = handle_timeout (c, m); break;
    default: ret = 1; break;
    }
  delete m;
  return ret;
}

int
main (int /*argc*/, char * /*argv*/ [])
{
  int listen_fd, remote_fd;
  struct sockaddr_in myaddr, remote_addr;
  socklen_t remote_len;
  if ((listen_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      return 1;
    }
  int optval = 1;
  if (setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
      perror ("setsockopt()");
      return 1;
    }
  /* Although we select() on listen_fd we need O_NONBLOCK, due to
     possible network errors making accept() block although select() said
     there was some activity.  */
  if (fcntl (listen_fd, F_SETFL, O_NONBLOCK) < 0)
    {
      perror ("fcntl()");
      return 1;
    }
  myaddr.sin_family = AF_INET;
  myaddr.sin_port = htons (8765);
  myaddr.sin_addr.s_addr = INADDR_ANY;
  if (bind (listen_fd, (struct sockaddr *) &myaddr, sizeof (myaddr)) < 0)
    {
      perror ("bind()");
      return 1;
    }
  if (listen (listen_fd, 20) < 0)
    {
      perror ("listen()");
      return 1;
    }
  while (1)
    {
      fd_set read_set;
      int max_fd;
      FD_ZERO (&read_set);
      max_fd = listen_fd;
      FD_SET (listen_fd, &read_set);
      for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
           it != fd2chan.end(); ++it)
	 {
	   int i = it->first;
	   if (i > max_fd)
	     max_fd = i;
	   FD_SET (i, &read_set);
	 }
      max_fd = select (max_fd + 1, &read_set, NULL, NULL, NULL);
      if (max_fd < 0 && errno == EINTR)
        continue;
      if (max_fd < 0)
        {
	  perror ("select()");
	  return 1;
	}
      if (FD_ISSET (listen_fd, &read_set))
        {
	  max_fd--;
	  remote_len = sizeof (remote_addr);
	  if ((remote_fd = accept (listen_fd, (struct sockaddr *) &remote_addr, &remote_len)) < 0
	      && errno != EAGAIN && errno != EINTR)
	    {
	      perror ("accept()");
	      return 1;
	    }
	  if (remote_fd >= 0)
	    {
	      CS *cs = new CS ((struct sockaddr*) &remote_addr, remote_len);
	      printf ("accepting from %s:%d\n", cs->name.c_str(), cs->port);
	      MsgChannel *c = new MsgChannel (remote_fd, cs);
	      handle_new_connection (c);
	    }
        }
      for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
           max_fd && it != fd2chan.end(); ++it)
	 {
	   int i = it->first;
	   if (FD_ISSET (i, &read_set))
	     {
	       MsgChannel *c = it->second;
	       printf ("message from %s:%d (%d)\n", c->other_end->name.c_str(),
	               c->other_end->port, i);
	       handle_activity (c);
	       max_fd--;
	     }
	 }
    }
  return 0;
}
