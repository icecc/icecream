/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */

#ifndef _COMM_H
#define _COMM_H

#include "job.h"

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

  // CS --> S, first message sent
  M_LOGIN,

  // CS --> S (periodic)
  M_STATS
};

class Msg {
public:
  enum MsgType type;
  Msg (enum MsgType t) : type(t) {}
  virtual ~Msg () {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class MsgChannel;

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
  Msg *get_msg(void);
  // false <--> error (msg not send)
  bool send_msg (const Msg &);
  // return last error (0 == no error)
  int error(void) {return 0;}
  // be careful: it also deletes the service it belongs to
  ~MsgChannel ();
private:
  MsgChannel (int _fd);
  MsgChannel (int _fd, Service *serv);
};

MsgChannel *connect_scheduler ();

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
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class UseCSMsg : public Msg {
public:
  unsigned int job_id;
  std::string hostname;
  unsigned int port;
  std::string environment;
  UseCSMsg () : Msg(M_USE_CS) {}
  UseCSMsg (std::string env, std::string host, unsigned int p, unsigned int id)
    : Msg(M_USE_CS), job_id(id), hostname (host), port (p), environment( env ) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};


class GetSchedulerMsg : public Msg {
public:
  GetSchedulerMsg () : Msg(M_GET_SCHEDULER) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class UseSchedulerMsg : public Msg {
public:
  std::string hostname;
  unsigned int port;
  UseSchedulerMsg () : Msg(M_USE_SCHEDULER), port( 0 ) {}
  UseSchedulerMsg (std::string host, unsigned int p)
      : Msg(M_USE_SCHEDULER), hostname (host), port (p) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class CompileFileMsg : public Msg {
public:
  CompileFileMsg (CompileJob *j, bool delete_job = false)
      : Msg(M_COMPILE_FILE), deleteit( delete_job ), job( j ) {}
  ~CompileFileMsg() { if ( deleteit ) delete job; }
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
  CompileJob *takeJob();

private:
  bool deleteit;
  CompileJob *job;
};

class FileChunkMsg : public Msg {
public:
  unsigned char* buffer;
  size_t len;
  bool del_buf;

  FileChunkMsg (unsigned char *_buffer, size_t _len)
      : Msg(M_FILE_CHUNK), buffer( _buffer ), len( _len ), del_buf(false) {}
  FileChunkMsg() : Msg( M_FILE_CHUNK ), buffer( 0 ), len( 0 ), del_buf(true) {}
  ~FileChunkMsg();
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class CompileResultMsg : public Msg {
public:
  int status;
  std::string out;
  std::string err;

  CompileResultMsg () : Msg(M_COMPILE_RESULT) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class JobBeginMsg : public Msg {
public:
  unsigned int job_id;
  unsigned int stime;
  JobBeginMsg () : Msg(M_JOB_BEGIN) {}
  JobBeginMsg (unsigned int j) : Msg(M_JOB_BEGIN), job_id(j), stime(time(0)) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class JobDoneMsg : public Msg {
public:
  unsigned int job_id;
  JobDoneMsg () : Msg(M_JOB_DONE) {}
  JobDoneMsg (unsigned int i) : Msg(M_JOB_DONE), job_id(i) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class LoginMsg : public Msg {
public:
  unsigned int port;
  LoginMsg (unsigned int myport)
      : Msg(M_LOGIN), port( myport ) {}
  LoginMsg () : Msg(M_LOGIN), port( 0 ) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

class StatsMsg : public Msg {
public:
  double load[3];
  StatsMsg () : Msg(M_STATS) { load[0] = load[1] = load[2] = 0; }
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd) const;
};

#endif
