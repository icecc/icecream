enum MsgType {
  M_UNKNOWN = 'A',
  M_PING,
  M_END, // End of all kinds of message chunks
  // Fake message used in message chunk looks
  M_TIMEOUT, 
  // ??? maybe use M_END
  M_DISCONNECT,

  // C <--> CD
  // Not sure, if we need those, or use a different protocol for
  // C <--> CD communication
  M_COMPILE_REQ,
  M_COMPILE_DONE,

  // CD --> S
  M_GET_CS,
  // S --> CD
  M_USE_CS,
  // CD --> CS
  M_COMPILE_FILE,
  // generic file transfer
  M_FILE_CHUNK,
  // CS --> CD
  M_COMPILE_RESULT,

  // CS --> S
  M_JOB_BEGIN,
  M_JOB_DONE,
  M_STATS
};

class Msg {
  enum MsgType type;
public:
  Msg (enum MsgType t) : type(t) {}
  virtual ~Msg () {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

// an endpoint of a MsgChannel, i.e. most often a host
class Service {
  struct sockaddr *addr;
  const std::string name; // ???
};

class MsgChannel {
  Service &other_end;
  // our filedesc
  int fd;
  // NULL  <--> channel closed
  Msg *get_msg(void);
  // false <--> error (msg not send)
  bool send_msg (const Msg &);
  // return last error (0 == no error)
  int error(void);
};

class PingMsg : public Msg {
public:
  PingMsg () : Msg(M_PING) {}
};

class EndMsg : public Msg {
public:
  EndMsg () : Msg(M_END) {}
};

class CompileReqMsg : public Msg {
public:
  CompileReqMsg () : Msg(M_COMPILE_REQ) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class TimeoutMsg : public Msg {
public:
  TimeoutMsg () : Msg(M_TIMEOUT) {}
};

class DisconnectMsg : public Msg {
public:
  DisconnectMsg () : Msg(M_DISCONNECT) {}
};

class CompileDoneMsg : public Msg {
public:
  CompileDoneMsg () : Msg(M_COMPILE_DONE) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class GetCSMsg : public Msg {
  const std::string version;
  const std::string filename;
  unsigned long filesize;
public:
  GetCSMsg () : Msg(M_GET_CS) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class UseCSMsg : public Msg {
  unsigned int job_id;
  const std::string hostname;
public:
  UseCSMsg () : Msg(M_USE_CS) {}
  UseCSMsg (Service &s, unsigned int id) : Msg(M_USE_CS), job_id(id),
    hostname (s.name) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class CompileFileMsg : public Msg {
public:
  CompileFileMsg () : Msg(M_COMPILE_FILE) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class FileChunkMsg : public Msg {
public:
  FileChunkMsg () : Msg(M_FILE_CHUNK) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class CompileResultMsg : public Msg {
public:
  CompileResultMsg () : Msg(M_COMPILE_RESULT) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class JobBeginMsg : public Msg {
public:
  unsigned int job_id;
  JobBeginMsg () : Msg(M_JOB_BEGIN) {}
  JobBeginMsg (unsigned int i) : Msg(M_JOB_BEGIN), job_id(i) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class JobDoneMsg : public Msg {
public:
  unsigned int job_id;
  JobDoneMsg () : Msg(M_JOB_DONE) {}
  JobDoneMsg (unsigned int i) : Msg(M_JOB_DONE), job_id(i) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

class StatsMsg : public Msg {
public:
  StatsMsg () : Msg(M_STATS) {}
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};
