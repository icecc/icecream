/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

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
#include <getopt.h>
#include <string>
#include <list>
#include <map>
#include <queue>
#include <algorithm>
#include <cassert>
#include <stdio.h>
#include "comm.h"
#include "logging.h"

/* TODO:
   * leak check
   * are all filedescs closed when done?
   * simplify livetime of the various structures (Jobs/Channels/CSs know
     of each other and sometimes take over ownership)
 */

/* The typical flow of messages for a remote job should be like this:
     prereq: daemon is connected to scheduler
     * client does GET_CS
     * request gets queued
     * request gets handled
     * scheduler sends USE_CS
     * client asks remote daemon
     * daemon sends JOB_BEGIN
     * client sends END + closes connection
     * daemon sends JOB_DONE (this can be swapped with the above one)
   This means, that iff the client somehow closes the connection we can and
   must remove all traces of jobs resulting from that client in all lists.
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
  unsigned int hostid;
  string nodename;
  bool busy_installing;
  string host_platform;

    // unsigned int jobs_done;
    //  unsigned long long rcvd_kb, sent_kb;
    // unsigned int ms_per_job;
    // unsigned int bytes_per_ms;
  // LOAD is load * 1000
  unsigned int load;
  int max_jobs;
  list<Job*> joblist;
  Environments compiler_versions;  // Available compilers
  CS (struct sockaddr *_addr, socklen_t _len)
    : Service(_addr, _len), load(1000), max_jobs(0), state(CONNECTED),
      type(UNKNOWN) {
    hostid = 0;
    busy_installing = false;
  }
  void pick_new_id() {
    assert( !hostid );
    hostid = ++hostid_counter;
  }
  list<JobStat> last_compiled_jobs;
  list<JobStat> last_requested_jobs;
  JobStat cum_compiled;  // cumulated
  JobStat cum_requested;
  enum {CONNECTED, LOGGEDIN} state;
  enum {UNKNOWN, CLIENT, DAEMON, MONITOR} type;
  static unsigned int hostid_counter;
};

unsigned int CS::hostid_counter = 0;

static map<int, MsgChannel *> fd2chan;

class Job {
public:
  unsigned int id;
  enum {PENDING, WAITINGFORCS, COMPILING} state;
  CS *server;  // on which server we build
  CS *submitter;  // who submitted us
  MsgChannel *channel;
  string environment;
  time_t starttime;  // _local_ to the compiler server
  time_t start_on_scheduler;  // starttime local to scheduler
  string target_platform;
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
static list<CS*> css;
static unsigned int new_job_id;
static map<unsigned int, Job*> jobs;
/* XXX Uah.  Don't use a queue for the job requests.  It's a hell
   to delete anything out of them (for clean up).  */
struct UnansweredList {
  list<Job*> l;
  CS *server;
  bool remove_job (Job *);
};
static list<UnansweredList*> toanswer;

static list<JobStat> all_job_stats;
static JobStat cum_job_stats;

static list<Service*> monitors;

/* Searches the queue for JOB and removes it.
   Returns true of something was deleted.  */
bool
UnansweredList::remove_job (Job *job)
{
  list<Job*>::iterator it;
  for (it = l.begin(); it != l.end(); ++it)
    if (*it == job)
      {
        l.erase (it);
	return true;
      }
  return false;
}

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

static void
handle_monitor_stats( CS *cs, StatsMsg *m = 0)
{
  string msg;
  char buffer[1000];
  sprintf( buffer, "Name:%s\n", cs->nodename.c_str() );
  msg += buffer;
  sprintf( buffer, "IP:%s\n", cs->name.c_str() );
  msg += buffer;
  sprintf( buffer, "MaxJobs:%d\n", cs->max_jobs );
  msg += buffer;
  if ( m ) {
    sprintf( buffer, "Load:%d\n", m->load );
    msg += buffer;
    sprintf( buffer, "NiceLoad:%d\n", m->niceLoad );
    msg += buffer;
    sprintf( buffer, "SysLoad:%d\n", m->sysLoad );
    msg += buffer;
    sprintf( buffer, "UserLoad:%d\n", m->userLoad );
    msg += buffer;
    sprintf( buffer, "IdleLoad:%d\n", m->idleLoad );
    msg += buffer;
    sprintf( buffer, "LoadAvg1:%d\n", m->loadAvg1 );
    msg += buffer;
    sprintf( buffer, "LoadAvg5:%d\n", m->loadAvg5 );
    msg += buffer;
    sprintf( buffer, "LoadAvg10:%d\n", m->loadAvg10 );
    msg += buffer;
    sprintf( buffer, "FreeMem:%d\n", m->freeMem );
    msg += buffer;
  } else {
    sprintf( buffer, "Load:%d\n", cs->load );
    msg += buffer;
  }
  notify_monitors( MonStatsMsg( cs->hostid, msg ) );
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

static void
enqueue_job_request (Job *job)
{
  if (!toanswer.empty() && toanswer.back()->server == job->submitter)
    toanswer.back()->l.push_back (job);
  else {
    UnansweredList *newone = new UnansweredList();
    newone->server = job->submitter;
    newone->l.push_back (job);
    toanswer.push_back (newone);
  }
}

static Job *
get_job_request (void)
{
  if (toanswer.empty())
    return 0;

  UnansweredList *first = toanswer.front();
  assert (!first->l.empty());
  return first->l.front();
}

/* Removes the first job request (the one returned by get_job_request()) */
static void
remove_job_request (void)
{
  if (toanswer.empty())
    return;
  UnansweredList *first = toanswer.front();
  first->l.pop_front();
  if (first->l.empty())
    {
      toanswer.pop_front();
      delete first;
    }
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

  for ( unsigned int i = 0; i < m->count; ++i )
    {
      Job *job = create_new_job (c, submitter);
#warning MATZ: FASS!
      job->environment = "";
      job->target_platform = m->target;
      for ( Environments::const_iterator it = m->versions.begin(); it != m->versions.end(); ++it )
        {
          if ( it->first == m->target ) {
            job->environment = it->second;
            break;
          }
        }
      enqueue_job_request (job);
      log_info() << "NEW: " << job->id << " version=\""
                 << job->environment << "\"(" << m->target << ") " << m->filename
                 << " " << ( m->lang == CompileJob::Lang_C ? "C" : "C++" ) << endl;
      notify_monitors (MonGetCSMsg (job->id, submitter->hostid, m));
    }
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

  list<CS*>::iterator it;
  for (it = css.begin(); it != css.end(); ++it)
    if (c->other_end->eq_ip (**it))
      break;
  if ( it != css.end() )
    notify_monitors (MonLocalJobBeginMsg( new_job_id, m->outfile, m->stime, ( *it )->hostid ) );
  return true;
}

static bool
handle_local_job_end (MsgChannel *, Msg *_m)
{
  JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg *>(_m);
  if (!m)
    return false;

  notify_monitors ( MonLocalJobDoneMsg( *m ) );
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

static bool
envs_match( CS* cs, const Job *job )
{
  for ( Environments::const_iterator it = cs->compiler_versions.begin(); it != cs->compiler_versions.end(); ++it )
    {
      if ( it->first == job->target_platform && it->second == job->environment )
        return true;
    }
  return false;
}

static bool
can_install( CS* cs, const Job *job )
{
  trace() << "can_install host: '" << cs->host_platform << "' target: '" << job->target_platform << "'" << endl;
  if ( cs->busy_installing )
    return false;
  // XXX: instead of doing string compares all the time we should keep an array of platforms and compare indices
  // there we could also put i386/i486/i586/i686 in one index (at least upwards?)
  return cs->host_platform == job->target_platform;
}

static CS *
pick_server(Job *job)
{
  list<CS*>::iterator it;

  trace() << "pick_server " << job->id << " " << job->target_platform << endl;

  time_t now = time( 0 );
  // first we check if all are up
  for (it = css.begin(); it != css.end(); ) {
    if ( now - ( *it )->last_talk > 15 ) {
      if ( ( *it )->max_jobs > 0 ) {
        trace() << "send ping " << ( *it )->name << endl;
        ( *it )->channel()->send_msg( PingMsg() );
        ( *it )->max_jobs *= -1; // better not give it
      } else { // R.I.P.
        trace() << "removing " << ( *it )->name << endl;
        it = css.erase( it );
        continue;
      }
    }
    ++it;
  }

  /* If we have no statistics simply use the first server which is usable.  */
  if (!all_job_stats.size ())
    {
      for (it = css.begin(); it != css.end(); ++it)
        {
          trace() << "no job stats - looking at " << ( *it )->name << " load: " << (*it )->load << " can install: " << can_install( *it, job ) << endl;
          if (int( (*it)->joblist.size() ) < (*it)->max_jobs
              && (*it)->load < 1000 && can_install( *it, job ) )
            {
              trace() << "returning first " << ( *it )->name << endl;
              return *it;
            }
        }
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
  // best uninstalled
  CS *bestui = 0;

  for (it = css.begin(); it != css.end(); ++it)
    {
      CS *cs = *it;
      /* For now ignore overloaded servers.  */
      if (int( cs->joblist.size() ) >= cs->max_jobs || cs->load >= 1000) {
	// trace() << "overloaded " << cs->name << " " << cs->joblist.size() << "/" <<  cs->max_jobs << " jobs, load:" << cs->load << endl;
        continue;
      }

      // incompatible architecture
      if ( !can_install( cs, job ) ) {
	trace() << cs->name << " can't install " << job->environment << endl;
        continue;
      }

      /* Servers that are already compiling jobs but got no environments
         are currently installing new environments - ignore so far */
      if ( cs->joblist.size() != 0 && cs->compiler_versions.size() == 0 ) {
        trace() << cs->name << " is currently installing\n";
        continue;
      }

      if ( cs->last_compiled_jobs.size() == 0 && cs->joblist.size() == 0)
	{
	  /* Make all servers compile a job at least once, so we'll get an
	     idea about their speed.  */
	  if (envs_match (cs, job))
	    best = cs;
	  else // if there is one server that already got the environment and one that
            // hasn't compiled at all, pick the one with environment first
	    bestui = cs;
	  break;
	}

      // trace() << "server_speed " << cs->name << " " << server_speed (cs) << endl;
      if ( envs_match( cs, job ) )
        {
          if ( !best )
            best = cs;
          /* Search the server with the earliest projected time to compile
             the job.  (XXX currently this is equivalent to the fastest one)  */
          else
            if (best->last_compiled_jobs.size() != 0
                && server_speed (best) < server_speed (cs))
              best = cs;
        }
      else
        {
          if ( !bestui )
            bestui = cs;
          /* Search the server with the earliest projected time to compile
             the job.  (XXX currently this is equivalent to the fastest one)  */
          else
            if (bestui->last_compiled_jobs.size() != 0
                && server_speed (bestui) < server_speed (cs))
              bestui = cs;
        }
    }

  if ( best )
    return best;
  return bestui;
}

static bool
empty_queue()
{
  Job *job = get_job_request ();
  if (!job)
    return false;

  if (css.empty())
    {
      trace() << "no servers to handle\n";
      remove_job_request ();
      job->channel->send_msg( EndMsg() );
      jobs.erase( job->id );
      notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id,  255 )));
      // Don't delete channel here.  We expect the client on the other side
      // to exit, and that will remove the channel in handle_end
      delete job;
      return false;
    }

  CS *cs = pick_server (job);

  if (!cs) {
    trace() << "tried to pick a server for " << job->id << " and failed\n";
    return false;
  }

  remove_job_request ();

  job->state = Job::WAITINGFORCS;
  job->server = cs;

  bool gotit = envs_match( cs, job );
  UseCSMsg m2(job->environment, cs->name, cs->remote_port, job->id, gotit );

  if (!job->channel->send_msg (m2))
    {
      trace() << "failed to deliver job " << job->id << endl;
      job->channel->send_msg (EndMsg()); // most likely won't work
      jobs.erase( job->id );
      notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id, 255 )));
      delete job;
      return true;
    }
  else
    {
      trace() << "put " << job->id << " in joblist of " << cs->name << endl;
      cs->joblist.push_back( job );
      if ( !gotit ) { // if we made the environment transfer, don't rely on the list
        cs->compiler_versions.clear();
        cs->busy_installing = true;
      }
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
  if ( m->nodename.length() )
    cs->nodename = m->nodename;
  else
    cs->nodename = cs->name;
  cs->host_platform = m->host_platform;
  cs->pick_new_id();
  handle_monitor_stats( cs );
  css.push_back (cs);

#if 0
  trace() << cs->name << ": [";
  for (list<string>::const_iterator it = m->envs.begin();
       it != m->envs.end(); ++it)
    trace() << *it << ", ";
  trace() << "]\n";
#endif

  return true;
}

static bool
handle_relogin (MsgChannel *c, Msg *_m)
{
  LoginMsg *m = dynamic_cast<LoginMsg *>(_m);
  if (!m)
    return false;

  CS *cs = static_cast<CS *>(c->other_end);
  cs->compiler_versions = m->envs;
  cs->busy_installing = false;

  trace() << cs->name << "(" << cs->host_platform << "): [";
  for (Environments::const_iterator it = m->envs.begin();
       it != m->envs.end(); ++it)
    trace() << it->second << "(" << it->first << "), ";
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
  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
    handle_monitor_stats( *it );
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
  CS *cs = dynamic_cast<CS*>( c->other_end );
  notify_monitors (MonJobBeginMsg (m->job_id, m->stime, cs->hostid));
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
  c->other_end->last_talk = time( 0 );

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
handle_ping (MsgChannel * c, Msg * /*_m*/)
{
  c->other_end->last_talk = time( 0 );
  CS *cs = dynamic_cast<CS*>( c->other_end );
  if ( cs && cs->max_jobs < 0 )
    cs->max_jobs *= -1;
  return true;
}

static bool
handle_stats (MsgChannel * c, Msg * _m)
{
  StatsMsg *m = dynamic_cast<StatsMsg *>(_m);
  if (!m)
    return false;

  c->other_end->last_talk = time( 0 );
  CS *cs = dynamic_cast<CS*>( c->other_end );
  if ( cs && cs->max_jobs < 0 )
    cs->max_jobs *= -1;

  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
    if (( *it )->channel() == c)
      {
        ( *it )->load = m->load;
        handle_monitor_stats( *it, m );
        return true;
      }

  return false;
}

static bool
handle_timeout (MsgChannel * c, Msg * /*_m*/)
{
  c->other_end->last_talk = time( 0 );
  return false;
}

// return false if some error occured, leaves C open.  */
static bool
try_login (MsgChannel *c, Msg *m)
{
  bool ret = true;
  CS *cs = static_cast<CS *>(c->other_end);
  trace() << "login " << cs->name << " protocol version: " << c->protocol << endl;
  switch (m->type)
    {
    case M_GET_CS:
      cs->type = CS::CLIENT;
      ret = handle_cs_request (c, m);
      break;
    case M_LOGIN:
      cs->type = CS::DAEMON;
      ret = handle_login (c, m);
      break;
    case M_MON_LOGIN:
      cs->type = CS::MONITOR;
      ret = handle_mon_login (c, m);
      break;
    case M_JOB_LOCAL_BEGIN:
      cs->type = CS::CLIENT;
      ret = handle_local_job (c, m);
      break;
    default:
      log_info() << "Invalid first message " << (char)m->type << endl;
      ret = false;
      break;
    }
  delete m;
  if (ret)
    cs->state = CS::LOGGEDIN;
  else
    {
      fd2chan.erase (c->fd);
      delete c;
    }
  return ret;
}

static bool
handle_end (MsgChannel *c, Msg *m)
{
  trace() << "Handle_end " << c << m << endl;

  CS *toremove = static_cast<CS *>(c->other_end);
  if (toremove->type == CS::MONITOR)
    {
      assert (find (monitors.begin(), monitors.end(), c->other_end) != monitors.end());
      monitors.remove (c->other_end);
      trace() << "handle_end(moni) " << monitors.size() << endl;
    }
  else if (toremove->type == CS::DAEMON)
    {
      trace() << "remove daemon\n";

      notify_monitors( MonStatsMsg( toremove->hostid, "State:Offline\n" ) );

      /* A daemon disconnected.  We must remove it from the css list,
         and we have to delete all jobs scheduled on that daemon.
	 There might be still clients connected running on the machine on which
	 the daemon died.  We expect that the daemon dying makes the client
	 disconnect soon too.  */
      css.remove (toremove);

      /* Unfortunately the toanswer queues are also tagged based on the daemon,
         so we need to clean them up also.  */
      list<UnansweredList*>::iterator it;
      for (it = toanswer.begin(); it != toanswer.end();)
	if ((*it)->server == toremove)
	  {
	    UnansweredList *l = *it;
	    list<Job*>::iterator jit;
	    for (jit = l->l.begin(); jit != l->l.end(); ++jit)
	      {
		trace() << "STOP FOR " << (*jit)->id << endl;
		(*jit)->channel->send_msg( EndMsg() );
                notify_monitors (MonJobDoneMsg (JobDoneMsg( ( *jit )->id,  255 )));
		jobs.erase( (*jit)->id );
		delete (*jit);
	      }
	    delete l;
	    it = toanswer.erase (it);
	  }
	else
	  ++it;

      map<unsigned int, Job*>::iterator mit;
      for (mit = jobs.begin(); mit != jobs.end(); ++mit )
	if (mit->second->server == toremove
	    || mit->second->submitter == toremove)
	  {
	    trace() << "STOP FOR " << mit->first << endl;
	    mit->second->channel->send_msg( EndMsg() );
            notify_monitors (MonJobDoneMsg (JobDoneMsg( mit->second->id,  255 )));
	    delete mit->second;
	    jobs.erase( mit );
	  }
    }
  else if (toremove->type == CS::CLIENT)
    {
      trace() << "remove client\n";

      /* A client disconnected.  */
      if (!m)
        {
	  /* If it's disconnected without END message something went wrong,
	     and we must remove all its job requests and jobs.  All job
	     requests are also in the jobs list, so it's enough to traverse
	     that one, and when finding a job to possibly remove it also
	     from any request queues.
	     XXX This is made particularly ugly due to using real queues.  */
	  map<unsigned int, Job*>::iterator it;
	  for (it = jobs.begin(); it != jobs.end(); ++it)
	    {
	      if (it->second->channel == c)
		{
		  trace() << "STOP FOR " << it->first << endl;
		  Job *job = it->second;
                  notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id,  255 )));

		  /* Remove this job from the request queue.  */
		  list<UnansweredList*>::iterator ait;
		  for (ait = toanswer.begin(); ait != toanswer.end();)
		    {
		      UnansweredList *l = *ait;
		      if (l->server == job->submitter
			  && (l->l.remove (job), true)
			  && l->l.empty())
			{
			  ait = toanswer.erase (ait);
			  delete l;
			}
		      else
			++ait;
		    }

		  if ( job->server )
		      job->server->joblist.remove (job);
		  jobs.erase (it);
		  delete job;
		}
	    }
	}
    }
  else
    trace() << "remote end had UNKNOWN type?" << endl;

  fd2chan.erase (c->fd);
  delete c;
  return true;
}

/* Returns TRUE if C was not closed.  */
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
    case M_END: handle_end (c, m); ret = false; break;
    case M_TIMEOUT: ret = handle_timeout (c, m); break;
    case M_JOB_LOCAL_BEGIN: ret = handle_local_job (c, m); break;
    case M_JOB_LOCAL_DONE: ret = handle_local_job_end (c, m); break;
    case M_LOGIN: ret = handle_relogin( c, m ); break;
    default:
      log_info() << "Invalid message type arrived " << ( char )m->type << endl;
      handle_end (c, m);
      ret = false;
      break;
    }
  delete m;
  return ret;
}

static int
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

static void
usage(const char* reason = 0)
{
  if (reason)
     cerr << reason << endl;

  cerr << "usage: scheduler [options] \n"
       << "Options:\n"
       << "  -n, --netname <name>\n"
       << "  -p, --port <port>\n"
       << "  -h, --help\n"
       << "  -l, --log-file <file>\n"
       << "  -d, --daemonize\n"
       << "  -v[v[v]]]\n"
       << endl;

  exit(1);
}

int
main (int argc, char * argv[])
{
  int listen_fd, remote_fd, broad_fd;
  unsigned int port = 8765;
  struct sockaddr_in myaddr, remote_addr;
  socklen_t remote_len;
  char *netname = (char*)"ICECREAM";
  bool detach = false;
  int debug_level = Error;
  string logfile;

  while ( true ) {
    int option_index = 0;
    static const struct option long_options[] = {
      { "netname", 1, NULL, 'n' },
      { "help", 0, NULL, 'h' },
      { "port", 0, NULL, 'p' },
      { "daemonize", 0, NULL, 'd'},
      { "log-file", 1, NULL, 'l'},
      { 0, 0, 0, 0 }
    };

    const int c = getopt_long( argc, argv, "n:p:hl:vd", long_options, &option_index );
    if ( c == -1 ) break; // eoo

    switch ( c ) {
    case 0:
      {
        ( void ) long_options[option_index].name;
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
    case 'p':
      if ( optarg && *optarg )
        {
          port = 0; port = atoi( optarg );
          if ( 0 == port )
            usage("Error: Invalid port specified");
         }
      else
        usage("Error: -p requires argument");
      break;
    default:
      usage();
    }
  }

  if ( !logfile.size() && detach )
    logfile = "/var/log/icecc_scheduler";

  setup_debug( debug_level, logfile );
  if ( detach )
    daemon( 0, 0 );

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
  myaddr.sin_port = htons (port);
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
      if (toanswer.size() < 100) // TODO: this is rather pointless as toanswer is now a queue of queues
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
              cs->last_talk = time( 0 );
	      trace() << "accepting from " << cs->name << ":" << cs->port << "\n";
	      MsgChannel *c = cs->createChannel (remote_fd);
              if ( !c->protocol ) // protocol mismatch
                {
                  delete cs;
                  continue;
                }
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
	      int err = errno;
	      perror ("recvfrom()");
	      /* Some linux 2.6 kernels can return from select with
	         data available, and then return from read() with EAGAIN
		 even on a blocking socket (breaking POSIX).  Happens
		 when the arriving packet has a wrong checksum.  So
		 we ignore EAGAIN here, but still abort for all other errors. */
	      if (err != EAGAIN)
	        return -1;
	    }
	  else
	    {
	      log_info() << "broadcast from " << inet_ntoa (broad_addr.sin_addr)
                         << ":" << ntohs (broad_addr.sin_port) << "\n";
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
