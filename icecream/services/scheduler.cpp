/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <string>
#include <list>
#include <map>
#include <queue>
#include <algorithm>
#include <cassert>
#include "comm.h"
#include "logging.h"

/* TODO:
   * leak check
   * are all filedescs closed when done?
   * simplify livetime of the various structures (Jobs/Channels/CSs know
     of each other and sometimes take over ownership)
 */

using namespace std;

struct JobStat {
  unsigned long osize;  // output size (uncompressed)
  unsigned long compile_time_real;  // in milliseconds
  unsigned long compile_time_user;
  unsigned long compile_time_sys;
  unsigned long maxrss; // KB
  JobStat() : osize(0), compile_time_real(0), compile_time_user(0),
	      compile_time_sys(0), maxrss(0) {}
  JobStat& operator +=(const JobStat &st) {
    osize += st.osize;
    compile_time_real += st.compile_time_real;
    compile_time_user += st.compile_time_user;
    compile_time_sys += st.compile_time_sys;
    maxrss += st.maxrss;
    return *this;
  }
  JobStat& operator -=(const JobStat &st) {
    osize -= st.osize;
    compile_time_real -= st.compile_time_real;
    compile_time_user -= st.compile_time_user;
    compile_time_sys -= st.compile_time_sys;
    maxrss -= st.maxrss;
    return *this;
  }
  JobStat& operator /=(int d) {
    osize /= d;
    compile_time_real /= d;
    compile_time_user /= d;
    compile_time_sys /= d;
    maxrss /= d;
    return *this;
  }
  JobStat operator /(int d) const {
    JobStat r = *this;
    r /= d;
    return r;
  }
};

class Job;

/* One compile server (receiver, compile daemon)  */
class CS : public Service {
public:
  /* The listener port, on which it takes compile requests.  */
  unsigned int remote_port;
  unsigned int id;

    // unsigned int jobs_done;
    //  unsigned long long rcvd_kb, sent_kb;
    // unsigned int ms_per_job;
    // unsigned int bytes_per_ms;
  // LOAD is load * 1000
  unsigned int load;
  unsigned int max_jobs;
  //  time_t uptime;  // time connected with scheduler
  list<Job*> joblist;
  list<string> compiler_versions;  // Available compilers
  CS (struct sockaddr *_addr, socklen_t _len)
    : Service (_addr, _len), load(1000), max_jobs(0), state(CONNECTED) {}
  list<JobStat> last_compiled_jobs;
  list<JobStat> last_requested_jobs;
  JobStat cum_compiled;  // cumulated
  JobStat cum_requested;
  enum {CONNECTED, LOGGEDIN} state;
};

map<int, MsgChannel *> fd2chan;

class Job {
public:
  unsigned int id;
  enum {PENDING, COMPILING} state;
  CS *server;  // on which server we build
  CS *submitter;  // who submitted us
  MsgChannel *channel;
  string environment;
  time_t starttime;  // _local_ to the compiler server
  time_t start_on_scheduler;  // starttime local to scheduler
  Job (MsgChannel *c, unsigned int _id, CS *subm)
     : id(_id), state(PENDING), server(0),
       submitter(subm),
       channel(c), starttime(0), start_on_scheduler(0) {}
  ~Job()
  {
   // XXX is this really deleted on all other paths?
/*    fd2chan.erase (channel->fd);
    delete channel;*/
  }
};

// A subset of connected_hosts representing the compiler servers
list<CS*> css;
unsigned int new_job_id;
map<unsigned int, Job*> jobs;
queue<Job*> toanswer;

list<JobStat> all_job_stats;
JobStat cum_job_stats;

list<Service*> monitors;

static void
add_job_stats (Job *job, JobDoneMsg *msg)
{
  JobStat st;
  st.osize = msg->out_uncompressed;
  st.compile_time_real = msg->real_msec;
  st.compile_time_user = msg->user_msec;
  st.compile_time_sys = msg->sys_msec;
  st.maxrss = msg->maxrss;
  job->server->last_compiled_jobs.push_back (st);
  job->server->cum_compiled += st;
  if (job->server->last_compiled_jobs.size() > 40)
    {
      job->server->cum_compiled -= *job->server->last_compiled_jobs.begin ();
      job->server->last_compiled_jobs.pop_front ();
    }
  job->submitter->last_requested_jobs.push_back (st);
  job->submitter->cum_requested += st;
  if (job->submitter->last_requested_jobs.size() > 40)
    {
      job->submitter->cum_requested
        -= *job->submitter->last_requested_jobs.begin ();
      job->submitter->last_requested_jobs.pop_front ();
    }
  all_job_stats.push_back (st);
  cum_job_stats += st;
  if (all_job_stats.size () > 500)
    {
      cum_job_stats -= *all_job_stats.begin ();
      all_job_stats.pop_front ();
    }
}

static bool handle_end (MsgChannel *c, Msg *);

static void
notify_monitors (const Msg &m)
{
  list<Service*>::iterator it;
  for (it = monitors.begin(); it != monitors.end();)
    {
      MsgChannel *c = (*it)->channel();
      ++it;
      /* If we can't send it, don't be clever, simply close this monitor.  */
      if (!c->send_msg (m))
        handle_end (c, 0);
    }
}

static Job *
create_new_job (MsgChannel *channel, CS *submitter)
{
  ++new_job_id;
  assert (jobs.find(new_job_id) == jobs.end());

  Job *job = new Job (channel, new_job_id, submitter);
  jobs[new_job_id] = job;
  return job;
}

static bool
handle_cs_request (MsgChannel *c, Msg *_m)
{
  GetCSMsg *m = dynamic_cast<GetCSMsg *>(_m);
  if (!m)
    return false;

  list<CS*>::iterator it;
  for (it = css.begin(); it != css.end(); ++it)
    if (c->other_end->eq_ip (**it))
      break;
  if (it == css.end())
    {
      fprintf (stderr, "Asking host not connected\n");
      c->send_msg( EndMsg() ); // forget it!
      return false;
    }

  /* Don't use the CS from the channel on which the request came in.
     It will go away as soon as we sent him which server to use.
     Instead use the long-lasting connection to the daemon.  */
  CS *submitter = *it;
  Job *job = create_new_job (c, submitter);
  job->environment = m->version;
  log_info() << "NEW: " << job->id << " version=\""
             << job->environment << "\" " << m->filename
             << " " << ( m->lang == CompileJob::Lang_C ? "C" : "C++" ) << endl;
  toanswer.push( job );
  notify_monitors (MonGetCSMsg (job->id, c->other_end->name, m));
  return true;
}

static bool
handle_local_job (MsgChannel *c, Msg *_m)
{
  JobLocalBeginMsg *m = dynamic_cast<JobLocalBeginMsg *>(_m);
  if (!m)
    return false;

  ++new_job_id;
  if ( !c->send_msg( JobLocalId( new_job_id ) ) )
    return false;

  notify_monitors (MonLocalJobBeginMsg( new_job_id, m->stime, c->other_end->name ) );
  return true;
}

static bool
handle_local_job_end (MsgChannel *c, Msg *_m)
{
  JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg *>(_m);
  trace() << "handle_local_job_end " << m << endl;
  if (!m)
    return false;

  trace() << "handle_local_job_end\n";
  notify_monitors ( MonLocalJobDoneMsg( *m ) );
  fd2chan.erase (c->fd);
  delete c;
  return true;
}

static float
server_speed (CS *cs)
{
  if (cs->last_compiled_jobs.size() == 0
      || cs->cum_compiled.compile_time_user == 0)
    return 0;
  else
    return (float)cs->cum_compiled.osize
             / (float) cs->cum_compiled.compile_time_user;
}

static CS *
pick_server(Job *job)
{
  string environment = job->environment;
  assert( !environment.empty() );
  if ( environment == "*" )
    environment = "gcc33"; // TODO: logic

  list<CS*>::iterator it;

  /* If we have no statistics simply use the first server which is usable.  */
  if (!all_job_stats.size ())
    {
      for (it = css.begin(); it != css.end(); ++it)
        if ((*it)->joblist.size() < (*it)->max_jobs
	    && (*it)->load < 1000)
          return *it;
      return 0;
    }

  /* Now guess about the job.  First see, if this submitter already
     had other jobs.  Use them as base.  */
  JobStat guess;
  if (job->submitter->last_requested_jobs.size() > 0)
    {
      guess = job->submitter->cum_requested
	        / job->submitter->last_requested_jobs.size();
    }
  else
    {
      /* Otherwise simply average over all jobs.  */
      guess = cum_job_stats / all_job_stats.size();
    }
  CS *best = 0;
  for (it = css.begin(); it != css.end(); ++it)
    {
      CS *cs = *it;
      /* For now ignore overloaded servers.  */
      if (cs->joblist.size() >= cs->max_jobs || cs->load >= 1000)
        ;
      else if (!best)
	best = cs;
      else if (cs->last_compiled_jobs.size() == 0)
	best = cs;
      /* Search the server with the earliest projected time to compile
         the job.  (XXX currently this is equivalent to the fastest one)  */
      else if (best->last_compiled_jobs.size() != 0
               && server_speed (best) < server_speed (cs))
	best = cs;
      /* Make all servers compile a job at least once, so we'll get an
         idea about their speed.  */
      if (best && best->last_compiled_jobs.size() == 0)
        break;
    }

  return best;
}

static bool
empty_queue()
{
  // trace() << "empty_queue " << toanswer.size() << " " << css.size() << endl;

  if ( toanswer.empty() )
    return false;

  Job *job = toanswer.front();

  if (css.empty())
    {
      trace() << "no servers to handle\n";
      toanswer.pop();
      job->channel->send_msg( EndMsg() );
      notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id,  255 )));
      return false;
    }

  CS *cs = pick_server (job);

  if (!cs) {
    // trace() << "tried to pick a server for " << job->id << " and failed\n";
    return false;
  }

  toanswer.pop();
  job->server = cs;

  UseCSMsg m2(job->environment, cs->name, cs->remote_port, job->id );

  if (!job->channel->send_msg (m2))
    {
      trace() << "failed to deliver job " << job->id << endl;
      job->channel->send_msg (EndMsg()); // most likely won't work
      cs->joblist.remove (job);
      jobs.erase(job->id );
      notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id, 255 )));
      delete job;
      return true;
    }
  else
    {
      trace() << "put " << job->id << " in joblist of " << cs->name << endl;
      cs->joblist.push_back( job );
    }
  return true;
}

static bool
handle_login (MsgChannel *c, Msg *_m)
{
  LoginMsg *m = dynamic_cast<LoginMsg *>(_m);
  if (!m)
    return false;
  CS *cs = static_cast<CS *>(c->other_end);
  cs->remote_port = m->port;
  cs->compiler_versions = m->envs;
  cs->max_jobs = m->max_kids;
  css.push_back (cs);

  trace() << cs->name << ": [";
  for (list<string>::const_iterator it = m->envs.begin();
       it != m->envs.end(); ++it)
    trace() << *it << ", ";
  trace() << "]\n";

  return true;
}

static bool
handle_mon_login (MsgChannel *c, Msg *_m)
{
  MonLoginMsg *m = dynamic_cast<MonLoginMsg *>(_m);
  if (!m)
    return false;
  // This is really a CS*, but we don't need the full one here
  Service *s = c->other_end;
  monitors.push_back (s);
  return true;
}

static bool
handle_job_begin (MsgChannel *c, Msg *_m)
{
  JobBeginMsg *m = dynamic_cast<JobBeginMsg *>(_m);
  if ( !m )
    return false;

  if (jobs.find(m->job_id) == jobs.end()) {
    trace() << "handle_job_begin: no valid job id " << m->job_id << endl;
    return false;
  }
  trace() << "BEGIN: " << m->job_id << endl;
  if (jobs[m->job_id]->server != c->other_end) {
    trace() << "that job isn't handled by " << c->other_end->name << endl;
    return false;
  }
  jobs[m->job_id]->state = Job::COMPILING;
  jobs[m->job_id]->starttime = m->stime;
  jobs[m->job_id]->start_on_scheduler = time(0);
  notify_monitors (MonJobBeginMsg (m->job_id, m->stime, c->other_end->name));
  return true;
}

static bool
handle_job_done (MsgChannel *c, Msg *_m)
{
  JobDoneMsg *m = dynamic_cast<JobDoneMsg *>(_m);
  if ( !m )
    return false;

  if (jobs.find(m->job_id) == jobs.end()) {
    trace() << "job ID not present " << m->job_id << endl;
    return false;
  }

  if ( m->exitcode == 0 && m->in_uncompressed && m->out_uncompressed )
    trace() << "END " << m->job_id
            << " status=" << m->exitcode
            << " in=" << m->in_uncompressed
            << "(" << int( m->in_compressed * 100 / m->in_uncompressed ) << "%)"
            << " out=" << m->out_uncompressed
            << "(" << int( m->out_compressed * 100 / m->out_uncompressed ) << "%)"
            << " real=" << m->real_msec
            << " user=" << m->user_msec
            << " sys=" << m->sys_msec
            << " rss=" << m->maxrss
            << " idrss=" << m->idrss
            << " pfaults=" << m->majflt
            << " nswaps=" << m->nswap
            << " server=" << c->other_end->name
            << endl;
  else
    trace() << "END " << m->job_id
            << " status=" << m->exitcode << endl;

  if (jobs[m->job_id]->server != c->other_end) {
    log_info() << "the server isn't the same for job " << m->job_id << endl;
    return false;
  }
  Job *j = jobs[m->job_id];
  j->server->joblist.remove (j);
  add_job_stats (j, m);
  notify_monitors (MonJobDoneMsg (*m));
  jobs.erase (m->job_id);
  delete j;


#ifdef DEBUG_SCHEDULER
  bool first = true;

  for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
       it != jobs.end(); ++it)
    {
      int id = it->first;
      Job *c = it->second;
      trace() << "  undone: " << id << " " << c->state << endl;
      if ( first && c->state == Job::PENDING ) {
        trace() << "first job is pending! Something is fishy\n";
        abort();
      }
      first = false;
    }
#endif

  return true;
}

static bool
handle_ping (MsgChannel * /*c*/, Msg * /*_m*/)
{
  trace() << "handle_ping\n";
  return true;
}

static bool
handle_stats (MsgChannel * c, Msg * _m)
{
  StatsMsg *m = dynamic_cast<StatsMsg *>(_m);
  if (!m)
    return false;

  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
    if (( *it )->channel() == c)
      {
        ( *it )->load = m->load;
        notify_monitors( MonStatsMsg( c->other_end->name, ( *it )->max_jobs, *m ) );
        return true;
      }

  return false;
}

static bool
handle_timeout (MsgChannel * /*c*/, Msg * /*_m*/)
{
  return false;
}

// return false if some error occured, leaves C open.  */
static bool
try_login (MsgChannel *c, Msg *m)
{
  bool ret = true;
  switch (m->type)
    {
    case M_GET_CS:
      ret = handle_cs_request (c, m);
      break;
    case M_LOGIN:
      ret = handle_login (c, m);
      break;
    case M_MON_LOGIN:
      ret = handle_mon_login (c, m);
      break;
    case M_JOB_LOCAL_BEGIN:
      ret = handle_local_job(c, m);
      break;
    default:
      log_info() << "Invalid first message" << endl;
      ret = false;
      break;
    }
  delete m;
  if (ret)
    static_cast<CS *>(c->other_end)->state = CS::LOGGEDIN;
  else
    {
      fd2chan.erase (c->fd);
      delete c;
    }
  return ret;
}

static bool
handle_end (MsgChannel *c, Msg *)
{
  if (find (monitors.begin(), monitors.end(), c->other_end) != monitors.end())
    {
      monitors.remove (c->other_end);
      // trace() << "handle_end(moni) " << monitors.size() << endl;
    }
  else
    {
      css.remove (static_cast<CS*>(c->other_end));
      // trace() << "handle_end " << css.size() << endl;
    }

  fd2chan.erase (c->fd);
  delete c;
  return true;
}

static bool
handle_activity (MsgChannel *c)
{
  Msg *m;
  bool ret = true;
  m = c->get_msg (false);
  if (!m)
    {
      handle_end (c, m);
      return false;
    }
  /* First we need to login.  */
  if (static_cast<CS *>(c->other_end)->state == CS::CONNECTED)
    return try_login (c, m);

  switch (m->type)
    {
    case M_JOB_BEGIN: ret = handle_job_begin (c, m); break;
    case M_JOB_DONE: ret = handle_job_done (c, m); break;
    case M_PING: ret = handle_ping (c, m); break;
    case M_STATS: ret = handle_stats (c, m); break;
    case M_END: ret = handle_end (c, m); break;
    case M_TIMEOUT: ret = handle_timeout (c, m); break;
    case M_JOB_LOCAL_DONE: ret = handle_local_job_end( c, m ); break;
    default:
      log_info() << "Invalid message type arrived " << ( char )m->type << endl;
      handle_end (c, m);
      ret = false;
      break;
    }
  delete m;
  return ret;
}

int
open_broad_listener ()
{
  int listen_fd;
  struct sockaddr_in myaddr;
  if ((listen_fd = socket (PF_INET, SOCK_DGRAM, 0)) < 0)
    {
      perror ("socket()");
      return -1;
    }
  int optval = 1;
  if (setsockopt (listen_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0)
    {
      perror ("setsockopt()");
      return -1;
    }
  myaddr.sin_family = AF_INET;
  myaddr.sin_port = htons (8765);
  myaddr.sin_addr.s_addr = INADDR_ANY;
  if (bind (listen_fd, (struct sockaddr *) &myaddr, sizeof (myaddr)) < 0)
    {
      perror ("bind()");
      return -1;
    }
  return listen_fd;
}

int
main (int argc, char * argv[])
{
  int listen_fd, remote_fd, broad_fd;
  struct sockaddr_in myaddr, remote_addr;
  socklen_t remote_len;
  char *netname = (char*)"ICECREAM";

  for (int argi = 1; argi < argc; argi++)
    if (argv[argi][0] == '-' && argv[argi][2] == 0)
      {
        switch (argv[argi][1])
	  {
	  case 'n':
	    argi++;
	    if (argi >= argc)
	      fprintf (stderr, "-n requires argument\n");
	    else
	      {
	        netname = strdup (argv[argi]);
		for (int i = 0; netname[i]; i++)
		  netname[i] = toupper (netname[i]);
	      }
	    break;
	  default:
	    fprintf (stderr, "Unknown argument %s\n", argv[argi]);
	    break;
	  }
      }

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
  broad_fd = open_broad_listener ();
  if (broad_fd < 0)
    {
      return 1;
    }

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
      log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
      return 1;
    }

  while (1)
    {
      while (empty_queue())
	continue;

      fd_set read_set;
      int max_fd = 0;
      FD_ZERO (&read_set);
      if (toanswer.size() < 100)
        { // don't let us overrun
          max_fd = listen_fd;
          FD_SET (listen_fd, &read_set);
        }
      if (broad_fd > max_fd)
        max_fd = broad_fd;
      FD_SET (broad_fd, &read_set);
      for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
           it != fd2chan.end();)
	 {
	   int i = it->first;
	   MsgChannel *c = it->second;
	   bool ok = true;
	   ++it;
	   /* handle_activity() can delete c and make the iterator
	      invalid.  */
	   while (ok && c->has_msg ())
	     if (!handle_activity (c))
	       ok = false;
	   if (ok)
	     {
	       if (i > max_fd)
	         max_fd = i;
	       FD_SET (i, &read_set);
	     }
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
          remote_fd = accept (listen_fd,
                              (struct sockaddr *) &remote_addr,
                              &remote_len );
	  if (remote_fd < 0 && errno != EAGAIN && errno != EINTR)
	    {
	      perror ("accept()");
	      return 1;
	    }
	  if (remote_fd >= 0)
	    {
	      CS *cs = new CS ((struct sockaddr*) &remote_addr, remote_len);
	      // printf ("accepting from %s:%d\n", cs->name.c_str(), cs->port);
	      MsgChannel *c = cs->createChannel (remote_fd);
	      fd2chan[c->fd] = c;
	      if (!c->read_a_bit () || c->has_msg ())
	        handle_activity (c);
	    }
        }
      if (max_fd && FD_ISSET (broad_fd, &read_set))
        {
	  max_fd--;
	  char buf[16];
	  struct sockaddr_in broad_addr;
	  socklen_t broad_len = sizeof (broad_addr);
	  if (recvfrom (broad_fd, buf, 1, 0, (struct sockaddr*) &broad_addr,
			&broad_len) != 1)
	    {
	      perror ("recvfrom()");
	      return -1;
	    }
	  else
	    {
	      printf ("broadcast from %s:%d\n", inet_ntoa (broad_addr.sin_addr),
		      ntohs (broad_addr.sin_port));
	      buf[0]++;
	      memset (buf + 1, 0, sizeof (buf) - 1);
	      snprintf (buf + 1, sizeof (buf) - 1, netname);
	      buf[sizeof (buf) - 1] = 0;
	      if (sendto (broad_fd, buf, sizeof (buf), 0,
	      		  (struct sockaddr*)&broad_addr, broad_len) != sizeof (buf))
		{
		  perror ("sendto()");
		}
	    }
	}
      for (map<int, MsgChannel *>::const_iterator it = fd2chan.begin();
           max_fd && it != fd2chan.end();)
	 {
	   int i = it->first;
	   MsgChannel *c = it->second;
	   /* handle_activity can delete the channel from the fd2chan list,
	      hence advance the iterator right now, so it doesn't become
	      invalid.  */
	   ++it;
	   if (FD_ISSET (i, &read_set))
	     {
	       if (!c->read_a_bit () || c->has_msg ())
	         handle_activity (c);
	       max_fd--;
	     }
	 }
    }
  close (broad_fd);
  return 0;
}
