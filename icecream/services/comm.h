enum MsgType {
  M_UNKNOWN = 'A',
  M_PING,
  M_END, // End of all kinds of message chunks
  // Fake message used in message chunk looks
  M_TIMEOUT, 
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
  virtual bool fill_from_fd (int fd);
  virtual bool send_to_fd (int fd);
};

// an endpoint of a MsgChannel, i.e. most often a host
class Service {
  struct sockaddr *addr;
  const char *name; // ???
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

#define DECL_MSG_SIMPLE(NAME,ENUM)	\
class NAME : public Msg {	\
public:				\
  NAME () : Msg(ENUM) {}	\
};

#define DECL_MSG(NAME,ENUM)	\
class NAME : public Msg {	\
public:				\
  NAME () : Msg(ENUM) {}	\
  virtual bool fill_from_fd (int fd);	\
  virtual bool send_to_fd (int fd);	\
};

DECL_MSG_SIMPLE (PingMsg, M_PING)
DECL_MSG_SIMPLE (EndMsg, M_END)
DECL_MSG (CompileReqMsg, M_COMPILE_REQ)
DECL_MSG_SIMPLE (TimeoutMsg, M_TIMEOUT)
DECL_MSG_SIMPLE (DisconnectMsg, M_DISCONNECT)
DECL_MSG (CompileDoneMsg, M_COMPILE_DONE)
DECL_MSG (GetCSMsg, M_GET_CS)
DECL_MSG (UseCSMsg, M_USE_CS)
DECL_MSG (CompileFileMsg, M_COMPILE_FILE)
DECL_MSG (FileChunkMsg, M_FILE_CHUNK)
DECL_MSG (CompileResultMsg, M_COMPILE_RESULT)
DECL_MSG (JobBeginMsg, M_JOB_BEGIN)
DECL_MSG (JobDoneMsg, M_JOB_DONE)
DECL_MSG (StatsMsg, M_STATS)
