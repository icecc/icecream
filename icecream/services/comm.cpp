#include <unistd.h>
#include <errno.h>
#include "comm.h"

bool
readfull (int fd, char *buf, size_t count)
{
  while (count)
    {
      size_t ret = read (fd, buf, count);
      if (ret < 0 && (errno == EINTR || errno == EAGAIN))
	continue;
      // EOF or some error
      if (ret <= 0)
	break;
      count -= ret;
      buf += ret;
    }
  if (count)
    return false;
  return true;
}

bool
writefull (int fd, const char *buf, size_t count)
{
  while (count)
    {
      size_t ret = write (fd, buf, count);
      if (ret < 0 && (errno == EINTR || errno == EAGAIN))
	continue;
      // XXX handle EPIPE ?
      // EOF or some error
      if (ret <= 0)
	break;
      count -= ret;
      buf += ret;
    }
  if (count)
    return false;
  return true;
}

bool
readuint (int fd, unsigned int *buf)
{
  unsigned int b;
  *buf = 0;
  if (!readfull (fd, &b, 4))
    return false;
  *buf = ntohl (b);
  return true;
}

bool
writeuint (int fd, unsigned int i)
{
  i = htonl (i);
  return writefull (fd, &i, 4);
}

Msg *
MsgChannel::get_msg(void)
{
  Msg *m;
  enum MsgType type;
  unsigned int t;
  if (!readuint (fd, &t))
    return 0;
  type = (enum MsgType) t;
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

char *
read_string (int fd)
{
  char *buf;
  // len is including the (also saved) 0 Byte
  unsigned int len;
  if (!readuint (fd, &len))
    return 0;
  buf = new char[len];
  if (!readfull (fd, buf, len))
    {
      delete [] buf;
      return 0;
    }
  return buf;
}

bool
write_string (const char *s)
{
  if (!s)
    s = "";
  if (!writefull (fd, 1 + strlen (s)))
    return false;
  return writefull (fd, s, len);
}

bool
Msg::fill_from_fd (int fd)
{
  return true;
}

bool
Msg::send_to_fd (int fd)
{
  return writefull (fd, (unsigned int) type);
}

bool
CompileReqMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
CompileReqMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}

bool
CompileDoneMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
CompileDoneMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}

bool
GetCSMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  if ((version = read_string (fd)) == 0
      || (filename = read_string (fd)) == 0
      || !readuint (fd, &filesize))
    return false;
  return true;
}

bool
GetCSMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  if (!write_string (version)
      || !write_string (filename)
      || !writefull (fd, filesize))
    return false;
  return true;
}

bool
UseCSMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return (readuint (fd, &job_id)
          && (hostname = read_string (fd)) != 0)
}

bool
UseCSMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return (writeuint (fd, jobid)
          && write_string (fd, hostname));
}

bool
CompileFileMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
CompileFileMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}

bool
FileChunkMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
FileChunkMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}

bool
CompileResultMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
CompileResultMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}

bool
JobBeginMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return readuint (fd, &job_id);
}

bool
JobBeginMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint (fd, job_id);
}

bool
JobDoneMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return readuint (fd, &job_id);
}

bool
JobDoneMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint (fd, job_id);
}

bool
StatsMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
StatsMsg::send_to_fd (int fd)
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}
