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
#include "job.h"
#include "config.h"

#define DEBUG_SCHEDULER 2

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
  JobStat() : osize(0), compile_time_real(0), compile_time_user(0),
	      compile_time_sys(0) {}
  JobStat& operator +=(const JobStat &st)
  {
    osize += st.osize;
    compile_time_real += st.compile_time_real;
    compile_time_user += st.compile_time_user;
    compile_time_sys += st.compile_time_sys;
    return *this;
  }
  JobStat& operator -=(const JobStat &st)
  {
    osize -= st.osize;
    compile_time_real -= st.compile_time_real;
    compile_time_user -= st.compile_time_user;
    compile_time_sys -= st.compile_time_sys;
    return *this;
  }
  JobStat& operator /=(int d)
  {
    osize /= d;
    compile_time_real /= d;
    compile_time_user /= d;
    compile_time_sys /= d;
    return *this;
  }
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
  list<Job*> joblist;
  Environments compiler_versions;  // Available compilers
  CS (int fd, struct sockaddr *_addr, socklen_t _len, bool text_based)
    : MsgChannel(fd, _addr, _len, text_based), load(1000), max_jobs(0), state(CONNECTED),
      type(UNKNOWN), chroot_possible(false)
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
};

unsigned int CS::hostid_counter = 0;

static map<int, MsgChannel *> fd2chan;
static bool allow_run_as_user = false;

time_t starttime;

class Job
{
public:
  unsigned int id;
  unsigned int local_client_id;
  enum {PENDING, WAITINGFORCS, COMPILING, WAITINGFORDONE} state;
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
  Job (unsigned int _id, CS *subm)
    : id(_id), local_client_id( 0 ), state(PENDING), server(0),
      submitter(subm),
      starttime(0), start_on_scheduler(0), done_time( 0 ), arg_flags( 0 ) {}
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
static map<unsigned int, Job*> done_jobs;

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

static list<MsgChannel*> monitors;

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
  if (msg->out_uncompressed < 512
      || msg->exitcode != 0)
    return;

  st.osize = msg->out_uncompressed;
  st.compile_time_real = msg->real_msec;
  st.compile_time_user = msg->user_msec;
  st.compile_time_sys = msg->sys_msec;

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

static bool handle_end (MsgChannel *c, Msg *);

static void
notify_monitors (const Msg &m)
{
  list<MsgChannel*>::iterator it, it_old;
  for (it = monitors.begin(); it != monitors.end();)
    {
      it_old = it++; // handle_end removes it from monitors, so don't be clever
      /* If we can't send it, don't be clever, simply close this monitor.  */
      if (!(*it_old)->send_msg (m))
        handle_end (*it_old, 0);
    }
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
        /* The submitter of a job gets more speed.  So if he is equally
           fast to the rest of the farm it will be prefered to chose him
           to compile the job.  Then this can be done locally without
           needing the preprocessor.  */
        if (job->submitter == cs)
          f *= 1.2;
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
  notify_monitors( MonStatsMsg( cs->hostid, msg ) );
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
      enqueue_job_request (job);
      log_info() << "NEW " << job->id << " client="
                 << submitter->nodename << " versions=[";
      for ( Environments::const_iterator it = job->environments.begin();
            it != job->environments.end(); ++it )
        log_info() << it->second << "(" << it->first << "), ";
      log_info() << "] " << m->filename << " " << job->language << endl;
      notify_monitors (MonGetCSMsg (job->id, submitter->hostid, m));
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
handle_local_job (MsgChannel *c, Msg *_m)
{
  JobLocalBeginMsg *m = dynamic_cast<JobLocalBeginMsg *>(_m);
  if (!m)
    return false;

  ++new_job_id;
  trace() << "handle_local_job " << m->outfile << " " << m->id << endl;
  CS *cs = dynamic_cast<CS*>( c );
  cs->client_map[m->id] = new_job_id;
  notify_monitors (MonLocalJobBeginMsg( new_job_id, m->outfile, m->stime, cs->hostid ) );
  return true;
}

static bool
handle_local_job_done (MsgChannel *c, Msg *_m)
{
  JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg *>(_m);
  if (!m)
    return false;

  trace() << "handle_local_job_done " << m->job_id << endl;
  CS *cs = dynamic_cast<CS*>( c );
  notify_monitors (JobLocalDoneMsg( cs->client_map[m->job_id] ) );
  cs->client_map.erase( m->job_id );
  return true;
}

static bool
platforms_compatible( const string &target, const string &platform )
{
  if ( target == platform )
    return true;
  // the below doesn't work as the unmapped platform is transfered back to the
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

static bool
envs_match( CS* cs, const Job *job )
{
  if ( job->submitter == cs)
    return true; // it will compile itself

  for ( Environments::const_iterator it = cs->compiler_versions.begin(); it != cs->compiler_versions.end(); ++it )
    {
      if ( platforms_compatible( it->first, job->target_platform ) )
        {
          for ( Environments::const_iterator it2 = job->environments.begin(); it2 != job->environments.end(); ++it2 )
            {
              if ( it->second == it2->second && platforms_compatible( it2->first, cs->host_platform ) )
                return true;
            }
        }
    }
  return false;
}

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
      if ( platforms_compatible( it->first, cs->host_platform ) )
        return it->first;
    }
  return string();
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
                 && int( (*it)->joblist.size() ) < (*it)->max_jobs
                 && (*it)->chroot_possible
                 && (*it)->load < 1000 && can_install( *it, job ).size() )
	       return *it;
	  }
    }

  /* If we have no statistics simply use the first server which is usable.  */
  if (!all_job_stats.size ())
    {
      for (it = css.begin(); it != css.end(); ++it)
        {
          trace() << "no job stats - looking at " << ( *it )->nodename << " load: " << (*it )->load << " can install: " << can_install( *it, job ) << endl;
          if (int( (*it)->joblist.size() ) < (*it)->max_jobs
	      && (*it)->chroot_possible
              && (*it)->load < 1000 && can_install( *it, job ).size() )
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

  uint matches = 0;

  for (it = css.begin(); it != css.end(); ++it)
    {
      CS *cs = *it;
      /* For now ignore overloaded servers.  */
      if (int( cs->joblist.size() ) >= cs->max_jobs || cs->load >= 1000)
        {
#if DEBUG_SCHEDULER > 1
          trace() << "overloaded " << cs->nodename << " " << cs->joblist.size() << "/" <<  cs->max_jobs << " jobs, load:" << cs->load << endl;
#endif
          continue;
      }

      /* Servers that are already compiling jobs but got no environments
         are currently installing new environments - ignore so far */
      if ( cs->joblist.size() != 0 && cs->compiler_versions.size() == 0 )
        {
#if DEBUG_SCHEDULER > 0
          trace() << cs->nodename << " is currently installing\n";
#endif
          continue;
        }

      // incompatible architecture
      if ( !can_install( cs, job ).size() )
        {
#if DEBUG_SCHEDULER > 2
          trace() << cs->nodename << " can't install " << job->id << endl;
#endif
          continue;
        }

      /* Don't use non-chroot-able daemons for remote jobs.  XXX */
      if (!allow_run_as_user && !cs->chroot_possible)
        {
	  trace() << cs->nodename << " can't use chroot\n";
	  continue;
	}


#if DEBUG_SCHEDULER > 1
      trace() << cs->nodename << " compiled " << cs->last_compiled_jobs.size() << " got now: " <<
        cs->joblist.size() << " speed: " << server_speed (cs) << " compile time " <<
        cs->cum_compiled.compile_time_user << " produced code " << cs->cum_compiled.osize << endl;
#endif

      if ( cs->last_compiled_jobs.size() == 0 && cs->joblist.size() == 0)
	{
	  /* Make all servers compile a job at least once, so we'll get an
	     idea about their speed.  */
	  if (envs_match (cs, job))
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

      if ( envs_match( cs, job ) )
        {
          if ( !best )
            best = cs;
          /* Search the server with the earliest projected time to compile
             the job.  (XXX currently this is equivalent to the fastest one)  */
          else
            if (best->last_compiled_jobs.size() != 0
                && server_speed (best, job) < server_speed (cs, job))
              best = cs;
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
              bestui = cs;
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

#if DEBUG_SCHEDULER > 1
  if ( bestui )
    trace() << "taking best uninstalled " << bestui->nodename << " " <<  server_speed (bestui) << endl;
#endif
  return bestui;
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

  for (it = css.begin(); it != css.end(); )
    {
      if ( now - ( *it )->last_talk >= MAX_SCHEDULER_PING )
        {
          if ( ( *it )->max_jobs >= 0 )
            {
              trace() << "send ping " << ( *it )->nodename << endl;
              ( *it )->send_msg( PingMsg() );
              ( *it )->max_jobs *= -1; // better not give it away
              // give it a few seconds to answer a ping
              ( *it )->last_talk = time( 0 ) - MAX_SCHEDULER_PING + MIN_SCHEDULER_PING;
            }
          else
            { // R.I.P.
              trace() << "removing " << ( *it )->nodename << endl;
              CS *old = *it;
              ++it;
              handle_end (old, 0);
              continue;
            }
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


  /**
   * check the jobs that were not cared about even though they are done
   * (one in a million ;( */
  for (map<unsigned int, Job*>::const_iterator it = done_jobs.begin();
       it != done_jobs.end(); ++it)
    {
      Job *j = it->second;
      if (now - j->done_time > 30 )
        {
          trace() << "undone " << dump_job( j ) << endl;
          trace() << "FORCED removing " << j->server->nodename << endl;
          handle_end( j->server, 0 );
          /* the above will kill all jobs associated with this server, so
             we better get out of this, as done_jobs is changed too and
             we'll come back (</schwarzeneggeraccent>)
          */
          break;
        }
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

  if (css.empty())
    {
      /* XXX Can't happen anymore, right?  We have a request, hence one
	 daemon must be connected to us (the submitter), so css can't
	 be empty.  */
      log_error() << "no servers to handle\n";
      abort ();
      remove_job_request ();
      jobs.erase( job->id );
      notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id,  255 )));
      // Don't delete channel here.  We expect the client on the other side
      // to exit, and that will remove the channel in handle_end
      delete job;
      return false;
    }

  Job *first_job = job;
  CS *cs = 0;

  while ( true )
    {
      cs = pick_server (job);

      if (cs)
        break;

#if DEBUG_SCHEDULER > 0
      trace() << "tried to pick a server for " << job->id;
#endif

      /* Ignore the load on the submitter itself if no other host could
         be found.  We only obey to its max job number.  */
      cs = job->submitter;
      if (! (int( cs->joblist.size() ) < cs->max_jobs
             /* This should be trivially true.  */
             && can_install (cs, job).size()))
        {
#if DEBUG_SCHEDULER > 0
          trace() << " and failed ";
#endif

#if DEBUG_SCHEDULER > 1
          list<UnansweredList*>::iterator it;
          for (it = toanswer.begin(); it != toanswer.end(); ++it)
            trace() << (*it)->server->nodename << " ";
#endif

#if DEBUG_SCHEDULER > 0
          trace() << endl;
#endif

          job = delay_current_job();
          if ( job == first_job || !job ) // no job found in the whole toanswer list
            return false;
        }
      else
        {
#if DEBUG_SCHEDULER > 0
          trace () << " and had to use submitter\n";
#endif
          break;
        }
    }

  remove_job_request ();

  job->state = Job::WAITINGFORCS;
  job->server = cs;

  bool gotit = envs_match( cs, job );
  UseCSMsg m2(can_install( cs, job ), cs->name, cs->remote_port, job->id, gotit, job->local_client_id );

  if (!job->submitter->send_msg (m2))
    {
      trace() << "failed to deliver job " << job->id << endl;
      handle_end( job->submitter, 0 ); // will care for the rest
      return true;
    }
  else
    {
#if DEBUG_SCHEDULER >= 0
      trace() << "put " << job->id << " in joblist of " << cs->nodename;
      if (!gotit)
	trace() << " (will install now)";
      trace() << endl;
#endif
      cs->joblist.push_back( job );
      if ( !gotit ) // if we made the environment transfer, don't rely on the list
        {
          cs->compiler_versions.clear();
          cs->busy_installing = time(0);
        }
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
handle_login (MsgChannel *c, Msg *_m)
{
  LoginMsg *m = dynamic_cast<LoginMsg *>(_m);
  if (!m)
    return false;

  /* If we don't allow non-chroot-able daemons in the farm,
     discard them here.  */
  if (!allow_run_as_user && !m->chroot_possible)
    return false;

  trace() << "login " << m->nodename << " protocol version: " << c->protocol;

  CS *cs = static_cast<CS *>(c);
  cs->remote_port = m->port;
  cs->compiler_versions = m->envs;
  cs->max_jobs = m->max_kids;
  if ( m->nodename.length() )
    cs->nodename = m->nodename;
  else
    cs->nodename = cs->name;
  cs->host_platform = m->host_platform;
  cs->chroot_possible = m->chroot_possible;
  cs->pick_new_id();
  handle_monitor_stats( cs );
  css.push_back (cs);

#if 1
  trace() << " [";
  for (Environments::const_iterator it = m->envs.begin();
       it != m->envs.end(); ++it)
    trace() << it->second << "(" << it->first << "), ";
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

  CS *cs = static_cast<CS *>(c);
  cs->compiler_versions = m->envs;
  cs->busy_installing = 0;

  trace() << "RELOGIN " << cs->nodename << "(" << cs->host_platform << "): [";
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
  monitors.push_back (c);
  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
    handle_monitor_stats( *it );

  fd2chan.erase( c->fd ); // no expected data from them
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
  Job *job = jobs[m->job_id];
  if (job->server != c)
    {
      trace() << "that job isn't handled by " << c->name << endl;
      return false;
    }
  job->state = Job::COMPILING;
  job->starttime = m->stime;
  job->start_on_scheduler = time(0);
  CS *cs = dynamic_cast<CS*>( c );
  notify_monitors (MonJobBeginMsg (m->job_id, m->stime, cs->hostid));
#if DEBUG_SCHEDULER >= 0
  trace() << "BEGIN: " << m->job_id << " client=" << job->submitter->nodename
          << "(" << job->target_platform << ")" << " server="
          << job->server->nodename << "(" << job->server->host_platform
          << ")" << endl;
#endif

  return true;
}


static bool
handle_job_done (MsgChannel *c, Msg *_m)
{
  JobDoneMsg *m = dynamic_cast<JobDoneMsg *>(_m);
  if ( !m )
    return false;

  if (jobs.find(m->job_id) == jobs.end())
    {
      trace() << "job ID not present " << m->job_id << endl;
      return false;
    }

  Job *j = jobs[m->job_id];

  if ( m->exitcode == 0 )
    {
      trace() << "END " << m->job_id
              << " status=" << m->exitcode;

      if ( m->in_uncompressed )
        trace() << " in=" << m->in_uncompressed
                << "(" << int( m->in_compressed * 100 / m->in_uncompressed ) << "%)";
      else
        trace() << " in=0(0%)";

      if ( m->out_uncompressed )
        trace() << " out=" << m->out_uncompressed
                << "(" << int( m->out_compressed * 100 / m->out_uncompressed ) << "%)";
      else
        trace() << " out=0(0%)";

      trace() << " real=" << m->real_msec
              << " user=" << m->user_msec
              << " sys=" << m->sys_msec
              << " pfaults=" << m->pfaults
              << " server=" << j->server->nodename
              << endl;
    }
  else
    trace() << "END " << m->job_id
            << " status=" << m->exitcode << endl;

  if (m->is_from_server() && jobs[m->job_id]->server != c)
    {
      log_info() << "the server isn't the same for job " << m->job_id << endl;
      log_info() << "server: " << jobs[m->job_id]->server->nodename << endl;
      log_info() << "msg came from: " << ((CS*)c)->nodename << endl;
      // the daemon is not following matz's rules: kick him
      handle_end(c, 0);
      return false;
    }
  if (!m->is_from_server() && jobs[m->job_id]->submitter != c)
    {
      log_info() << "the submitter isn't the same for job " << m->job_id << endl;
      log_info() << "submitter: " << jobs[m->job_id]->submitter->nodename << endl;
      log_info() << "msg came from: " << ((CS*)c)->nodename << endl;
      // the daemon is not following matz's rules: kick him
      handle_end(c, 0);
      return false;
    }
  c->last_talk = time( 0 );

  j->server->joblist.remove (j);
  add_job_stats (j, m);
  notify_monitors (MonJobDoneMsg (*m));
  j->server->busy_installing = 0;
  jobs.erase (m->job_id);
  done_jobs.erase (m->job_id);
  delete j;

  return true;
}

static bool
handle_ping (MsgChannel * c, Msg * /*_m*/)
{
  c->last_talk = time( 0 );
  CS *cs = dynamic_cast<CS*>( c );
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

  c->last_talk = time( 0 );
  CS *cs = dynamic_cast<CS*>( c );
  if ( cs && cs->max_jobs < 0 )
    cs->max_jobs *= -1;

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
handle_timeout (MsgChannel * c, Msg * /*_m*/)
{
  c->last_talk = time( 0 );
  return false;
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
             : job->state == Job::WAITINGFORDONE ? "DONE"
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
handle_line (MsgChannel *c, Msg *_m)
{
  TextMsg *m = dynamic_cast<TextMsg *>(_m);
  if (!m)
    return false;
  char buffer[1000];
  string line;
  list<string> l;
  split_string (m->text, " \t\n", l);
  string cmd;
  if (l.empty())
    cmd = "";
  else
    {
      cmd = l.front();
      l.pop_front();
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
	  c->send_msg (TextMsg (line));
          for ( list<Job*>::const_iterator it2 = cs->joblist.begin(); it2 != cs->joblist.end(); ++it2 )
            c->send_msg (TextMsg ("   " + dump_job (*it2) ) );
	}
    }
  else if (cmd == "listjobs")
    {
      for (map<unsigned int, Job*>::const_iterator it = jobs.begin();
	   it != jobs.end(); ++it)
	c->send_msg( TextMsg( " " + dump_job (it->second) ) );
    }
  else if (cmd == "quit")
    {
      handle_end (c, m);
      return false;
    }
  else if (cmd == "removecs")
    {
      if (l.empty())
        c->send_msg (TextMsg (string ("Sure.  But which hosts?")));
      else
        for (list<string>::const_iterator si = l.begin(); si != l.end(); ++si)
	  for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
	    if ((*it)->nodename == *si || (*it)->name == *si)
	      {
                c->send_msg (TextMsg (string ("removing host ") + *si));
		handle_end ( *it, 0);
		break;
	      }
    }
  else if (cmd == "crashme")
    {
      *(int *)0 = 42;  // ;-)
    }
  else if (cmd == "internals" )
    {
      for (list<CS*>::iterator it = css.begin(); it != css.end(); ++it)
        {
          ( *it )->send_msg( GetInternalStatus() );
          Msg *msg = ( *it )->get_msg();
          if ( msg && msg->type == M_STATUS_TEXT )
            c->send_msg( TextMsg( dynamic_cast<StatusTextMsg*>( msg )->text ) );
          else
            c->send_msg( TextMsg( ( *it )->nodename + " not reporting\n" ) );
	  delete msg;
        }
    }
  else if (cmd == "help")
    {
      c->send_msg (TextMsg (
        "listcs\nlistjobs\nremovecs\ninternals\nhelp\nquit"));
    }
  else
    {
      string txt = "Invalid command '";
      txt += m->text;
      txt += "'";
      c->send_msg (TextMsg (txt));
    }
  c->send_msg (TextMsg (string ("done")));
  return true;
}

// return false if some error occured, leaves C open.  */
static bool
try_login (MsgChannel *c, Msg *m)
{
  bool ret = true;
  CS *cs = static_cast<CS *>(c);
  switch (m->type)
    {
    case M_LOGIN:
      cs->type = CS::DAEMON;
      ret = handle_login (c, m);
      break;
    case M_MON_LOGIN:
      cs->type = CS::MONITOR;
      ret = handle_mon_login (c, m);
      break;
    case M_TEXT:
      cs->type = CS::LINE;
      ret = handle_line (c, m);
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
#if DEBUG_SCHEDULER > 1
  trace() << "Handle_end " << c << " " << m << endl;
#endif

  CS *toremove = static_cast<CS *>(c);
  if (toremove->type == CS::MONITOR)
    {
      assert (find (monitors.begin(), monitors.end(), c) != monitors.end());
      monitors.remove (c);
#if DEBUG_SCHEDULER > 1
      trace() << "handle_end(moni) " << monitors.size() << endl;
#endif
    }
  else if (toremove->type == CS::DAEMON)
    {
#if DEBUG_SCHEDULER > 0
      trace() << "remove daemon\n";
#endif

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
		trace() << "STOP (DAEMON) FOR " << (*jit)->id << endl;
                notify_monitors (MonJobDoneMsg (JobDoneMsg( ( *jit )->id,  255 )));
		if ((*jit)->server)
		  (*jit)->server->busy_installing = 0;
		jobs.erase( (*jit)->id );
                done_jobs.erase( (*jit)->id );
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
              notify_monitors (MonJobDoneMsg (JobDoneMsg( job->id,  255 )));
	      /* If this job is removed because the submitter is removed
		 also remove the job from the servers joblist.  */
	      if (job->server && job->server != toremove)
		job->server->joblist.remove (job);
	      if (job->server)
	        job->server->busy_installing = 0;
              done_jobs.erase( job->id );
              jobs.erase( mit++ );
              delete job;
            }
          else
            {
              ++mit;
            }
        }
    }
  else if (toremove->type == CS::LINE)
    {
      c->send_msg (TextMsg ("Good Bye!"));
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
  m = c->get_msg (0);
  if (!m)
    {
      handle_end (c, m);
      return false;
    }
  /* First we need to login.  */
  if (static_cast<CS *>(c)->state == CS::CONNECTED)
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
    case M_JOB_LOCAL_DONE: ret = handle_local_job_done( c, m ); break;
    case M_LOGIN: ret = handle_relogin (c, m); break;
    case M_TEXT: ret = handle_line (c, m); break;
    case M_GET_CS: ret = handle_cs_request (c, m); break;
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
  if (listen (fd, 20) < 0)
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
  cerr << "usage: scheduler [options] \n"
       << "Options:\n"
       << "  -n, --netname <name>\n"
       << "  -p, --port <port>\n"
       << "  -h, --help\n"
       << "  -l, --log-file <file>\n"
       << "  -d, --daemonize\n"
       << "  -r, --allow-run-as-user\n"
       << "  -v[v[v]]]\n"
       << endl;

  exit(1);
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

  while ( true ) {
    int option_index = 0;
    static const struct option long_options[] = {
      { "netname", 1, NULL, 'n' },
      { "help", 0, NULL, 'h' },
      { "port", 0, NULL, 'p' },
      { "daemonize", 0, NULL, 'd'},
      { "log-file", 1, NULL, 'l'},
      { "allow-run-as-user", 1, NULL, 'u' },
      { 0, 0, 0, 0 }
    };

    const int c = getopt_long( argc, argv, "n:p:hl:vdr", long_options, &option_index );
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

    case 'r':
      allow_run_as_user = true;
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

  while (1)
    {
      struct timeval tv;
      tv.tv_usec = 0;
      tv.tv_sec = prune_servers ();

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
      if (text_fd > max_fd)
	max_fd = text_fd;
      FD_SET (text_fd, &read_set);
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
	  remote_len = sizeof (remote_addr);
          remote_fd = accept (listen_fd,
                              (struct sockaddr *) &remote_addr,
                              &remote_len );
	  if (remote_fd < 0 && errno != EAGAIN && errno != EINTR)
	    {
	      log_perror ("accept()");
	      return 1;
	    }
	  if (remote_fd >= 0)
	    {
	      CS *cs = new CS (remote_fd, (struct sockaddr*) &remote_addr, remote_len, false);
              trace() << "accepted " << cs->name << " " << cs->port << endl;
              cs->last_talk = time( 0 );

              if ( !cs->protocol ) // protocol mismatch
                {
                  delete cs;
                  continue;
                }
	      fd2chan[cs->fd] = cs;
	      if (!cs->read_a_bit () || cs->has_msg ())
	        handle_activity (cs);
	    }
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
              cs->last_talk = time (0);
              if (!cs->protocol) // protocol mismatch
                {
                  delete cs;
                  continue;
                }
	      fd2chan[cs->fd] = cs;
	      if (!cs->read_a_bit () || cs->has_msg ())
	        handle_activity (cs);
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
		  log_perror ("sendto()");
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
/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
