enum MsgType {
  M_PING = 'A',
  M_END, // End of all kinds of message chunks
  // Fake message used in message chunk looks
  M_TIMEOUT, 

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
  M_JOB_DONE,
  M_STATS
};

class Msg {
  enum MsgType type;
  size_t size;
};

// an endpoint of a MsgChannel, i.e. most often a host
class Service {
  struct sockaddr *addr;
  const char *name; // ???
};

class MsgChannel {
  Service *receiver;
  // our filedesc
  int fd;
  // NULL  <--> channel closed
  Msg *get_msg(void);
  // false <--> error (msg not send)
  bool send_msg (Msg *);
  // return last error (0 == no error)
  int error(void);
};

class PingMsg : public Msg {
};

class EndMsg : public Msg {
};

class CompileReqMsg : public Msg {
};

class GetCSMsg : public Msg {
};

class UseCSMsg : public Msg {
};

class CompileFileMsg : public Msg {
};

class FileChunkMsg : public Msg {
};

class CompileResultMsg : public Msg {
};

class JobDoneMsg : public Msg {
};

class StatsMsg : public Msg {
};
