#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include "job.h"
#include "comm.h"

using namespace std;

/* TODO
 * buffered in/output per MsgChannel
    + move read* into MsgChannel, create buffer-fill function
    + add timeouting select() there, handle it in the different
    + read* functions.
    + write* unbuffered / or per message buffer (flush in send_msg)
 * think about error handling
    + saving errno somewhere (in MsgChannel class)
 */   
bool
readfull (int fd, void *_buf, size_t count)
{
  char *buf = (char*)_buf;
  while (count)
    {
      ssize_t ret = read (fd, buf, count);
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
writefull (int fd, const void *_buf, size_t count)
{
  const char *buf = (const char*)_buf;
  while (count)
    {
      ssize_t ret = write (fd, buf, count);
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

Service::Service (struct sockaddr *_a, socklen_t _l)
{
  c = 0;
  len = _l;
  if (len && _a)
    {
      addr = (struct sockaddr *)malloc (len);
      memcpy (addr, _a, len);
      name = inet_ntoa (((struct sockaddr_in *) addr)->sin_addr);
      port = ntohs (((struct sockaddr_in *)addr)->sin_port);
    }
  else
    {
      addr = 0;
      name = "";
      port = 0;
    }
}

Service::Service (const string &hostname, unsigned short p)
{
  int remote_fd;
  struct sockaddr_in remote_addr;
  c = 0;
  addr = 0;
  port = 0;
  name = "";
  if ((remote_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      return;
    }
  struct hostent *host = gethostbyname (hostname.c_str());
  if (!host)
    {
      fprintf (stderr, "Unknown host\n");
      close (remote_fd);
      return;
    }
  if (host->h_length != 4)
    {
      fprintf (stderr, "Invalid address length\n");
      close (remote_fd);
      return;
    }
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons (p);
  memcpy (&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
  if (connect (remote_fd, (struct sockaddr *) &remote_addr, sizeof (remote_addr)) < 0)
    {
      perror ("connect()");
      close (remote_fd);
      return;
    }
  len = sizeof (remote_addr);
  addr = (struct sockaddr *)malloc (len);
  memcpy (addr, &remote_addr, len);
  name = hostname;
  port = p;
  c = new MsgChannel (remote_fd, this);
}

Service::~Service ()
{
  if (addr)
    free (addr);
}

MsgChannel::MsgChannel (int _fd)
  : other_end(0), fd(_fd)
{
}

MsgChannel::MsgChannel (int _fd, Service *serv)
  : other_end(serv), fd(_fd)
{
}

MsgChannel::~MsgChannel()
{
  close (fd);
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
  case M_GET_CS: m = new GetCSMsg; break;
  case M_USE_CS: m = new UseCSMsg; break;
  case M_COMPILE_FILE: m = new CompileFileMsg (new CompileJob); break;
  case M_FILE_CHUNK: m = new FileChunkMsg; break;
  case M_COMPILE_RESULT: m = new CompileResultMsg; break;
  case M_JOB_BEGIN: m = new JobBeginMsg; break;
  case M_JOB_DONE: m = new JobDoneMsg; break;
  case M_STATS: m = new StatsMsg; break;
  default: return 0; break;
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
read_string (int fd, string &s)
{
  char *buf;
  // len is including the (also saved) 0 Byte
  unsigned int len;
  if (!readuint (fd, &len))
    return 0;
  buf = new char[len];
  if (!readfull (fd, buf, len))
    {
      s = "";
      delete [] buf;
      return false;
    }
  s = buf;
  delete [] buf;
  return true;
}

bool
write_string (int fd, const string &s)
{
  unsigned int len = 1 + s.length();
  if (!writeuint (fd, len))
    return false;
  return writefull (fd, s.data(), len);
}

bool
read_strlist (int fd, list<string> &l)
{
  unsigned int len;
  l.clear();
  if (!readuint (fd, &len))
    return false;
  while (len--)
    {
      string s;
      if (!read_string (fd, s))
        return false;
      l.push_back (s);
    }
  return true;
}

bool
write_strlist (int fd, const list<string> &l)
{
  if (!writeuint (fd, (unsigned int) l.size()))
    return false;
  for (list<string>::const_iterator it = l.begin();
       it != l.end(); ++it )
    {
      if (!write_string (fd, *it))
        return false;
    }
  return true;
}

bool
Msg::fill_from_fd (int)
{
  return true;
}

bool
Msg::send_to_fd (int fd) const
{
  return writeuint (fd, (unsigned int) type);
}

bool
GetCSMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  if (!read_string (fd, version)
      || !read_string (fd, filename)
      || !readuint (fd, &filesize))
    return false;
  return true;
}

bool
GetCSMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  if (!write_string (fd, version)
      || !write_string (fd, filename)
      || !writeuint (fd, filesize))
    return false;
  return true;
}

bool
UseCSMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  bool ret = (readuint (fd, &job_id)
  	  && readuint (fd, &port)
          && read_string (fd, hostname));
  port = 10245;
  return ret;
}

bool
UseCSMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return (writeuint (fd, job_id)
  	  && writeuint (fd, port)
          && write_string (fd, hostname));
}

bool
CompileFileMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  unsigned int id, lang;
  list<string> l1, l2;
  if (!readuint (fd, &lang)
      || !readuint (fd, &id)
      || !read_strlist (fd, l1)
      || !read_strlist (fd, l2))
    return false;
  job->setLanguage ((CompileJob::Language) lang);
  job->setJobID (id);
  job->setRemoteFlags (l1);
  job->setRestFlags (l2);
  return true;
}

bool
CompileFileMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return (writeuint (fd, (unsigned int) job->language())
  	  && writeuint (fd, job->jobID())
  	  && write_strlist (fd, job->remoteFlags())
          && write_strlist (fd, job->restFlags()));
}

bool
FileChunkMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return true;
}

bool
FileChunkMsg::send_to_fd (int fd) const
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
CompileResultMsg::send_to_fd (int fd) const
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
  return readuint (fd, &job_id)
  	 && readuint (fd, &stime);
}

bool
JobBeginMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return writeuint (fd, job_id)
  	 && writeuint (fd, stime);
}

bool
JobDoneMsg::fill_from_fd (int fd)
{
  if (!Msg::fill_from_fd (fd))
    return false;
  return readuint (fd, &job_id);
}

bool
JobDoneMsg::send_to_fd (int fd) const
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
StatsMsg::send_to_fd (int fd) const
{
  if (!Msg::send_to_fd (fd))
    return false;
  return true;
}
