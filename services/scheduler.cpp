/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */
/* vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4: */
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
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif

#include <sys/stat.h>
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
#include <fstream>
#include <string>
#include <stdio.h>
#include <pwd.h>
#include "comm.h"
#include "logging.h"
#include "job.h"
#include "config.h"
#include "bench.h"

#define DEBUG_SCHEDULER 0

/* TODO:
   * leak check
   * are all filedescs closed when done?
   * simplify livetime of the various structures (Jobs/Channels/CSs know
     of each other and sometimes take over ownership)
 */

/* TODO:
  - iron out differences in code size between architectures
   + ia64/i686: 1.63
   + x86_64/i686: 1.48
   + ppc/i686: 1.22
   + ppc64/i686: 1.59
  (missing data for others atm)
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

static string pidFilePath;

struct JobStat {
  unsigned long osize;  // output size (uncompressed)
  unsigned long compile_time_real;  // in milliseconds
  unsigned long compile_time_user;
  unsigned long compile_time_sys;
  unsigned int job_id;
  JobStat() : osize(0), compile_time_real(0), compile_time_user(0),
	      compile_time_sys(0), job_id(0) {}
  JobStat& operator +=(const JobStat &st)
  {
    osize += st.osize;
    compile_time_real += st.compile_time_real;
    compile_time_user += st.compile_time_user;
    compile_time_sys += st.compile_time_sys;
    job_id = 0;
    return *this;
  }
  JobStat& operator -=(const JobStat &st)
  {
    osize -= st.osize;
    compile_time_real -= st.compile_time_real;
    compile_time_user -= st.compile_time_user;
    compile_time_sys -= st.compile_time_sys;
    job_id = 0;
    return *this;
  }
private:
  JobStat& operator /=(int d)
  {
    osize /= d;
    compile_time_real /= d;
    compile_time_user /= d;
    compile_time_sys /= d;
    job_id = 0;
    return *this;
  }
public:
  JobStat operator /(int d) const
  {
    JobStat r = *this;
    r /= d;
    return r;
  }

};

class Job;

/* One compile server (receiver, compile daemon)  */
class CS : public MsgChannel
{
public:
  /* The listener port, on which it takes compile requests.  */
  unsigned int remote_port;
  unsigned int hostid;
  string nodename;
  time_t busy_installing;
  string host_platform;

  // unsigned int jobs_done;
  //  unsigned long long rcvd_kb, sent_kb;
  // unsigned int ms_per_job;
  // unsigned int bytes_per_ms;
  // LOAD is load * 1000
  unsigned int load;
  int max_jobs;
  bool noremote;
  list<Job*> joblist;
  int submitted_jobs_count;
  Environments compiler_versions;  // Available compilers
  CS (int fd, struct sockaddr *_addr, socklen_t _len, bool text_based)
    : MsgChannel(fd, _addr, _len, text_based),
      load(1000), max_jobs(0), noremote(false), submitted_jobs_count(0),
      state(CONNECTED), type(UNKNOWN), chroot_possible(false)
  {
    hostid = 0;
    busy_installing = 0;
  }
  void pick_new_id()
  {
    assert( !hostid );
    hostid = ++hostid_counter;
  }
  list<JobStat> last_compiled_jobs;
  list<JobStat> last_requested_jobs;
  JobStat cum_compiled;  // cumulated
  JobStat cum_requested;
  enum {CONNECTED, LOGGEDIN} state;
  enum {UNKNOWN, DAEMON, MONITOR, LINE} type;
  bool chroot_possible;
  static unsigned int hostid_counter;
  map<int, int> client_map; // map client ID for daemon to our IDs
  map<CS*, Environments> blacklist;
  bool is_eligible( const Job *job );
  bool check_remote( const Job *job ) const;
};

unsigned int CS::hostid_counter = 0;

static map<int, CS *> fd2cs;
static bool exit_main_loop = false;

time_t starttime;

class Job
{
public:
  unsigned int id;
  unsigned int local_client_id;
  enum {PENDING, WAITINGFORCS, COMPILING} state;
  CS *server;  // on which server we build
  CS *submitter;  // who submitted us
  Environments environments;
  time_t starttime;  // _local_ to the compiler server
  time_t start_on_scheduler;  // starttime local to scheduler
  /**
   * the end signal from client and daemon is a bit of a race and
   * in 99.9% of all cases it's catched correctly. But for the remaining
   * 0.1% we need a solution too - otherwise these jobs are eating up slots.
   * So the solution is to track done jobs (client exited, daemon didn't signal)
   * and after 10s no signal, kill the daemon (and let it rehup) **/
  time_t done_time;

  string target_platform;
  string filename;
  list<Job*> master_job_for;
  unsigned int arg_flags;
  string language; // for debugging
  string preferred_host; // for debugging daemons
  bool ignore_unverified; // ignore CSs that don't know M_VERIFY_ENV
  Job (unsigned int _id, CS *subm)
    : id(_id), local_client_id( 0 ), state(PENDING), server(0),
      submitter(subm),
      starttime(0), start_on_scheduler(0), done_time( 0 ), arg_flags( 0 )
  {
    ++submitter->submitted_jobs_count;
  }

  ~Job()
  {
   // XXX is this really deleted on all other paths?
/*    fd2chan.erase (channel->fd);
    delete channel;*/
    --submitter->submitted_jobs_count;
  }
};

// A subset of connected_hosts representing the compiler servers
static list<CS*> css, monitors, controls;
static list<string> block_css;
static unsigned int new_job_id;
static map<unsigned int, Job*> jobs;

/* XXX Uah.  Don't use a queue for the job requests.  It's a hell
   to delete anything out of them (for clean up).  */
struct UnansweredList
{
  list<Job*> l;
  CS *server;
  bool remove_job (Job *);
};
static list<UnansweredList*> toanswer;

static list<JobStat> all_job_stats;
static JobStat cum_job_stats;

static float server_speed (CS *cs, Job *job = 0);

/* Searches the queue for JOB and removes it.
   Returns true if something was deleted.  */
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
  /* We don't want to base our timings on failed or too small jobs.  */
  if (msg->out_uncompressed < 4096
      || msg->exitcode != 0)
    return;

  st.osize = msg->out_uncompressed;
  st.compile_time_real = msg->real_msec;
  st.compile_time_user = msg->user_msec;
  st.compile_time_sys = msg->sys_msec;
  st.job_id = job->id;

  if ( job->arg_flags & CompileJob::Flag_g )
    st.osize = st.osize * 10 / 36; // average over 1900 jobs: faktor 3.6 in osize
  else if ( job->arg_flags & CompileJob::Flag_g3 )
    st.osize = st.osize * 10 / 45; // average over way less jobs: factor 1.25 over -g

  // the difference between the -O flags isn't as big as the one between -O0 and -O>=1
  // the numbers are actually for gcc 3.3 - but they are _very_ rough heurstics anyway)
  if ( job->arg_flags & CompileJob::Flag_O ||
       job->arg_flags & CompileJob::Flag_O2 ||
       job->arg_flags & CompileJob::Flag_Ol2)
    st.osize = st.osize * 58 / 35;

  if ( job->server->last_compiled_jobs.size() >= 7)
    {
      /* Smooth out spikes by not allowing one job to add more than
         20% of the current speed.  */
      float this_speed = (float) st.osize / (float) st.compile_time_user;
      /* The current speed of the server, but without adjusting to the current
         job, hence no second argument.  */
      float cur_speed = server_speed (job->server);

      if ((this_speed / 1.2) > cur_speed)
        st.osize = (long unsigned) (cur_speed * 1.2 * st.compile_time_user);
      else if ((this_speed * 1.2) < cur_speed)
        st.osize = (long unsigned) (cur_speed / 1.2 * st.compile_time_user);
    }

  job->server->last_compiled_jobs.push_back (st);
  job->server->cum_compiled += st;
  if (job->server->last_compiled_jobs.size() > 200)
    {
      job->server->cum_compiled -= *job->server->last_compiled_jobs.begin ();
      job->server->last_compiled_jobs.pop_front ();
    }
  job->submitter->last_requested_jobs.push_back (st);
  job->submitter->cum_requested += st;
  if (job->submitter->last_requested_jobs.size() > 200)
    {
      job->submitter->cum_requested
        -= *job->submitter->last_requested_jobs.begin ();
      job->submitter->last_requested_jobs.pop_front ();
    }
  all_job_stats.push_back (st);
  cum_job_stats += st;
  if (all_job_stats.size () > 2000)
    {
      cum_job_stats -= *all_job_stats.begin ();
      all_job_stats.pop_front ();
    }

#if DEBUG_SCHEDULER > 1
  if ( job->arg_flags < 7000 )
    trace() << "add_job_stats " << job->language << " "
            << ( time( 0 ) - starttime ) << " "
            << st.compile_time_user << " "
            << ( job->arg_flags & CompileJob::Flag_g ? '1' : '0')
            << ( job->arg_flags & CompileJob::Flag_g3 ? '1' : '0')
            << ( job->arg_flags & CompileJob::Flag_O ? '1' : '0')
            << ( job->arg_flags & CompileJob::Flag_O2 ? '1' : '0')
            << ( job->arg_flags & CompileJob::Flag_Ol2 ? '1' : '0')
            << " " << st.osize << " " << msg->out_uncompressed << " "
            << job->server->nodename << " "
            << float(msg->out_uncompressed) / st.compile_time_user << " "
            << server_speed( job->server ) << endl ;
#endif

}

static bool handle_end (CS *c, Msg *);

static void
notify_monitors (Msg* m)
{
  list<CS*>::iterator it, it_old;
  for (it = monitors.begin(); it != monitors.end();)
    {
      it_old = it++;
      /* If we can't send it, don't be clever, simply close this monitor.  */
      if (!(*it_old)->send_msg (*m, MsgChannel::SendNonBlocking /*| MsgChannel::SendBulkOnly*/)) {
        trace() << "monitor is blocking... removing" << endl;
        handle_end (*it_old, 0);
      }
    }
  delete m;
}

static float
server_speed (CS *cs, Job *job)
{
  if (cs->last_compiled_jobs.size() == 0
      || cs->cum_compiled.compile_time_user == 0)
    return 0;
  else
    {
      float f = (float)cs->cum_compiled.osize
	       / (float) cs->cum_compiled.compile_time_user;

      // we only care for the load if we're about to add a job to it
      if (job) {
        if (job->submitter == cs) {
        /* The submitter of a job gets more speed if it's capable of handling its requests on its own.
           So if he is equally fast to the rest of the farm it will be preferred to chose him
           to compile the job.  Then this can be done locally without needing the preprocessor.
           However if there are more requests than the number of jobs the submitter can handle,
           it is assumed the submitter is doing a massively parallel build, in which case it is
           better not to build on the submitter and let it do other work (such as preprocessing
           output for other nodes) that can be done only locally.  */
          if (cs->submitted_jobs_count <= cs->max_jobs)
            f *= 1.1;
          else
            f *= 0.1; // penalize heavily
        }
        else // ignoring load for submitter - assuming the load is our own
          f *= float(1000 - cs->load) / 1000;
      }

      // below we add a pessimism factor - assuming the first job a computer got is not representative
      if ( cs->last_compiled_jobs.size() < 7 )
          f *= ( -0.5 * cs->last_compiled_jobs.size() + 4.5 );

      return f;
    }
}

static void
handle_monitor_stats( CS *cs, StatsMsg *m = 0)
{
  if ( monitors.empty() )
    return;

  string msg;
  char buffer[1000];
  sprintf( buffer, "Name:%s\n", cs->nodename.c_str() );
  msg += buffer;
  sprintf( buffer, "IP:%s\n", cs->name.c_str() );
  msg += buffer;
  sprintf( buffer, "MaxJobs:%d\n", cs->max_jobs );
  msg += buffer;
  sprintf( buffer, "NoRemote:%s\n", cs->noremote ? "true" : "false"  );
  msg += buffer;
  sprintf( buffer, "Platform:%s\n", cs->host_platform.c_str() );
  msg += buffer;
  sprintf( buffer, "Speed:%f\n", server_speed( cs ) );
  msg += buffer;
  if ( m )
    {
      sprintf( buffer, "Load:%d\n", m->load );
      msg += buffer;
      sprintf( buffer, "LoadAvg1:%d\n", m->loadAvg1 );
      msg += buffer;
      sprintf( buffer, "LoadAvg5:%d\n", m->loadAvg5 );
      msg += buffer;
      sprintf( buffer, "LoadAvg10:%d\n", m->loadAvg10 );
      msg += buffer;
      sprintf( buffer, "FreeMem:%d\n", m->freeMem );
      msg += buffer;
    }
  else
    {
      sprintf( buffer, "Load:%d\n", cs->load );
      msg += buffer;
    }
  notify_monitors( new MonStatsMsg( cs->hostid, msg ) );
}

static Job *
create_new_job (CS *submitter)
{
  ++new_job_id;
  assert (jobs.find(new_job_id) == jobs.end());

  Job *job = new Job (new_job_id, submitter);
  jobs[new_job_id] = job;
  return job;
}

static void
enqueue_job_request (Job *job)
{
  if (!toanswer.empty() && toanswer.back()->server == job->submitter)
    {
      toanswer.back()->l.push_back (job);
    }
  else
    {
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
  toanswer.pop_front();
  first->l.pop_front();
  if (first->l.empty())
    {
      delete first;
    }
  else
    {
      toanswer.push_back( first );
    }
}

static string dump_job (Job *job);

static bool
handle_cs_request (MsgChannel *c, Msg *_m)
{
  GetCSMsg *m = dynamic_cast<GetCSMsg *>(_m);
  if (!m)
    return false;

  CS *submitter = static_cast<CS*>( c );

  Job *master_job = 0;

  for ( unsigned int i = 0; i < m->count; ++i )
    {
      Job *job = create_new_job (submitter);
      job->environments = m->versions;
      job->target_platform = m->target;
      job->arg_flags = m->arg_flags;
      job->language = ( m->lang == CompileJob::Lang_C ? "C" : "C++" );
      job->filename = m->filename;
      job->local_client_id = m->client_id;
      job->preferred_host = m->preferred_host;
      job->ignore_unverified = m->ignore_unverified;
      enqueue_job_request (job);
      std::ostream& dbg = log_info();
      dbg << "NEW " << job->id << " client="
                    << submitter->nodename << " versions=[";
      for ( Environments::const_iterator it = job->environments.begin();
            it != job->environments.end();)
        {
          dbg << it->second << "(" << it->first << ")";
          if (++it != job->environments.end())
            dbg << ", ";
        }
      dbg << "] " << m->filename << " " << job->language << endl;
      notify_monitors (new MonGetCSMsg (job->id, submitter->hostid, m));
      if ( !master_job )
        {
          master_job = job;
        }
      else
        {
          master_job->master_job_for.push_back( job );
        }
    }
  return true;

}

static bool
handle_local_job (CS *c, Msg *_m)
{
  JobLocalBeginMsg *m = dynamic_cast<JobLocalBeginMsg *>(_m);
  if (!m)
    return false;

  ++new_job_id;
  trace() << "handle_local_job " << m->outfile << " " << m->id << endl;
  c->client_map[m->id] = new_job_id;
  notify_monitors (new MonLocalJobBeginMsg( new_job_id, m->outfile, m->stime, c->hostid ) );
  return true;
}

static bool
handle_local_job_done (CS *c, Msg *_m)
{
  JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg *>(_m);
  if (!m)
    return false;

  trace() << "handle_local_job_done " << m->job_id << endl;
  notify_monitors (new JobLocalDoneMsg( c->client_map[m->job_id] ) );
  c->client_map.erase( m->job_id );
  return true;
}

static bool
platforms_compatible( const string &target, const string &platform )
{
  if ( target == platform )
    return true;
  // the below doesn't work as the unmapped platform is transferred back to the
  // client and that asks the daemon for a platform he can't install (see TODO)

  static multimap<string, string> platform_map;

  if (platform_map.empty())
    {
      platform_map.insert( make_pair( string( "i386" ), string( "i486" ) ) );
      platform_map.insert( make_pair( string( "i386" ), string( "i586" ) ) );
      platform_map.insert( make_pair( string( "i386" ), string( "i686" ) ) );
      platform_map.insert( make_pair( string( "i386" ), string( "x86_64" ) ) );

      platform_map.insert( make_pair( string( "i486" ), string( "i586" ) ) );
      platform_map.insert( make_pair( string( "i486" ), string( "i686" ) ) );
      platform_map.insert( make_pair( string( "i486" ), string( "x86_64" ) ) );

      platform_map.insert( make_pair( string( "i586" ), string( "i686" ) ) );
      platform_map.insert( make_pair( string( "i586" ), string( "x86_64" ) ) );

      platform_map.insert( make_pair( string( "i686" ), string( "x86_64" ) ) );

      platform_map.insert( make_pair( string( "ppc" ), string( "ppc64" ) ) );
      platform_map.insert( make_pair( string( "s390" ), string( "s390x" ) ) );
    }

  multimap<string, string>::const_iterator end = platform_map.upper_bound( target );
  for ( multimap<string, string>::const_iterator it = platform_map.lower_bound( target );
        it != end;
        ++it )
    {
      if ( it->second == platform )
        return true;
    }

  return false;
}

/* Given a candidate CS and a JOB, check all installed environments
   on the CS for a match.  Return an empty string if none of the required
   environments for this job is installed.  Otherwise return the
   host platform of the first found installed environment which is among
   the requested.  That can be send to the client, which then completely
   specifies which environment to use (name, host platform and target
   platform).  */
static string
envs_match( CS* cs, const Job *job )
{
  if ( job->submitter == cs)
    return cs->host_platform; // it will compile itself

  /* Check all installed envs on the candidate CS ...  */
  for ( Environments::const_iterator it = cs->compiler_versions.begin(); it != cs->compiler_versions.end(); ++it )
    {
      if ( it->first == job->target_platform )
        {
	  /* ... IT now is an installed environment which produces code for
	     the requested target platform.  Now look at each env which
	     could be installed from the client (i.e. those coming with the
	     job) if it matches in name and additionally could be run
	     by the candidate CS.  */
          for ( Environments::const_iterator it2 = job->environments.begin(); it2 != job->environments.end(); ++it2 )
            {
              if ( it->second == it2->second && platforms_compatible( it2->first, cs->host_platform ) )
                return it2->first;
            }
        }
    }
  return string();
}

static bool
blacklisted( CS* cs, const Job *job, const pair<string, string >& environment )
{
  list< pair< string, string > > &blacklist = job->submitter->blacklist[ cs ];
  return find(blacklist.begin(), blacklist.end(), environment ) != blacklist.end();
}

/* Given a candidate CS and a JOB, check if any of the requested
   environments could be installed on the CS.  This is the case if that
   env can be run there, i.e. if the host platforms of the CS and of the
   environment are compatible.  Return an empty string if none can be
   installed, otherwise return the platform of the first found
   environments which can be installed.  */
static string
can_install( CS* cs, const Job *job )
{
  // trace() << "can_install host: '" << cs->host_platform << "' target: '" << job->target_platform << "'" << endl;
  if ( cs->busy_installing )
    {
#if DEBUG_SCHEDULER > 0
      trace() << cs->nodename << " is busy installing since " << time(0) - cs->busy_installing << " seconds." << endl;
#endif
      return string();
    }

  for ( Environments::const_iterator it = job->environments.begin(); it != job->environments.end(); ++it )
    {
      if ( platforms_compatible( it->first, cs->host_platform ) && !blacklisted( cs, job, *it ))
        return it->first;
    }
  return string();
}

bool CS::check_remote( const Job *job ) const
{
    bool local = (job->submitter == this);
    return local || !noremote;
}

bool CS::is_eligible( const Job *job )
{
  bool jobs_okay = int( joblist.size() ) < max_jobs;
  bool load_okay = load < 1000;
  bool ignore = job->ignore_unverified && !IS_PROTOCOL_31(this);
  return jobs_okay
    && chroot_possible
    && load_okay
    && !ignore
    && can_install( this, job ).size()
    && this->check_remote( job );
}

static CS *
pick_server(Job *job)
{
  list<CS*>::iterator it;

#if DEBUG_SCHEDULER > 1
  trace() << "pick_server " << job->id << " " << job->target_platform << endl;
#endif

#if DEBUG_SCHEDULER > 0
  /* consistency checking for now */
  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
    {
      CS* cs= *it;
      for ( list<Job*>::const_iterator it2 = cs->joblist.begin(); it2 != cs->joblist.end(); ++it2 )
        {
          Job *job = *it2;
          assert( jobs.find( job->id ) != jobs.end() );
        }
    }
  for (map<unsigned int, Job*>::const_iterator it = jobs.begin();
       it != jobs.end(); ++it)
    {
      Job *j = it->second;

      CS *cs = j->server;
      assert( j->state != j->COMPILING ||
              find( cs->joblist.begin(),
                    cs->joblist.end(), j ) != cs->joblist.end() );
    }
#endif

  /* if the user wants to test/prefer one specific daemon, we look for that one first */
  if (!job->preferred_host.empty())
    {
	for (it = css.begin(); it != css.end(); ++it)
          {
	     if ((*it)->nodename == job->preferred_host
                 && (*it)->is_eligible( job ))
	       return *it;
	  }
        return 0;
    }

  /* If we have no statistics simply use the first server which is usable.  */
  if (!all_job_stats.size ())
    {
      for (it = css.begin(); it != css.end(); ++it)
        {
          trace() << "no job stats - looking at " << ( *it )->nodename << " load: " << (*it )->load << " can install: " << can_install( *it, job ) << endl;
          if ((*it)->is_eligible( job ))
            {
              trace() << "returning first " << ( *it )->nodename << endl;
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
  // best preloadable host
  CS *bestpre = 0;

  uint matches = 0;

  for (it = css.begin(); it != css.end(); ++it)
    {
      CS *cs = *it;
      /* For now ignore overloaded servers.  */
      /* Pre-loadable (cs->joblist.size()) == (cs->max_jobs) is checked later.  */
      if (int( cs->joblist.size() ) > cs->max_jobs || cs->load >= 1000)
        {
#if DEBUG_SCHEDULER > 1
          trace() << "overloaded " << cs->nodename << " " << cs->joblist.size() << "/" <<  cs->max_jobs << " jobs, load:" << cs->load << endl;
#endif
          continue;
      }

      // incompatible architecture or busy installing
      if ( !can_install( cs, job ).size() )
        {
#if DEBUG_SCHEDULER > 2
          trace() << cs->nodename << " can't install " << job->id << endl;
#endif
          continue;
        }

      /* Don't use non-chroot-able daemons for remote jobs.  XXX */
      if (!cs->chroot_possible)
        {
	  trace() << cs->nodename << " can't use chroot\n";
	  continue;
	}

      // Check if remote & if remote allowed
      if (!cs->check_remote( job ))
        {
	  trace() << cs->nodename << " fails remote job check\n";
	  continue;
	}


#if DEBUG_SCHEDULER > 1
      trace() << cs->nodename << " compiled " << cs->last_compiled_jobs.size() << " got now: " <<
        cs->joblist.size() << " speed: " << server_speed (cs) << " compile time " <<
        cs->cum_compiled.compile_time_user << " produced code " << cs->cum_compiled.osize << endl;
#endif

      if ( cs->last_compiled_jobs.size() == 0 && cs->joblist.size() == 0 && cs->max_jobs)
	{
	  /* Make all servers compile a job at least once, so we'll get an
	     idea about their speed.  */
	  if (!envs_match (cs, job).empty())
            {
              best = cs;
              matches++;
            }
	  else
            {
              // if there is one server that already got the environment and one that
              // hasn't compiled at all, pick the one with environment first
              bestui = cs;
            }
	  break;
	}

      if (!envs_match (cs, job).empty())
        {
          if ( !best )
            best = cs;
          /* Search the server with the earliest projected time to compile
             the job.  (XXX currently this is equivalent to the fastest one)  */
          else
            if (best->last_compiled_jobs.size() != 0
                && server_speed (best, job) < server_speed (cs, job))
              {
                if (int( cs->joblist.size() ) < cs->max_jobs)
                  best = cs;
                else
                  bestpre = cs;
              }
          matches++;
        }
      else
        {
          if ( !bestui )
            bestui = cs;
          /* Search the server with the earliest projected time to compile
             the job.  (XXX currently this is equivalent to the fastest one)  */
          else
            if (bestui->last_compiled_jobs.size() != 0
                && server_speed (bestui, job) < server_speed (cs, job))
              {
                if (int( cs->joblist.size() ) < cs->max_jobs)
                  bestui = cs;
                else
                  bestpre = cs;
              }
        }
    }

  // to make sure we find the fast computers at least after some time, we overwrite
  // the install rule for every 19th job - if the farm is only filled a bit
  if ( bestui && ( matches < 11 && matches < css.size() / 3 ) && job->id % 19 != 0 )
	best = 0;

  if ( best )
    {
#if DEBUG_SCHEDULER > 1
      trace() << "taking best installed " << best->nodename << " " <<  server_speed (best) << endl;
#endif
      return best;
    }

  if ( bestui )
    {
#if DEBUG_SCHEDULER > 1
      trace() << "taking best uninstalled " << bestui->nodename << " " <<  server_speed (bestui) << endl;
#endif
      return bestui;
    }

  if ( bestpre )
    {
#if DEBUG_SCHEDULER > 1
      trace() << "taking best preload " << bestui->nodename << " " <<  server_speed (bestui) << endl;
#endif
    }

  return bestpre;
}

/* Prunes the list of connected servers by those which haven't
   answered for a long time. Return the number of seconds when
   we have to cleanup next time. */
static time_t
prune_servers ()
{
  list<CS*>::iterator it;

  time_t now = time( 0 );
  time_t min_time = MAX_SCHEDULER_PING;

  for (it = controls.begin(); it != controls.end();)
    {
      if (now - ( *it )->last_talk >= MAX_SCHEDULER_PING) 
        {
	  CS *old = *it;
          ++it;
	  handle_end (old, 0);
	  continue;
        }
      min_time = min (min_time, MAX_SCHEDULER_PING - now + ( *it )->last_talk);
      ++it;
    }

  for (it = css.begin(); it != css.end(); )
    {
      if ((*it)->busy_installing && (now - (*it)->busy_installing >=
                                     MAX_BUSY_INSTALLING))
        {
	  trace() << "busy installing for a long time - removing " << ( *it )->nodename << endl;
	  CS *old = *it;
	  ++it;
	  handle_end (old, 0);
	  continue;
        }

      /* protocol version 27 and newer use TCP keepalive */
      if (IS_PROTOCOL_27(*it)) {
        ++it;
        continue;
      }

      if (now - ( *it )->last_talk >= MAX_SCHEDULER_PING)
        {
          if (( *it )->max_jobs >= 0)
            {
              trace() << "send ping " << ( *it )->nodename << endl;
              ( *it )->max_jobs *= -1; // better not give it away
              if(( *it )->send_msg( PingMsg() ))
		{
                  // give it MAX_SCHEDULER_PONG to answer a ping
                  ( *it )->last_talk = time( 0 ) - MAX_SCHEDULER_PING
		                       + 2 * MAX_SCHEDULER_PONG;
                  min_time = min (min_time, (time_t) 2 * MAX_SCHEDULER_PONG);
		  ++it;
		  continue;
		}
	    }
	  // R.I.P.
	  trace() << "removing " << ( *it )->nodename << endl;
	  CS *old = *it;
	  ++it;
	  handle_end (old, 0);
	  continue;
        }
      else
        min_time = min (min_time, MAX_SCHEDULER_PING - now + ( *it )->last_talk);
#if DEBUG_SCHEDULER > 1
      if ((random() % 400) < 0)
        { // R.I.P.
          trace() << "FORCED removing " << ( *it )->nodename << endl;
          CS *old = *it;
          ++it;
          handle_end (old, 0);
          continue;
        }
#endif

      ++it;
    }

    return min_time;
}

static Job*
delay_current_job()
{
  assert (!toanswer.empty());
  if ( toanswer.size() == 1 )
    return 0;

  UnansweredList *first = toanswer.front();
  toanswer.pop_front();
  toanswer.push_back( first );
  return get_job_request();
}

static bool
empty_queue()
{
  Job *job = get_job_request ();
  if (!job)
    return false;

  assert(!css.empty());

  Job *first_job = job;
  CS *cs = 0;

  while ( true )
    {
      cs = pick_server (job);

      if (cs)
        break;

      /* Ignore the load on the submitter itself if no other host could
         be found.  We only obey to its max job number.  */
      cs = job->submitter;
      if (! (int( cs->joblist.size() ) < cs->max_jobs
             && job->preferred_host.empty()
             /* This should be trivially true.  */
             && can_install (cs, job).size()))
        {
          job = delay_current_job();
          if ( job == first_job || !job ) // no job found in the whole toanswer list
            return false;
        }
      else
        {
          break;
        }
    }

  remove_job_request ();

  job->state = Job::WAITINGFORCS;
  job->server = cs;

  string host_platform = envs_match (cs, job);
  bool gotit = true;
  if (host_platform.empty ())
    {
      gotit = false;
      host_platform = can_install (cs, job);
    }

  // mix and match between job ids
  unsigned matched_job_id = 0;
  unsigned count = 0;
  for (list<JobStat>::const_iterator l = job->submitter->last_requested_jobs.begin();
       l != job->submitter->last_requested_jobs.end(); ++l)
    {
      unsigned rcount = 0;
      for (list<JobStat>::const_iterator r = cs->last_compiled_jobs.begin();
           r != cs->last_compiled_jobs.end(); ++r)
        {
          if (l->job_id == r->job_id)
            matched_job_id = l->job_id;
          if (++rcount > 16)
            break;
        }

       if (matched_job_id || ++count > 16)
         break;
    }

  UseCSMsg m2(host_platform, cs->name, cs->remote_port, job->id,
	      gotit, job->local_client_id, matched_job_id );

  if (!job->submitter->send_msg (m2))
    {
      trace() << "failed to deliver job " << job->id << endl;
      handle_end( job->submitter, 0 ); // will care for the rest
      return true;
    }
  else
    {
#if DEBUG_SCHEDULER >= 0
      if (!gotit)
	trace() << "put " << job->id << " in joblist of " << cs->nodename << " (will install now)" << endl;
      else
        trace() << "put " << job->id << " in joblist of " << cs->nodename << endl;
#endif
      cs->joblist.push_back( job );
      /* if it doesn't have the environment, it will get it. */
      if ( !gotit )
        cs->busy_installing = time(0);

      string env;
      if ( !job->master_job_for.empty() )
        {
          for ( Environments::const_iterator it = job->environments.begin(); it != job->environments.end(); ++it )
            {
              if ( it->first == cs->host_platform )
                {
                  env = it->second;
                  break;
                }
            }
        }
      if ( !env.empty() )
        {
          for ( list<Job*>::iterator it = job->master_job_for.begin(); it != job->master_job_for.end(); ++it )
            { // remove all other environments
              ( *it )->environments.clear();
              ( *it )->environments.push_back( make_pair( cs->host_platform, env ) );
            }
        }
    }
  return true;
}

static bool
handle_login (CS *cs, Msg *_m)
{
  LoginMsg *m = dynamic_cast<LoginMsg *>(_m);
  if (!m)
    return false;

  std::ostream& dbg = trace();

  cs->remote_port = m->port;
  cs->compiler_versions = m->envs;
  cs->max_jobs = m->chroot_possible ? m->max_kids : 0;
  cs->noremote = m->noremote;
  if ( m->nodename.length() )
    cs->nodename = m->nodename;
  else
    cs->nodename = cs->name;
  cs->host_platform = m->host_platform;
  cs->chroot_possible = m->chroot_possible;
  cs->pick_new_id();

  for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it)
      if (cs->name == *it)
          return false;

  dbg << "login " << m->nodename << " protocol version: " << cs->protocol;
#if 1
  dbg << " [";
  for (Environments::const_iterator it = m->envs.begin();
       it != m->envs.end(); ++it)
    dbg << it->second << "(" << it->first << "), ";
  dbg << "]" << endl;
#endif

  handle_monitor_stats( cs );

  /* remove any other clients with the same IP, they must be stale */
  for (list<CS*>::iterator it = css.begin(); it != css.end(); )
    {
      if (cs->eq_ip(*(*it)))
      {
        CS* old = *it;
        ++it;
        handle_end(old, 0);
        continue;
      }
      ++it;
    }

  css.push_back (cs);

  /* Configure the daemon */
  if (IS_PROTOCOL_24( cs ))
    cs->send_msg (ConfCSMsg(icecream_bench_code));

  return true;
}

static bool
handle_relogin (MsgChannel *c, Msg *_m)
{
  LoginMsg *m = dynamic_cast<LoginMsg *>(_m);
  if (!m)
    return false;

  CS *cs = static_cast<CS *>(c);
  cs->compiler_versions = m->envs;
  cs->busy_installing = 0;

  std::ostream &dbg = trace();
  dbg << "RELOGIN " << cs->nodename << "(" << cs->host_platform << "): [";
  for (Environments::const_iterator it = m->envs.begin();
       it != m->envs.end(); ++it)
    dbg << it->second << "(" << it->first << "), ";
  dbg << "]" << endl;

  /* Configure the daemon */
  if (IS_PROTOCOL_24( c ))
    c->send_msg (ConfCSMsg(icecream_bench_code));

  return false;
}

static bool
handle_mon_login (CS *c, Msg *_m)
{
  MonLoginMsg *m = dynamic_cast<MonLoginMsg *>(_m);
  if (!m)
    return false;
  monitors.push_back (c);
  // monitors really want to be fed lazily
  c->setBulkTransfer();

  for (list<CS*>::const_iterator it = css.begin(); it != css.end(); ++it)
    handle_monitor_stats( *it );

  fd2cs.erase( c->fd ); // no expected data from them
  return true;
}

static bool
handle_job_begin (CS *c, Msg *_m)
{
  JobBeginMsg *m = dynamic_cast<JobBeginMsg *>(_m);
  if ( !m )
    return false;

  if (jobs.find(m->job_id) == jobs.end()) {
    trace() << "handle_job_begin: no valid job id " << m->job_id << endl;
    return false;
  }
  Job *job = jobs[m->job_id];
  if (job->server != c)
    {
      trace() << "that job isn't handled by " << c->name << endl;
      return false;
    }
  job->state = Job::COMPILING;
  job->starttime = m->stime;
  job->start_on_scheduler = time(0);
  notify_monitors (new MonJobBeginMsg (m->job_id, m->stime, c->hostid));
#if DEBUG_SCHEDULER >= 0
  trace() << "BEGIN: " << m->job_id << " client=" << job->submitter->nodename
          << "(" << job->target_platform << ")" << " server="
          << job->server->nodename << "(" << job->server->host_platform
          << ")" << endl;
#endif

  return true;
}


static bool
handle_job_done (CS *c, Msg *_m)
{
  JobDoneMsg *m = dynamic_cast<JobDoneMsg *>(_m);
  if ( !m )
    return false;

  Job *j = 0;

  if (m->exitcode == CLIENT_WAS_WAITING_FOR_CS)
    {
      // the daemon saw a cancel of what he believes is waiting in the scheduler
      map<unsigned int, Job*>::iterator mit;
      for (mit = jobs.begin(); mit != jobs.end(); ++mit)
        {
          Job *job = mit->second;
          trace() << "looking for waitcs " << job->server << " " << job->submitter  << " " << c << " "
		  << job->state << " " << job->local_client_id << " " << m->job_id << endl;
          if (job->server == 0 && job->submitter == c && job->state == Job::PENDING
              && job->local_client_id == m->job_id )
            {
              trace() << "STOP (WAITFORCS) FOR " << mit->first << endl;
              j = job;
              m->job_id = j->id; // that's faked

	      /* Unfortunately the toanswer queues are also tagged based on the daemon,
		 so we need to clean them up also.  */
	      list<UnansweredList*>::iterator it;
	      for (it = toanswer.begin(); it != toanswer.end(); ++it)
		if ((*it)->server == c)
		  {
		    UnansweredList *l = *it;
		    list<Job*>::iterator jit;
		    for (jit = l->l.begin(); jit != l->l.end(); ++jit)
		      {
			if (*jit == j)
			  {
			    l->l.erase(jit);
			    break;
			  }
		      }
		    if (l->l.empty())
		      {
			it = toanswer.erase (it);
			break;
		      }
		  }
	    }
	}
    }
  else
    if (jobs.find(m->job_id) != jobs.end())
      j = jobs[m->job_id];

  if (!j)
    {
      trace() << "job ID not present " << m->job_id << endl;
      return false;
    }

  if (j->state == Job::PENDING)
    {
      trace() << "job ID still pending ?! scheduler recently restarted? " << m->job_id << endl;
      return false;
    }

  if (m->is_from_server() && j->server != c)
    {
      log_info() << "the server isn't the same for job " << m->job_id << endl;
      log_info() << "server: " << j->server->nodename << endl;
      log_info() << "msg came from: " << c->nodename << endl;
      // the daemon is not following matz's rules: kick him
      handle_end(c, 0);
      return false;
    }
  if (!m->is_from_server() && j->submitter != c)
    {
      log_info() << "the submitter isn't the same for job " << m->job_id << endl;
      log_info() << "submitter: " << j->submitter->nodename << endl;
      log_info() << "msg came from: " << c->nodename << endl;
      // the daemon is not following matz's rules: kick him
      handle_end(c, 0);
      return false;
    }



  if ( m->exitcode == 0 )
    {
      std::ostream &dbg = trace();
      dbg << "END " << m->job_id
	  << " status=" << m->exitcode;

      if ( m->in_uncompressed )
	dbg << " in=" << m->in_uncompressed
	    << "(" << int( m->in_compressed * 100 / m->in_uncompressed ) << "%)";
      else
	dbg << " in=0(0%)";

      if ( m->out_uncompressed )
	dbg << " out=" << m->out_uncompressed
	    << "(" << int( m->out_compressed * 100 / m->out_uncompressed ) << "%)";
      else
	dbg << " out=0(0%)";

      dbg << " real=" << m->real_msec
	  << " user=" << m->user_msec
	  << " sys=" << m->sys_msec
	  << " pfaults=" << m->pfaults
	  << " server=" << j->server->nodename
	  << endl;
    }
  else
    trace() << "END " << m->job_id
	    << " status=" << m->exitcode << endl;

  if (j->server)
      j->server->joblist.remove (j);

  add_job_stats (j, m);
  notify_monitors (new MonJobDoneMsg (*m));
  jobs.erase (m->job_id);
  delete j;

  return true;
}

static bool
handle_ping (CS* c, Msg * /*_m*/)
{
  c->last_talk = time( 0 );
  if ( c->max_jobs < 0 )
    c->max_jobs *= -1;
  return true;
}

static bool
handle_stats (CS * c, Msg * _m)
{
  StatsMsg *m = dynamic_cast<StatsMsg *>(_m);
  if (!m)
    return false;

  /* Before protocol 25, ping and stat handling was
     clutched together.  */
  if (!IS_PROTOCOL_25(c))
    {
      c->last_talk = time( 0 );
      if ( c && c->max_jobs < 0 )
        c->max_jobs *= -1;
    }

  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
    if ( *it == c )
      {
        ( *it )->load = m->load;
        handle_monitor_stats( *it, m );
        return true;
      }

  return false;
}

static bool
handle_blacklist_host_env(CS * c, Msg * _m)
{
  BlacklistHostEnvMsg *m = dynamic_cast<BlacklistHostEnvMsg *>(_m);
  if (!m)
    return false;
  for (list<CS*>::const_iterator it = css.begin(); it != css.end(); ++it)
    if ( (*it)->name == m->hostname )
      {
        trace() << "Blacklisting host " << m->hostname << " for environment " << m->environment
             << " (" << m->target << ")" << endl;
        c->blacklist[ *it ].push_back( make_pair( m->target, m->environment ));
      }
  return true;
}

static string
dump_job (Job *job)
{
  char buffer[1000];
  string line;
  snprintf (buffer, sizeof (buffer), "%d %s sub:%s on:%s ",
	   job->id,
	   job->state == Job::PENDING ? "PEND"
	     : job->state == Job::WAITINGFORCS ? "WAIT"
	     : job->state == Job::COMPILING ? "COMP"
	     : "Huh?",
	   job->submitter ? job->submitter->nodename.c_str() : "<>",
	   job->server ? job->server->nodename.c_str() : "<unknown>");
  buffer[sizeof (buffer) - 1] = 0;
  line = buffer;
  line = line + job->filename;
  return line;
}

/* Splits the string S between characters in SET and add them to list L.  */
static void
split_string (const string &s, const char *set, list<string> &l)
{
  string::size_type end = 0;
  while (end != string::npos)
    {
      string::size_type start = s.find_first_not_of (set, end);
      if (start == string::npos)
        break;
      end = s.find_first_of (set, start);
      /* Do we really need to check end here or is the subtraction
         defined on every platform correctly (with GCC it's ensured,
	 that (npos - start) is the rest of the string).  */
      if (end != string::npos)
        l.push_back (s.substr (start, end - start));
      else
        l.push_back (s.substr (start));
    }
}

static bool
handle_control_login(CS* c)
{
    c->type = CS::LINE;
    c->last_talk = time (0);
    c->setBulkTransfer();
    c->state = CS::LOGGEDIN;
    assert(find(controls.begin(), controls.end(), c) == controls.end());
    controls.push_back(c);

    std::ostringstream o;
    o << "200-ICECC " VERSION ": "
      << time(0) - starttime << "s uptime, "
      << css.size() << " hosts, "
      << jobs.size() << " jobs in queue "
      << "(" << new_job_id << " total)." << endl;
    o << "200 Use 'help' for help and 'quit' to quit." << endl;
    return c->send_msg(TextMsg(o.str()));
}

static bool
handle_line (CS *c, Msg *_m)
{
  TextMsg *m = dynamic_cast<TextMsg *>(_m);
  if (!m)
    return false;

  char buffer[1000];
  string line;
  list<string> l;
  split_string (m->text, " \t\n", l);
  string cmd;

  c->last_talk = time(0);

  if (l.empty())
    cmd = "";
  else
    {
      cmd = l.front();
      l.pop_front();
      transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    }
  if (cmd == "listcs")
    {
      for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
	{
	  CS* cs= *it;
	  sprintf (buffer, " (%s:%d) ", cs->name.c_str(), cs->remote_port);
	  line = " " + cs->nodename + buffer;
	  line += "[" + cs->host_platform + "] speed=";
	  sprintf (buffer, "%.2f jobs=%d/%d load=%d", server_speed (cs),
	  	   (int)cs->joblist.size(), cs->max_jobs, cs->load);
	  line += buffer;
          if (cs->busy_installing)
            {
              sprintf( buffer, " busy installing since %ld s",  time(0) - cs->busy_installing );
              line += buffer;
            }
	  if(!c->send_msg (TextMsg (line)))
            return false;
          for ( list<Job*>::const_iterator it2 = cs->joblist.begin(); it2 != cs->joblist.end(); ++it2 )
            if(!c->send_msg (TextMsg ("   " + dump_job (*it2) ) ))
              return false;
	}
    }
  else if (cmd == "listblocks")
    {
      for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it)
        if(!c->send_msg (TextMsg ("   " + (*it) ) ))
          return false;
    }
  else if (cmd == "listjobs")
    {
      for (map<unsigned int, Job*>::const_iterator it = jobs.begin();
	   it != jobs.end(); ++it)
	if(!c->send_msg( TextMsg( " " + dump_job (it->second) ) ))
          return false;
    }
  else if (cmd == "quit" || cmd == "exit" )
    {
      handle_end(c, 0);
      return false;
    }
  else if (cmd == "removecs" || cmd == "blockcs")
    {
      if (l.empty()) {
        if(!c->send_msg (TextMsg (string ("401 Sure. But which hosts?"))))
          return false;
      }
      else
        for (list<string>::const_iterator si = l.begin(); si != l.end(); ++si)
	  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
	    if ((*it)->nodename == *si || (*it)->name == *si)
	      {
                if (cmd == "blockcs")
                    block_css.push_back((*it)->name);
                if (c->send_msg (TextMsg (string ("removing host ") + *si)))
                    handle_end ( *it, 0);
		break;
	      }
    }
  else if (cmd == "crashme")
    {
      *(volatile int *)0 = 42;  // ;-)
    }
  else if (cmd == "internals" )
    {
      for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
        {
          Msg *msg = NULL;

	  if (!l.empty())
	    {
	      list<string>::const_iterator si;
	      for (si = l.begin(); si != l.end(); ++si) {
	        if ((*it)->nodename == *si || (*it)->name == *si)
		  break;
              }
	      if(si == l.end())
		continue;
	    }

          if(( *it )->send_msg( GetInternalStatus() ))
              msg = ( *it )->get_msg();
          if ( msg && msg->type == M_STATUS_TEXT ) {
            if (!c->send_msg( TextMsg( dynamic_cast<StatusTextMsg*>( msg )->text ) ))
              return false;
          }
          else {
            if (!c->send_msg( TextMsg( ( *it )->nodename + " not reporting\n" ) ))
              return false;
          }
	  delete msg;
        }
    }
  else if (cmd == "help")
    {
      if (!c->send_msg (TextMsg (
        "listcs\nlistblocks\nlistjobs\nremovecs\nblockcs\ninternals\nhelp\nquit")))
        return false;
    }
  else
    {
      string txt = "Invalid command '";
      txt += m->text;
      txt += "'";
      if(!c->send_msg (TextMsg (txt)))
        return false;
    }
  return c->send_msg (TextMsg (string ("200 done")));
}

// return false if some error occurred, leaves C open.  */
static bool
try_login (CS *c, Msg *m)
{
  bool ret = true;
  switch (m->type)
    {
    case M_LOGIN:
      c->type = CS::DAEMON;
      ret = handle_login (c, m);
      break;
    case M_MON_LOGIN:
      c->type = CS::MONITOR;
      ret = handle_mon_login (c, m);
      break;
    default:
      log_info() << "Invalid first message " << (char)m->type << endl;
      ret = false;
      break;
    }
  if (ret)
    c->state = CS::LOGGEDIN;
  else
    handle_end (c, m);

  delete m;
  return ret;
}

static bool
handle_end (CS *toremove, Msg *m)
{
#if DEBUG_SCHEDULER > 1
  trace() << "Handle_end " << toremove << " " << m << endl;
#else
  ( void )m;
#endif

  switch (toremove->type) {
  case CS::MONITOR:
    {
      assert (find (monitors.begin(), monitors.end(), toremove) != monitors.end());
      monitors.remove (toremove);
#if DEBUG_SCHEDULER > 1
      trace() << "handle_end(moni) " << monitors.size() << endl;
#endif
    }
    break;
  case CS::DAEMON:
    {
      log_info() << "remove daemon " << toremove->nodename << endl;

      notify_monitors(new  MonStatsMsg( toremove->hostid, "State:Offline\n" ) );

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
		trace() << "STOP (DAEMON) FOR " << (*jit)->id << endl;
                notify_monitors (new MonJobDoneMsg ( JobDoneMsg (( *jit )->id,  255 )));
		if ((*jit)->server)
		  (*jit)->server->busy_installing = 0;
		jobs.erase( (*jit)->id );
		delete (*jit);
	      }
	    delete l;
	    it = toanswer.erase (it);
	  }
	else
	  ++it;

      map<unsigned int, Job*>::iterator mit;
      for (mit = jobs.begin(); mit != jobs.end(); )
        {
          Job *job = mit->second;
          if (job->server == toremove || job->submitter == toremove)
            {
              trace() << "STOP (DAEMON2) FOR " << mit->first << endl;
              notify_monitors (new MonJobDoneMsg ( JobDoneMsg( job->id,  255 )));
	      /* If this job is removed because the submitter is removed
		 also remove the job from the servers joblist.  */
	      if (job->server && job->server != toremove)
		job->server->joblist.remove (job);
	      if (job->server)
	        job->server->busy_installing = 0;
              jobs.erase( mit++ );
              delete job;
            }
          else
            {
              ++mit;
            }
        }
      for( list<CS*>::iterator it = css.begin(); it != css.end(); ++it )
        (*it)->blacklist.erase( toremove );
    }
    break;
  case CS::LINE:
    {
      if (!toremove->send_msg (TextMsg ("200 Good Bye!"))) {
      }
      controls.remove (toremove);
    }
    break;
  default:
    {
      trace() << "remote end had UNKNOWN type?" << endl;
    }
  }

  fd2cs.erase (toremove->fd);
  delete toremove;
  return true;
}

/* Returns TRUE if C was not closed.  */
static bool
handle_activity (CS *c)
{
  Msg *m;
  bool ret = true;
  m = c->get_msg (0);
  if (!m)
    {
      handle_end (c, m);
      return false;
    }
  /* First we need to login.  */
  if (c->state == CS::CONNECTED)
    return try_login (c, m);

  switch (m->type)
    {
    case M_JOB_BEGIN: ret = handle_job_begin (c, m); break;
    case M_JOB_DONE: ret = handle_job_done (c, m); break;
    case M_PING: ret = handle_ping (c, m); break;
    case M_STATS: ret = handle_stats (c, m); break;
    case M_END: handle_end (c, m); ret = false; break;
    case M_JOB_LOCAL_BEGIN: ret = handle_local_job (c, m); break;
    case M_JOB_LOCAL_DONE: ret = handle_local_job_done( c, m ); break;
    case M_LOGIN: ret = handle_relogin (c, m); break;
    case M_TEXT: ret = handle_line (c, m); break;
    case M_GET_CS: ret = handle_cs_request (c, m); break;
    case M_BLACKLIST_HOST_ENV: ret = handle_blacklist_host_env(c, m); break;
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
      log_perror ("socket()");
      return -1;
    }
  int optval = 1;
  if (setsockopt (listen_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0)
    {
      log_perror ("setsockopt()");
      return -1;
    }
  myaddr.sin_family = AF_INET;
  myaddr.sin_port = htons (8765);
  myaddr.sin_addr.s_addr = INADDR_ANY;
  if (bind (listen_fd, (struct sockaddr *) &myaddr, sizeof (myaddr)) < 0)
    {
      log_perror ("bind()");
      return -1;
    }
  return listen_fd;
}

static int
open_tcp_listener (short port)
{
  int fd;
  struct sockaddr_in myaddr;
  if ((fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      log_perror ("socket()");
      return -1;
    }
  int optval = 1;
  if (setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
      log_perror ("setsockopt()");
      return -1;
    }
  /* Although we select() on fd we need O_NONBLOCK, due to
     possible network errors making accept() block although select() said
     there was some activity.  */
  if (fcntl (fd, F_SETFL, O_NONBLOCK) < 0)
    {
      log_perror ("fcntl()");
      return -1;
    }
  myaddr.sin_family = AF_INET;
  myaddr.sin_port = htons (port);
  myaddr.sin_addr.s_addr = INADDR_ANY;
  if (bind (fd, (struct sockaddr *) &myaddr, sizeof (myaddr)) < 0)
    {
      log_perror ("bind()");
      return -1;
    }
  if (listen (fd, 10) < 0)
    {
      log_perror ("listen()");
      return -1;
    }
  return fd;
}

static void
usage(const char* reason = 0)
{
  if (reason)
     cerr << reason << endl;

  cerr << "ICECREAM scheduler " VERSION "\n";
  cerr << "usage: icecc-scheduler [options] \n"
       << "Options:\n"
       << "  -n, --netname <name>\n"
       << "  -p, --port <port>\n"
       << "  -h, --help\n"
       << "  -l, --log-file <file>\n"
       << "  -d, --daemonize\n"
       << "  -u, --user-uid\n"
       << "  -v[v[v]]]\n"
       << endl;

  exit(1);
}

static void
trigger_exit( int signum )
{
  if (!exit_main_loop)
    exit_main_loop = true;
  else
    {
      // hmm, we got killed already. try better
      cerr << "forced exit." << endl;
      _exit(1);
    }
  // make BSD happy
  signal(signum, trigger_exit);
}

int
main (int argc, char * argv[])
{
  int listen_fd, remote_fd, broad_fd, text_fd;
  struct sockaddr_in remote_addr;
  unsigned int port = 8765;
  socklen_t remote_len;
  char *netname = (char*)"ICECREAM";
  bool detach = false;
  int debug_level = Error;
  string logfile;
  uid_t user_uid;
  gid_t user_gid;
  bool warn_icecc_user = false;

  if ( getuid() == 0 )
    {
    struct passwd *pw = getpwnam("icecc");
    if (pw)
      {
        user_uid = pw->pw_uid;
        user_gid = pw->pw_gid;
      }
    else
      {
        warn_icecc_user = true;
        user_uid = 65534;
        user_gid = 65533;
      }
    }
  else
    {
      user_uid = getuid();
      user_gid = getgid();
    }

  while ( true ) {
    int option_index = 0;
    static const struct option long_options[] = {
      { "netname", 1, NULL, 'n' },
      { "help", 0, NULL, 'h' },
      { "port", 0, NULL, 'p' },
      { "daemonize", 0, NULL, 'd'},
      { "log-file", 1, NULL, 'l'},
      { "user-uid", 1, NULL, 'u'},
      { 0, 0, 0, 0 }
    };

    const int c = getopt_long( argc, argv, "n:p:hl:vdr:u:", long_options, &option_index );
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
    case 'u':
        if ( optarg && *optarg )
          {
            struct passwd *pw = getpwnam( optarg );
            if ( !pw )
              {
                usage( "Error: -u requires a valid username" );
               }
            else
              {
                user_uid = pw->pw_uid;
                user_gid = pw->pw_gid;
                warn_icecc_user = false;
                if ( !user_gid || !user_uid )
                    usage( "Error: -u <username> must not be root" );
               }
           }
        else
          {
            usage( "Error: -u requires a valid username" );
           }
        break;

    default:
      usage();
    }
  }

  if (warn_icecc_user)
    log_perror ("Error: no icecc user on system. Falling back to nobody.");

  if ( getuid() == 0 )
    {
      if ( !logfile.size() && detach ) {
        if (mkdir("/var/log/icecc", S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)) {
          if (errno == EEXIST) {
              chmod("/var/log/icecc", S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
              chown("/var/log/icecc", user_uid, user_gid);
          }
        }

        logfile = "/var/log/icecc/scheduler.log";
      }

      if ( setgid( user_gid ) < 0 )
        {
          log_perror("setgid() failed" );
          return 1;
         }

      if ( setuid( user_uid ) < 0 )
        {
          log_perror("setuid() failed" );
          return 1;
         }
     }

  setup_debug( debug_level, logfile );
  if ( detach )
    daemon( 0, 0 );

  listen_fd = open_tcp_listener (port);
  if (listen_fd < 0)
    return 1;
  text_fd = open_tcp_listener (port + 1);
  if (text_fd < 0)
    return 1;
  broad_fd = open_broad_listener ();
  if (broad_fd < 0)
    return 1;

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
      log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
      return 1;
    }

  starttime = time( 0 );

  ofstream pidFile;
  string progName = argv[0];
  progName = progName.substr(progName.rfind('/')+1);
  pidFilePath = string(RUNDIR)+string("/")+progName+string(".pid");
  pidFile.open(pidFilePath.c_str());
  pidFile << getpid() << endl;
  pidFile.close();

  signal(SIGTERM, trigger_exit);
  signal(SIGINT, trigger_exit);
  signal(SIGALRM, trigger_exit);

  time_t next_listen = 0;

  while (!exit_main_loop)
    {
      struct timeval tv;
      tv.tv_usec = 0;
      tv.tv_sec = prune_servers ();

      while (empty_queue())
	continue;

      fd_set read_set;
      int max_fd = 0;
      FD_ZERO (&read_set);
      if (time(0) >= next_listen)
        {
          max_fd = listen_fd;
          FD_SET (listen_fd, &read_set);
          if (text_fd > max_fd)
            max_fd = text_fd;
          FD_SET (text_fd, &read_set);
        }
      if (broad_fd > max_fd)
        max_fd = broad_fd;
      FD_SET (broad_fd, &read_set);
      for (map<int, CS*>::const_iterator it = fd2cs.begin(); it != fd2cs.end();)
        {
          int i = it->first;
          CS *c = it->second;
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
      max_fd = select (max_fd + 1, &read_set, NULL, NULL, &tv);
      if (max_fd < 0 && errno == EINTR)
        continue;
      if (max_fd < 0)
        {
	  log_perror ("select()");
	  return 1;
	}
      if (FD_ISSET (listen_fd, &read_set))
        {
	  max_fd--;
          bool pending_connections = true;
          while (pending_connections)
            {
              remote_len = sizeof (remote_addr);
              remote_fd = accept (listen_fd,
                                  (struct sockaddr *) &remote_addr,
                                  &remote_len );
              if (remote_fd < 0)
                pending_connections = false;

              if (remote_fd < 0 && errno != EAGAIN && errno != EINTR && errno
                  != EWOULDBLOCK)
                {
                  log_perror ("accept()");
                  /* don't quit because of ECONNABORTED, this can happen during
                   * floods  */
                }
              if (remote_fd >= 0)
                {
                  CS *cs = new CS (remote_fd, (struct sockaddr*) &remote_addr, remote_len, false);
                  trace() << "accepted " << cs->name << endl;
                  cs->last_talk = time( 0 );

                  if ( !cs->protocol ) // protocol mismatch
                    {
                      delete cs;
                      continue;
                    }
                  fd2cs[cs->fd] = cs;
                  while (!cs->read_a_bit () || cs->has_msg ())
                    if(! handle_activity (cs))
                      break;
                }
            }
          next_listen = time(0) + 1;
        }
      if (max_fd && FD_ISSET (text_fd, &read_set))
        {
	  max_fd--;
	  remote_len = sizeof (remote_addr);
          remote_fd = accept (text_fd,
                              (struct sockaddr *) &remote_addr,
                              &remote_len );
	  if (remote_fd < 0 && errno != EAGAIN && errno != EINTR)
	    {
	      log_perror ("accept()");
	      /* Don't quit the scheduler just because a debugger couldn't
	         connect.  */
	    }
	  if (remote_fd >= 0)
	    {
	      CS *cs = new CS (remote_fd, (struct sockaddr*) &remote_addr, remote_len, true);
	      fd2cs[cs->fd] = cs;
              if (!handle_control_login(cs))
                {
                  handle_end(cs, 0);
                  continue;
                }
	      while (!cs->read_a_bit () || cs->has_msg ())
	        if (!handle_activity (cs))
                  break;
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
	      log_perror ("recvfrom()");
	      /* Some linux 2.6 kernels can return from select with
	         data available, and then return from read() with EAGAIN
		 even on a blocking socket (breaking POSIX).  Happens
		 when the arriving packet has a wrong checksum.  So
		 we ignore EAGAIN here, but still abort for all other errors. */
	      if (err != EAGAIN)
	        return -1;
	    }
            /* Only answer if daemon would be able to talk to us. */
	  else if (buf[0] >= MIN_PROTOCOL_VERSION)
	    {
	      log_info() << "broadcast from " << inet_ntoa (broad_addr.sin_addr)
                         << ":" << ntohs (broad_addr.sin_port) << "\n";
	      buf[0]++;
	      memset (buf + 1, 0, sizeof (buf) - 1);
	      snprintf (buf + 1, sizeof (buf) - 1, "%s", netname);
	      buf[sizeof (buf) - 1] = 0;
	      if (sendto (broad_fd, buf, sizeof (buf), 0,
	      		  (struct sockaddr*)&broad_addr, broad_len) != sizeof (buf))
		{
		  log_perror ("sendto()");
		}
	    }
	}
      for (map<int, CS*>::const_iterator it = fd2cs.begin();
           max_fd && it != fd2cs.end();)
        {
          int i = it->first;
          CS *c = it->second;
          /* handle_activity can delete the channel from the fd2cs list,
             hence advance the iterator right now, so it doesn't become
             invalid.  */
          ++it;
          if (FD_ISSET (i, &read_set))
            {
              while (!c->read_a_bit () || c->has_msg ())
                if(!handle_activity (c))
                  break;
              max_fd--;
            }
        }
    }
  shutdown (broad_fd, SHUT_RDWR);
  close (broad_fd);
  unlink(pidFilePath.c_str());
  return 0;
}

