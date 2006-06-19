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

#include "job.h"

// if you increase the PROTOCOL_VERSION, add a macro below and use that
#define PROTOCOL_VERSION 22
// if you increase the MIN_PROTOCOL_VERSION, comment out macros below and clean up the code
#define MIN_PROTOCOL_VERSION 21

#define MIN_SCHEDULER_PING 2
#define MAX_SCHEDULER_PING 30

#define IS_PROTOCOL_22( c ) ( c->protocol >= 22 )

enum MsgType {
  // so far unknown
  M_UNKNOWN = 'A',

  /* When the scheduler didn't get M_STATS from a CS
     for a specified time (e.g. 10m), then he sends a
     ping */
  M_PING,

  /* Either the end of file chunks or connection (A<->A) */
  M_END,

  // Fake message used in message reading loops (A<->A)
  M_TIMEOUT,

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
  M_GET_INTERNALS
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

  // the minimum protocol version between me and him
  int protocol;

  std::string name;
  uint32_t port;
  time_t last_talk;

  std::string dump() const;
  // NULL  <--> channel closed
  Msg *get_msg(int timeout = 10);
  // false <--> error (msg not send)
  bool send_msg (const Msg &, bool blocking = true);
  // return last error (0 == no error)
  int error(void) {return 0;}
  bool has_msg (void) const { return eof || instate == HAS_MSG; }
  bool need_write (void) const { return msgtogo != 0; }
  bool read_a_bit (void);
  bool write_a_bit (void) {
    return need_write () ? flush_writebuf (false) : true;
  }
  bool at_eof (void) const { return eof; }
  bool is_text_based(void) const { return text_based; }

  void readuint32 (uint32_t &buf);
  void writeuint32 (uint32_t u);
  void read_string (std::string &s);
  void write_string (const std::string &s);
  void read_strlist (std::list<std::string> &l);
  void write_strlist (const std::list<std::string> &l);
  void readcompressed (unsigned char **buf, size_t &_uclen, size_t &_clen);
  void writecompressed (const unsigned char *in_buf,
			size_t _in_len, size_t &_out_len);
  void write_environments( const Environments &envs );
  void read_environments( Environments &envs );
  void read_line (std::string &line);
  void write_line (const std::string &line);

  bool eq_ip (const MsgChannel &s);

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
  static MsgChannel *createChannel( int remote_fd, struct sockaddr *, socklen_t );
};

/* Connect to a scheduler waiting max. TIMEOUT milliseconds.
 * schedname can be the hostname of a box running a scheduler, to avoid
 * broadcasting. */
MsgChannel *connect_scheduler (const std::string &netname = std::string(),
			       int timeout = 2000,
			       const std::string &schedname = std::string());

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

class TimeoutMsg : public Msg {
public:
  TimeoutMsg () : Msg(M_TIMEOUT) {}
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
  GetCSMsg () : Msg(M_GET_CS), count( 1 ),arg_flags( 0 ), client_id( 0 ) {}
  GetCSMsg (const Environments &envs, const std::string &f, 
            CompileJob::Language _lang, unsigned int _count, 
	    std::string _target, unsigned int _arg_flags, 
            const std::string &host)
    : Msg(M_GET_CS), versions( envs ), filename(f), lang(_lang), 
            count( _count ), target( _target ), arg_flags( _arg_flags ), 
            client_id( 0 ), preferred_host(host)
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
  UseCSMsg () : Msg(M_USE_CS) {}
  UseCSMsg (std::string platform, std::string host, unsigned int p, unsigned int id, bool gotit, unsigned int _client_id)
    : Msg(M_USE_CS), job_id(id), hostname (host), port (p), host_platform( platform ), got_env( gotit ), client_id( _client_id ) {}
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
  bool         chroot_possible;
  std::string nodename;
  std::string host_platform;
  LoginMsg (unsigned int myport, const std::string &_nodename, const std::string _host_platform);
  LoginMsg () : Msg(M_LOGIN), port( 0 ) {}

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
    : GetCSMsg( Environments(), m->filename, m->lang, 1, m->target, 0, std::string() ), job_id( jobid ), clientid( hostid )
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
  MonJobDoneMsg( const JobDoneMsg &m )
    : JobDoneMsg(m)
  {
    type = M_MON_JOB_DONE;
  }
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
  {
  }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class TextMsg : public Msg {
public:
  std::string text;
  TextMsg() : Msg( M_TEXT ) {}
  TextMsg( const std::string &_text)
    : Msg ( M_TEXT ), text(_text) {}
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

#endif
