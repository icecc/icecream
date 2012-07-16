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


#ifndef _COMM_H
#define _COMM_H

#ifdef __linux__
#  include <stdint.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "job.h"

// if you increase the PROTOCOL_VERSION, add a macro below and use that
#define PROTOCOL_VERSION 31
// if you increase the MIN_PROTOCOL_VERSION, comment out macros below and clean up the code
#define MIN_PROTOCOL_VERSION 21

#define MAX_SCHEDULER_PONG 3
// MAX_SCHEDULER_PING must be multiple of MAX_SCHEDULER_PONG
#define MAX_SCHEDULER_PING 12 * MAX_SCHEDULER_PONG
// maximum amount of time in seconds a daemon can be busy installing
#define MAX_BUSY_INSTALLING 120

#define IS_PROTOCOL_22( c ) ( (c)->protocol >= 22 )
#define IS_PROTOCOL_23( c ) ( (c)->protocol >= 23 )
#define IS_PROTOCOL_24( c ) ( (c)->protocol >= 24 )
#define IS_PROTOCOL_25( c ) ( (c)->protocol >= 25 )
#define IS_PROTOCOL_26( c ) ( (c)->protocol >= 26 )
#define IS_PROTOCOL_27( c ) ( (c)->protocol >= 27 )
#define IS_PROTOCOL_28( c ) ( (c)->protocol >= 28 )
#define IS_PROTOCOL_29( c ) ( (c)->protocol >= 29 )
#define IS_PROTOCOL_30( c ) ( (c)->protocol >= 30 )
#define IS_PROTOCOL_31( c ) ( (c)->protocol >= 31 )

enum MsgType {
  // so far unknown
  M_UNKNOWN = 'A',

  /* When the scheduler didn't get M_STATS from a CS
     for a specified time (e.g. 10m), then he sends a
     ping */
  M_PING,

  /* Either the end of file chunks or connection (A<->A) */
  M_END,

  M_TIMEOUT, // unused

  // C --> CS
  M_GET_NATIVE_ENV,
  // CS -> C
  M_NATIVE_ENV,

  // C --> S
  M_GET_CS,
  // S --> C
  M_USE_CS,  // = 'G'

  // C --> CS
  M_COMPILE_FILE, // = 'I'
  // generic file transfer
  M_FILE_CHUNK,
  // CS --> C
  M_COMPILE_RESULT,

  // CS --> S (after the C got the CS from the S, the CS tells the S when the C asks him)
  M_JOB_BEGIN,
  M_JOB_DONE,     // = 'M'

  // C --> CS, CS --> S (forwarded from C), _and_ CS -> C as start ping
  M_JOB_LOCAL_BEGIN, // = 'N'
  M_JOB_LOCAL_DONE,

  // CS --> S, first message sent
  M_LOGIN,
  // CS --> S (periodic)
  M_STATS,

  // messages between monitor and scheduler
  M_MON_LOGIN,
  M_MON_GET_CS,
  M_MON_JOB_BEGIN, // = 'T'
  M_MON_JOB_DONE,
  M_MON_LOCAL_JOB_BEGIN,
  M_MON_STATS,

  M_TRANFER_ENV, // = 'X'

  M_TEXT,
  M_STATUS_TEXT,
  M_GET_INTERNALS,

  // S --> CS, answered by M_LOGIN
  M_CS_CONF,

  // C --> CS, after installing an environment
  M_VERIFY_ENV,
  M_VERIFY_ENV_RESULT,
  // C --> CS, CS --> S (forwarded from C), to not use given host for given environment
  M_BLACKLIST_HOST_ENV
};

class MsgChannel;

// a list of pairs of host platform, filename
typedef std::list<std::pair<std::string, std::string> > Environments;

class Msg {
public:
  enum MsgType type;
  Msg (enum MsgType t) : type(t) {}
  virtual ~Msg () {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MsgChannel {
  friend class Service;
  // deep copied
  struct sockaddr *addr;
  socklen_t len;
public:
  // our filedesc
  int fd;

  enum SendFlags {
      SendBlocking    = 1<<0,
      SendNonBlocking = 1<<1,
      SendBulkOnly    = 1<<2
  };

  // the minimum protocol version between me and him
  int protocol;

  std::string name;
  time_t last_talk;

  void setBulkTransfer();

  std::string dump() const;
  // NULL  <--> channel closed or timeout
  Msg *get_msg(int timeout = 10);
  // false <--> error (msg not send)
  bool send_msg (const Msg &, int SendFlags = SendBlocking);
  bool has_msg (void) const { return eof || instate == HAS_MSG; }
  bool read_a_bit (void);
  bool at_eof (void) const { return instate != HAS_MSG && eof; }
  bool is_text_based(void) const { return text_based; }

  MsgChannel &operator>>(uint32_t&);
  MsgChannel &operator>>(std::string&);
  MsgChannel &operator>>(std::list<std::string>&);

  MsgChannel &operator<<(uint32_t);
  MsgChannel &operator<<(const std::string&);
  MsgChannel &operator<<(const std::list<std::string>&);

  void readcompressed (unsigned char **buf, size_t &_uclen, size_t &_clen);
  void writecompressed (const unsigned char *in_buf,
			size_t _in_len, size_t &_out_len);
  void write_environments( const Environments &envs );
  void read_environments( Environments &envs );
  void read_line (std::string &line);
  void write_line (const std::string &line);

  bool eq_ip (const MsgChannel &s) const;

  virtual ~MsgChannel ();

protected:
  MsgChannel (int _fd, struct sockaddr *, socklen_t, bool text = false);
  bool wait_for_protocol ();
  // returns false if there was an error sending something
  bool flush_writebuf (bool blocking);
  void writefull (const void *_buf, size_t count);
  // returns false if there was an error in the protocol setup
  bool update_state (void);
  void chop_input (void);
  void chop_output (void);
  bool wait_for_msg (int timeout);
  char *msgbuf;
  size_t msgbuflen;
  size_t msgofs;
  size_t msgtogo;
  char *inbuf;
  size_t inbuflen;
  size_t inofs;
  size_t intogo;
  enum {NEED_PROTO, NEED_LEN, FILL_BUF, HAS_MSG} instate;
  uint32_t inmsglen;
  bool eof;
  bool text_based;
};

// just convenient functions to create MsgChannels
class Service {
public:
  static MsgChannel *createChannel( const std::string &host, unsigned short p, int timeout);
  static MsgChannel *createChannel( const std::string &domain_socket );
  static MsgChannel *createChannel( int remote_fd, struct sockaddr *, socklen_t );
};

// --------------------------------------------------------------------------
// this class is also used by icecream-monitor
class DiscoverSched 
{
  struct sockaddr_in remote_addr;
  std::string netname, schedname;
  int timeout;
  int ask_fd;
  time_t time0;
  unsigned int sport;
  void attempt_scheduler_connect();
public:
  /* Connect to a scheduler waiting max. TIMEOUT milliseconds.
     schedname can be the hostname of a box running a scheduler, to avoid
     broadcasting. */
  DiscoverSched (const std::string &_netname = std::string(),
		 int _timeout = 2000,
		 const std::string &_schedname = std::string());
  ~DiscoverSched();
  bool timed_out();
  int listen_fd() const { return schedname.empty() ? ask_fd : -1; }
  int connect_fd() const { return schedname.empty() ? -1 : ask_fd; }

  // compat for icecream monitor
  int get_fd() const { return listen_fd(); }

  MsgChannel *try_get_scheduler();
  // Returns the hostname of the scheduler - set by constructor or by try_get_scheduler
  std::string schedulerName() const { return schedname; }
  // Returns the network name of the scheduler - set by constructor or by try_get_scheduler
  std::string networkName() const { return netname; }
};
// --------------------------------------------------------------------------

/* Return a list of all reachable netnames.  We wait max. WAITTIME
   milliseconds for answers.  */
std::list<std::string> get_netnames (int waittime = 2000);

class PingMsg : public Msg {
public:
  PingMsg () : Msg(M_PING) {}
};

class EndMsg : public Msg {
public:
  EndMsg () : Msg(M_END) {}
};

class GetCSMsg : public Msg {
public:
  Environments versions;
  std::string filename;
  CompileJob::Language lang;
  uint32_t count; // the number of UseCS messages to answer with - usually 1
  std::string target;
  uint32_t arg_flags;
  uint32_t client_id;
  std::string preferred_host;
  bool ignore_unverified;
  GetCSMsg () : Msg(M_GET_CS), count( 1 ),arg_flags( 0 ), client_id( 0 ) {}
  GetCSMsg (const Environments &envs, const std::string &f, 
            CompileJob::Language _lang, unsigned int _count, 
	    std::string _target, unsigned int _arg_flags, 
            const std::string &host, bool _ignore_unverified)
    : Msg(M_GET_CS), versions( envs ), filename(f), lang(_lang), 
            count( _count ), target( _target ), arg_flags( _arg_flags ), 
            client_id( 0 ), preferred_host(host), ignore_unverified( _ignore_unverified )
  {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class UseCSMsg : public Msg {
public:
  uint32_t job_id;
  std::string hostname;
  uint32_t port;
  std::string host_platform;
  uint32_t got_env;
  uint32_t client_id;
  uint32_t matched_job_id;
  UseCSMsg () : Msg(M_USE_CS) {}
  UseCSMsg (std::string platform, std::string host, unsigned int p, unsigned int id, bool gotit,
          unsigned int _client_id, unsigned int matched_host_jobs)
    : Msg(M_USE_CS),
    job_id(id),
    hostname (host),
    port (p),
    host_platform( platform ),
    got_env( gotit ),
    client_id( _client_id ),
    matched_job_id (matched_host_jobs)
    { }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class GetNativeEnvMsg : public Msg {
public:
  GetNativeEnvMsg () : Msg(M_GET_NATIVE_ENV) {}
};

class UseNativeEnvMsg : public Msg {
public:
  std::string nativeVersion;
  UseNativeEnvMsg () : Msg(M_NATIVE_ENV) {}
  UseNativeEnvMsg (std::string _native)
      : Msg(M_NATIVE_ENV), nativeVersion( _native ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class CompileFileMsg : public Msg {
public:
  CompileFileMsg (CompileJob *j, bool delete_job = false)
      : Msg(M_COMPILE_FILE), deleteit( delete_job ), job( j ) {}
  ~CompileFileMsg() { if ( deleteit ) delete job; }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
  CompileJob *takeJob();

private:
  std::string remote_compiler_name() const;
  bool deleteit;
  CompileJob *job;
};

class FileChunkMsg : public Msg {
public:
  unsigned char* buffer;
  size_t len;
  mutable size_t compressed;
  bool del_buf;

  FileChunkMsg (unsigned char *_buffer, size_t _len)
      : Msg(M_FILE_CHUNK), buffer( _buffer ), len( _len ), del_buf(false) {}
  FileChunkMsg() : Msg( M_FILE_CHUNK ), buffer( 0 ), len( 0 ), del_buf(true) {}
  ~FileChunkMsg();
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
private:
  FileChunkMsg(const FileChunkMsg&);
  FileChunkMsg& operator=(const FileChunkMsg&);
};

class CompileResultMsg : public Msg {
public:
  int status;
  std::string out;
  std::string err;
  bool was_out_of_memory;

  CompileResultMsg () : Msg(M_COMPILE_RESULT), status( 0 ), was_out_of_memory( false ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobBeginMsg : public Msg {
public:
  uint32_t job_id;
  uint32_t stime;
  JobBeginMsg () : Msg(M_JOB_BEGIN) {}
  JobBeginMsg (unsigned int j) : Msg(M_JOB_BEGIN), job_id(j), stime(time(0)) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

enum SpecialExits {
  CLIENT_WAS_WAITING_FOR_CS = 200
};

class JobDoneMsg : public Msg {
public:
  uint32_t real_msec;  /* real time it used */
  uint32_t user_msec;  /* user time used */
  uint32_t sys_msec;   /* system time used */
  uint32_t pfaults;     /* page faults */

  int exitcode;            /* exit code */
  /* FROM_SERVER: this message was generated by the daemon responsible
        for remotely compiling the job (i.e. job->server).
     FROM_SUBMITTER: this message was generated by the daemon connected
        to the submitting client.  */
  enum from_type {FROM_SERVER = 0, FROM_SUBMITTER = 1};
  uint32_t flags;

  uint32_t in_compressed;
  uint32_t in_uncompressed;
  uint32_t out_compressed;
  uint32_t out_uncompressed;

  uint32_t job_id;
  JobDoneMsg (int job_id = 0, int exitcode = -1, unsigned int flags = FROM_SERVER);
  void set_from (from_type from)
  {
    flags |= (uint32_t)from;
  }
  bool is_from_server () { return (flags & FROM_SUBMITTER) == 0; }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobLocalBeginMsg : public Msg {
public:
  std::string outfile;
  uint32_t stime;
  uint32_t id;
  JobLocalBeginMsg(int job_id = 0, const std::string &file = "") : Msg( M_JOB_LOCAL_BEGIN ),
                                                                   outfile( file ), stime(time(0)), id( job_id ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobLocalDoneMsg : public Msg {
public:
  uint32_t job_id;
  JobLocalDoneMsg(unsigned int id = 0) : Msg( M_JOB_LOCAL_DONE ), job_id( id ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class LoginMsg : public Msg {
public:
  uint32_t port;
  Environments envs;
  uint32_t max_kids;
  bool noremote;
  bool         chroot_possible;
  std::string nodename;
  std::string host_platform;
  LoginMsg (unsigned int myport, const std::string &_nodename, const std::string _host_platform);
  LoginMsg () : Msg(M_LOGIN), port( 0 ) {}

  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class ConfCSMsg : public Msg {
public:
  uint32_t max_scheduler_pong;
  uint32_t max_scheduler_ping;
  std::string bench_source;

  ConfCSMsg (const char* bench) 
    : Msg(M_CS_CONF), max_scheduler_pong(MAX_SCHEDULER_PONG), max_scheduler_ping(MAX_SCHEDULER_PING), bench_source(bench) {}
  ConfCSMsg ()
    : Msg(M_CS_CONF), max_scheduler_pong(MAX_SCHEDULER_PONG), max_scheduler_ping(MAX_SCHEDULER_PING) {}


  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class StatsMsg : public Msg {
public:
  /**
   * For now the only load measure we have is the
   * load from 0-1000.
   * This is defined to be a daemon defined value
   * on how busy the machine is. The higher the load
   * is, the slower a given job will compile (preferably
   * linear scale). Load of 1000 means to not schedule
   * another job under no circumstances.
   */
  uint32_t load;

  uint32_t loadAvg1;
  uint32_t loadAvg5;
  uint32_t loadAvg10;
  uint32_t freeMem;

  StatsMsg () : Msg(M_STATS) { load = 0; }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class EnvTransferMsg : public Msg {
public:
  std::string name;
  std::string target;
  EnvTransferMsg() : Msg( M_TRANFER_ENV ) {
  }
  EnvTransferMsg( const std::string &_target, const std::string &_name )
    : Msg( M_TRANFER_ENV ), name( _name ), target( _target ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class GetInternalStatus : public Msg {
public:
  GetInternalStatus() : Msg(M_GET_INTERNALS) {}
  GetInternalStatus(const GetInternalStatus&) : Msg(M_GET_INTERNALS) {}
};

class MonLoginMsg : public Msg {
public:
  MonLoginMsg() : Msg(M_MON_LOGIN) {}
};

class MonGetCSMsg : public GetCSMsg {
public:
  uint32_t job_id;
  uint32_t clientid;

  MonGetCSMsg() : GetCSMsg() { // overwrite
    type = M_MON_GET_CS;
    clientid = job_id = 0;
  }
  MonGetCSMsg( int jobid, int hostid, GetCSMsg *m )
    : GetCSMsg( Environments(), m->filename, m->lang, 1, m->target, 0, std::string(), false ), job_id( jobid ), clientid( hostid )
  {
    type = M_MON_GET_CS;
  }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MonJobBeginMsg : public Msg {
public:
  uint32_t job_id;
  uint32_t stime;
  uint32_t hostid;
  MonJobBeginMsg() : Msg(M_MON_JOB_BEGIN), job_id( 0 ), stime( 0 ), hostid( 0 ) {}
  MonJobBeginMsg( unsigned int id, unsigned int time, int _hostid)
    : Msg( M_MON_JOB_BEGIN ), job_id( id ), stime( time ), hostid( _hostid ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MonJobDoneMsg : public JobDoneMsg {
public:
  MonJobDoneMsg() : JobDoneMsg() {
    type = M_MON_JOB_DONE;
  }
  MonJobDoneMsg(const JobDoneMsg& o)
    : JobDoneMsg(o) { type = M_MON_JOB_DONE; }
};

class MonLocalJobBeginMsg : public Msg {
public:
  uint32_t job_id;
  uint32_t stime;
  uint32_t hostid;
  std::string file;
  MonLocalJobBeginMsg() : Msg(M_MON_LOCAL_JOB_BEGIN) {}
  MonLocalJobBeginMsg( unsigned int id, const std::string &_file, unsigned int time, int _hostid )
    : Msg( M_MON_LOCAL_JOB_BEGIN ), job_id( id ), stime( time ), hostid( _hostid ), file( _file ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MonStatsMsg : public Msg {
public:
  uint32_t hostid;
  std::string statmsg;
  MonStatsMsg() : Msg( M_MON_STATS ) {}
  MonStatsMsg( int id, const std::string &_statmsg )
    : Msg( M_MON_STATS ), hostid( id ), statmsg( _statmsg )
  { }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class TextMsg : public Msg {
public:
  std::string text;
  TextMsg() : Msg( M_TEXT ) {}
  TextMsg( const std::string &_text)
    : Msg ( M_TEXT ), text(_text) {}
  TextMsg (const TextMsg& m)
    : Msg ( M_TEXT ), text(m.text) {}
  virtual void fill_from_channel (MsgChannel *c);
  virtual void send_to_channel (MsgChannel *c) const;
};

class StatusTextMsg : public Msg {
public:
  std::string text;
  StatusTextMsg() : Msg( M_STATUS_TEXT ) {}
  StatusTextMsg( const std::string &_text)
    : Msg ( M_STATUS_TEXT ), text(_text) {}
  virtual void fill_from_channel (MsgChannel *c);
  virtual void send_to_channel (MsgChannel *c) const;
};

class VerifyEnvMsg : public Msg {
public:
  std::string environment;
  std::string target;
  VerifyEnvMsg() : Msg( M_VERIFY_ENV ) {}
  VerifyEnvMsg( const std::string &_target, const std::string &_environment )
    : Msg( M_VERIFY_ENV ), environment( _environment ), target( _target ) {}
  virtual void fill_from_channel (MsgChannel *c);
  virtual void send_to_channel (MsgChannel *c) const;
};

class VerifyEnvResultMsg : public Msg {
public:
  bool ok;
  VerifyEnvResultMsg() : Msg( M_VERIFY_ENV_RESULT ) {}
  VerifyEnvResultMsg( bool _ok )
    : Msg ( M_VERIFY_ENV_RESULT ), ok(_ok) {}
  virtual void fill_from_channel (MsgChannel *c);
  virtual void send_to_channel (MsgChannel *c) const;
};

class BlacklistHostEnvMsg : public Msg {
public:
  std::string environment;
  std::string target;
  std::string hostname;
  BlacklistHostEnvMsg() : Msg( M_BLACKLIST_HOST_ENV ) {}
  BlacklistHostEnvMsg( const std::string &_target, const std::string &_environment, const std::string &_hostname )
    : Msg( M_BLACKLIST_HOST_ENV ), environment( _environment ), target( _target ), hostname( _hostname ) {}
  virtual void fill_from_channel (MsgChannel *c);
  virtual void send_to_channel (MsgChannel *c) const;
};

#endif
