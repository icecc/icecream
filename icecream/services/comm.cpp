#include "comm.h"

Msg *
MsgChannel::get_msg(void)
{
  Msg *m;
  enum MsgType type;
  unsigned int t;
  if (read (fd, &t, 4) != 4)
    return 0;
  type = (enum MsgType) ntohl (t);
  switch (type) {
  case M_UNKNOWN: return 0;
  case M_PING: m = new PingMsg; break;
  case M_END:  m = new EndMsg; break;
  case M_TIMEOUT: m = new TimeoutMsg; break;
  case M_DISCONNECT: m = new DisconnectMsg; break;
  case M_COMPILE_REQ: m = new CompileReqMsg; break;
  case M_COMPILE_DONE: m = new CompileDoneMsg; break;
  case M_GET_CS: m = new GetCSMsg; break;
  case M_USE_CS: m = new UseCSMsg; break;
  case M_COMPILE_FILE: m = new CompileFileMsg; break;
  case M_FILE_CHUNK: m = new FileChunkMsg; break;
  case M_COMPILE_RESULT: m = new CompileResultMsg; break;
  case M_JOB_BEGIN: m = new JobBeginMsg; break;
  case M_JOB_DONE: m = new JobDoneMsg; break;
  case M_STATS: m = new StatsMsg; break;
  }
  if (!m->fill_from_fd (fd))
    {
      delete m;
      return 0;
    }
  return m;
}

bool
MsgChannel::send_msg (const Msg &m)
{
  return m.send_to_fd (fd);
}

bool
Msg::fill_from_fd (int fd)
{
  return true;
}

bool
Msg::send_to_fd (int fd)
{
  unsigned int t = type;
  t = htonl (t);
  if (write (fd, &t, 4) != 4)
    return false;
  return true;
}

bool
CompileReqMsg::fill_from_fd (int fd)
{
  return true;
}

bool
CompileReqMsg::send_to_fd (int fd)
{
  return true;
}

bool
CompileDoneMsg::fill_from_fd (int fd)
{
  return true;
}

bool
CompileDoneMsg::send_to_fd (int fd)
{
  return true;
}

bool
GetCSMsg::fill_from_fd (int fd)
{
  return true;
}

bool
GetCSMsg::send_to_fd (int fd)
{
  return true;
}

bool
UseCSMsg::fill_from_fd (int fd)
{
  return true;
}

bool
UseCSMsg::send_to_fd (int fd)
{
  return true;
}

bool
CompileFileMsg::fill_from_fd (int fd)
{
  return true;
}

bool
CompileFileMsg::send_to_fd (int fd)
{
  return true;
}

bool
FileChunkMsg::fill_from_fd (int fd)
{
  return true;
}

bool
FileChunkMsg::send_to_fd (int fd)
{
  return true;
}

bool
CompileResultMsg::fill_from_fd (int fd)
{
  return true;
}

bool
CompileResultMsg::send_to_fd (int fd)
{
  return true;
}

bool
JobBeginMsg::fill_from_fd (int fd)
{
  return true;
}

bool
JobBeginMsg::send_to_fd (int fd)
{
  return true;
}

bool
JobDoneMsg::fill_from_fd (int fd)
{
  return true;
}

bool
JobDoneMsg::send_to_fd (int fd)
{
  return true;
}

bool
StatsMsg::fill_from_fd (int fd)
{
  return true;
}

bool
StatsMsg::send_to_fd (int fd)
{
  return true;
}
