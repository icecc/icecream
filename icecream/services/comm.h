/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */

#ifndef _COMM_H
#define _COMM_H

#ifdef __linux__
#  include <stdint.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>

#include "job.h"

#define PROTOCOL_VERSION 1

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
  M_GET_SCHEDULER,
  // CS -> C
  M_USE_SCHEDULER,

  // C --> S
  M_GET_CS,
  // S --> C
  M_USE_CS,

  // C --> CS
  M_COMPILE_FILE,
  // generic file transfer
  M_FILE_CHUNK,
  // CS --> C
  M_COMPILE_RESULT,

  // CS --> S (after the C got the CS from the S, the CS tells the S when the C asks him)
  M_JOB_BEGIN,
  M_JOB_DONE,

  // C --> S
  M_JOB_LOCAL_BEGIN,
  M_JOB_LOCAL_ID,
  M_JOB_LOCAL_DONE,

  // CS --> S, first message sent
  M_LOGIN,

  // CS --> S (periodic)
  M_STATS,

  // messages between monitor and scheduler
  M_MON_LOGIN,
  M_MON_GET_CS,
  M_MON_JOB_BEGIN,
  M_MON_JOB_DONE,
  M_MON_LOCAL_JOB_BEGIN,
  M_MON_LOCAL_JOB_DONE,
  M_MON_STATS,

  M_TRANFER_ENV
};

class MsgChannel;

class Msg {
public:
  enum MsgType type;
  Msg (enum MsgType t) : type(t) {}
  virtual ~Msg () {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

// an endpoint of a MsgChannel, i.e. most often a host
class Service {
  friend class MsgChannel;
  // deep copied
  struct sockaddr *addr;
  socklen_t len;
  MsgChannel *c;
public:
  std::string name;
  unsigned short port;
  Service (struct sockaddr *, socklen_t);
  Service (const std::string &host, unsigned short p);
  MsgChannel *channel() const { return c; }
  MsgChannel *createChannel( int remote_fd );
  bool eq_ip (const Service &s);
  virtual ~Service ();
};

class MsgChannel {
  friend class Service;
public:
  Service *other_end;
  // our filedesc
  int fd;
  // NULL  <--> channel closed
  Msg *get_msg(bool blocking = true);
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

  void readuint32 (uint32_t &buf);
  void writeuint32 (uint32_t u);
  void read_string (std::string &s);
  void write_string (const std::string &s);
  void read_strlist (std::list<std::string> &l);
  void write_strlist (const std::list<std::string> &l);
  void readcompressed (unsigned char **buf, size_t &_uclen, size_t &_clen);
  void writecompressed (const unsigned char *in_buf,
			size_t _in_len, size_t &_out_len);

  // be careful: it also deletes the service it belongs to
  ~MsgChannel ();
private:
  MsgChannel (int _fd);
  MsgChannel (int _fd, Service *serv);
  // returns false if there was an error sending something
  bool flush_writebuf (bool blocking);
  void writefull (const void *_buf, size_t count);
  void update_state (void);
  void chop_input (void);
  void chop_output (void);
  bool wait_for_msg (void);
  char *msgbuf;
  size_t msgbuflen;
  size_t msgofs;
  size_t msgtogo;
  char *inbuf;
  size_t inbuflen;
  size_t inofs;
  size_t intogo;
  enum {NEED_LEN, FILL_BUF, HAS_MSG} instate;
  uint32_t inmsglen;
  bool eof;
};

/* Connect to a scheduler waiting max. TIMEOUT milliseconds.  */
MsgChannel *connect_scheduler (const std::string &netname = std::string(),
			       int timeout = 2000);

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
  std::string version;
  std::string filename;
  CompileJob::Language lang;
  GetCSMsg () : Msg(M_GET_CS) {}
  GetCSMsg (const std::string &v, const std::string &f, CompileJob::Language _lang)
    : Msg(M_GET_CS), version(v), filename(f), lang(_lang) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class UseCSMsg : public Msg {
public:
  unsigned int job_id;
  std::string hostname;
  unsigned int port;
  std::string environment;
  unsigned int got_env;
  UseCSMsg () : Msg(M_USE_CS) {}
  UseCSMsg (std::string env, std::string host, unsigned int p, unsigned int id, bool gotit)
    : Msg(M_USE_CS), job_id(id), hostname (host), port (p), environment( env ), got_env( gotit ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};


class GetSchedulerMsg : public Msg {
public:
  GetSchedulerMsg () : Msg(M_GET_SCHEDULER) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class UseSchedulerMsg : public Msg {
public:
  std::string hostname;
  unsigned int port;
  UseSchedulerMsg () : Msg(M_USE_SCHEDULER), port( 0 ) {}
  UseSchedulerMsg (std::string host, unsigned int p)
      : Msg(M_USE_SCHEDULER), hostname (host), port (p) {}
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

  CompileResultMsg () : Msg(M_COMPILE_RESULT) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobBeginMsg : public Msg {
public:
  unsigned int job_id;
  unsigned int stime;
  JobBeginMsg () : Msg(M_JOB_BEGIN) {}
  JobBeginMsg (unsigned int j) : Msg(M_JOB_BEGIN), job_id(j), stime(time(0)) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobDoneMsg : public Msg {
public:
  unsigned int real_msec;  /* real time it used */
  unsigned int user_msec;  /* user time used */
  unsigned int sys_msec;   /* system time used */
  unsigned int maxrss;     /* maximum resident set size (KB) */
  unsigned int idrss;      /* integral unshared data size (KB) */
  unsigned int majflt;     /* page faults */
  unsigned int nswap;      /* swaps */

  int exitcode;            /* exit code */

  unsigned int in_compressed;
  unsigned int in_uncompressed;
  unsigned int out_compressed;
  unsigned int out_uncompressed;

  unsigned int job_id;
  JobDoneMsg (int job_id = 0, int exitcode = -1);
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobLocalBeginMsg : public Msg {
public:
  unsigned int stime;
  JobLocalBeginMsg() : Msg( M_JOB_LOCAL_BEGIN ), stime(time(0)) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobLocalId : public Msg {
public:
  unsigned int job_id;
  JobLocalId() : Msg( M_JOB_LOCAL_ID ), job_id(0) {}
  JobLocalId( unsigned int j ) : Msg( M_JOB_LOCAL_ID ), job_id( j ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class JobLocalDoneMsg : public Msg {
public:
  int exitcode;            /* exit code */
  unsigned int job_id;
  JobLocalDoneMsg (int job_id = 0, int exitcode = -1);
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class LoginMsg : public Msg {
public:
  unsigned int port;
  std::list<std::string> envs;
  unsigned int max_kids;
  LoginMsg (unsigned int myport)
      : Msg(M_LOGIN), port( myport ) {}
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
  unsigned int load;

  unsigned int niceLoad;
  unsigned int sysLoad;
  unsigned int userLoad;
  unsigned int idleLoad;
  unsigned int loadAvg1;
  unsigned int loadAvg5;
  unsigned int loadAvg10;
  unsigned int freeMem;

  StatsMsg () : Msg(M_STATS) { load = 0; }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MonLoginMsg : public Msg {
public:
  MonLoginMsg() : Msg(M_MON_LOGIN) {}
};

class MonGetCSMsg : public GetCSMsg {
public:
  unsigned int job_id;
  std::string client;

  MonGetCSMsg() : GetCSMsg() { // overwrite
    type = M_MON_GET_CS;
    job_id = 0;
  }
  MonGetCSMsg( int id, std::string host, GetCSMsg *m )
    : GetCSMsg( m->version, m->filename, m->lang ), job_id( id ), client( host )
  {
    type = M_MON_GET_CS;
  }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MonJobBeginMsg : public Msg {
public:
  unsigned int job_id;
  unsigned int stime;
  std::string host;
  MonJobBeginMsg() : Msg(M_MON_JOB_BEGIN) {}
  MonJobBeginMsg( unsigned int id, unsigned int time, std::string name )
    : Msg( M_MON_JOB_BEGIN ), job_id( id ), stime( time ), host( name ) {}
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
  unsigned int job_id;
  unsigned int stime;
  std::string host;
  MonLocalJobBeginMsg() : Msg(M_MON_LOCAL_JOB_BEGIN) {}
  MonLocalJobBeginMsg( unsigned int id, unsigned int time, std::string name )
    : Msg( M_MON_LOCAL_JOB_BEGIN ), job_id( id ), stime( time ), host( name ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class MonLocalJobDoneMsg : public JobLocalDoneMsg {
public:
  MonLocalJobDoneMsg() : JobLocalDoneMsg() {
    type = M_MON_LOCAL_JOB_DONE;
  }
  MonLocalJobDoneMsg( const JobLocalDoneMsg &m )
    : JobLocalDoneMsg(m)
  {
    type = M_MON_LOCAL_JOB_DONE;
  }
};

class MonStatsMsg : public StatsMsg {
public:
  std::string host;
  unsigned int max_kids;
  MonStatsMsg() : StatsMsg() {
    type = M_MON_STATS;
  }
  MonStatsMsg( const std::string& name, unsigned int mk, const StatsMsg &m )
    : StatsMsg(m), host( name ), max_kids( mk )
  {
    type = M_MON_STATS;
  }
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

class EnvTransferMsg : public Msg {
public:
  std::string name;
  EnvTransferMsg() : Msg( M_TRANFER_ENV ) {
  }
  EnvTransferMsg( const std::string &_name )
    : Msg( M_TRANFER_ENV ), name( _name ) {}
  virtual void fill_from_channel (MsgChannel * c);
  virtual void send_to_channel (MsgChannel * c) const;
};

#endif
