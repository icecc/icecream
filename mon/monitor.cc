/*
    This file is part of Icecream.

    Copyright (c) 2003 Frerich Raabe <raabe@kde.org>
    Copyright (c) 2003,2004 Stephan Kulow <coolo@kde.org>
    Copyright (c) 2003,2004 Cornelius Schumacher <schumacher@kde.org>

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

#include "monitor.h"

#include "hostinfo.h"
#include "statusview.h"

#include <services/logging.h>
#include <services/comm.h>

#include <klocale.h>
#include <kdebug.h>

#include <qsocketnotifier.h>
#include <qtimer.h>

#include <list>

using namespace std;

Monitor::Monitor( HostInfoManager *m, QObject *parent, const char *name )
  : QObject( parent, name ), m_hostInfoManager( m ), m_view( 0 ),
    m_scheduler( 0 ), m_scheduler_read( 0 ), mSchedulerOnline( false )
{
  checkScheduler();
}

Monitor::~Monitor()
{
}

void Monitor::checkScheduler(bool deleteit)
{
  if ( deleteit ) {
    m_rememberedJobs.clear();
    delete m_scheduler;
    m_scheduler = 0;
    delete m_scheduler_read;
    m_scheduler_read = 0;
  } else if ( m_scheduler )
    return;
  QTimer::singleShot( 1800, this, SLOT( slotCheckScheduler() ) );
}

void Monitor::slotCheckScheduler()
{
  list<string> names;

  if ( !m_current_netname.isEmpty() ) {
    names.push_front( m_current_netname.latin1() );
  } else {
    names = get_netnames( 60 );
  }

  if (getenv("USE_SCHEDULER"))
     names.push_front(""); // try $USE_SCHEDULER

  if ( names.empty() ) {
    checkScheduler( true );
    setSchedulerState( false );
    return;
  }

  for ( list<string>::const_iterator it = names.begin(); it != names.end();
        ++it ) {
    m_current_netname = it->c_str();
    m_scheduler = connect_scheduler ( m_current_netname.latin1() );
    if ( m_scheduler ) {
      if ( !m_scheduler->send_msg ( MonLoginMsg() ) ) {
        delete m_scheduler;
      } else {
        m_scheduler_read = new QSocketNotifier( m_scheduler->fd,
                                                QSocketNotifier::Read,
                                                this );
        QObject::connect( m_scheduler_read, SIGNAL( activated( int ) ),
                          SLOT( msgReceived() ) );
        setSchedulerState( true );
        return;
      }
    }
  }
  checkScheduler( true );
  setSchedulerState( false );
}

void Monitor::msgReceived()
{
  Msg *m = m_scheduler->get_msg ();
  if ( !m ) {
    kdDebug() << "lost connection to scheduler\n";
    checkScheduler( true );
    setSchedulerState( false );
    return;
  }

  switch ( m->type ) {
    case M_MON_GET_CS:
      handle_getcs( m );
      break;
    case M_MON_JOB_BEGIN:
      handle_job_begin( m );
      break;
    case M_MON_JOB_DONE:
      handle_job_done( m );
      break;
    case M_END:
      cout << "END" << endl;
      checkScheduler( true );
      break;
    case M_MON_STATS:
      handle_stats( m );
      break;
    case M_MON_LOCAL_JOB_BEGIN:
      handle_local_begin( m );
      break;
    case M_JOB_LOCAL_DONE:
      handle_local_done( m );
      break;
    default:
      cout << "UNKNOWN" << endl;
      break;
  }
  delete m;
}

void Monitor::handle_getcs( Msg *_m )
{
  MonGetCSMsg *m = dynamic_cast<MonGetCSMsg*>( _m );
  if ( !m ) return;
  m_rememberedJobs[m->job_id] = Job( m->job_id, m->clientid,
                                     m->filename.c_str(),
                                     m->lang == CompileJob::Lang_C ? "C" :
                                                                     "C++" );
  m_view->update( m_rememberedJobs[m->job_id] );
}

void Monitor::handle_local_begin( Msg *_m )
{
  MonLocalJobBeginMsg *m = dynamic_cast<MonLocalJobBeginMsg*>( _m );
  if ( !m ) return;

  m_rememberedJobs[m->job_id] = Job( m->job_id, m->hostid,
                                     m->file.c_str(),
                                     "C++" );
  m_rememberedJobs[m->job_id].setState( Job::LocalOnly );
  m_view->update( m_rememberedJobs[m->job_id] );
}

void Monitor::handle_local_done( Msg *_m )
{
  JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg*>( _m );
  if ( !m ) return;

  JobList::iterator it = m_rememberedJobs.find( m->job_id );
  if ( it == m_rememberedJobs.end() ) {
    // we started in between
    return;
  }

  ( *it ).setState( Job::Finished );
  m_view->update( *it );

  if ( m_rememberedJobs.size() > 3000 ) { // now remove 1000
      uint count = 1000;

      while ( --count )
          m_rememberedJobs.erase( m_rememberedJobs.begin() );
  }
}

void Monitor::handle_stats( Msg *_m )
{
  MonStatsMsg *m = dynamic_cast<MonStatsMsg*>( _m );
  if ( !m ) return;

  QStringList statmsg = QStringList::split( '\n', m->statmsg.c_str() );
  HostInfo::StatsMap stats;
  for ( QStringList::ConstIterator it = statmsg.begin(); it != statmsg.end();
        ++it ) {
      QString key = *it;
      key = key.left( key.find( ':' ) );
      QString value = *it;
      value = value.mid( value.find( ':' ) + 1 );
      stats[key] = value;
  }

  HostInfo *hostInfo = m_hostInfoManager->checkNode( m->hostid, stats );

  if ( hostInfo->isOffline() ) {
    m_view->removeNode( m->hostid );
  } else {
    m_view->checkNode( m->hostid );
  }
}

void Monitor::handle_job_begin( Msg *_m )
{
  MonJobBeginMsg *m = dynamic_cast<MonJobBeginMsg*>( _m );
  if ( !m ) return;

  JobList::iterator it = m_rememberedJobs.find( m->job_id );
  if ( it == m_rememberedJobs.end() ) {
    // we started in between
    return;
  }

  ( *it ).setServer( m->hostid );
  ( *it ).setStartTime( m->stime );
  ( *it ).setState( Job::Compiling );

#if 0
  kdDebug() << "BEGIN: " << (*it).fileName() << " (" << (*it).jobId()
            << ")" << endl;
#endif

  m_view->update( *it );
}

void Monitor::handle_job_done( Msg *_m )
{
  MonJobDoneMsg *m = dynamic_cast<MonJobDoneMsg*>( _m );
  if ( !m ) return;

  JobList::iterator it = m_rememberedJobs.find( m->job_id );
  if ( it == m_rememberedJobs.end() ) {
    // we started in between
    return;
  }

  ( *it ).exitcode = m->exitcode;
  if ( m->exitcode ) {
    ( *it ).setState( Job::Failed );
  } else {
    ( *it ).setState( Job::Finished );
    ( *it ).real_msec = m->real_msec;
    ( *it ).user_msec = m->user_msec;
    ( *it ).sys_msec = m->sys_msec;   /* system time used */
    ( *it ).pfaults = m->pfaults;     /* page faults */

    ( *it ).in_compressed = m->in_compressed;
    ( *it ).in_uncompressed = m->in_uncompressed;
    ( *it ).out_compressed = m->out_compressed;
    ( *it ).out_uncompressed = m->out_uncompressed;
  }

#if 0
  kdDebug() << "DONE: " << (*it).fileName() << " (" << (*it).jobId()
            << ")" << endl;
#endif

  m_view->update( *it );
}

void Monitor::setCurrentView( StatusView *view, bool rememberJobs )
{
  m_view = view;

  m_view->updateSchedulerState( mSchedulerOnline );

  if ( rememberJobs ) {
    JobList::ConstIterator it = m_rememberedJobs.begin();
    for ( ; it != m_rememberedJobs.end(); ++it )
      m_view->update( *it );
  }
}

void Monitor::setCurrentNet( const QString &netName )
{
  m_current_netname = netName;
}

void Monitor::setSchedulerState( bool online )
{
  mSchedulerOnline = online;
  m_view->updateSchedulerState( online );
}

#include "monitor.moc"
