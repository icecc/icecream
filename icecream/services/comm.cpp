/*  -*- mode: C++; c-file-style: "gnu"; fill-column: 78 -*- */

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <iostream>
#include <assert.h>
#include <minilzo.h>

#include "logging.h"
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
 * handle unknown messages (implement a UnknownMsg holding the content
    of the whole data packet?)
 */

/* Tries to fill the inbuf completely.  */
bool
MsgChannel::read_a_bit (void)
{
  chop_input ();
  size_t count = inbuflen - inofs;
  if (count < 128)
    {
      inbuflen = (inbuflen + 128 + 127) & ~(size_t)127;
      inbuf = (char *) realloc (inbuf, inbuflen);
      count = inbuflen - inofs;
    }
  char *buf = inbuf + inofs;
  bool error = false;
  while (count)
    {
      ssize_t ret = read (fd, buf, count);
      if (ret > 0)
        {
          count -= ret;
          buf += ret;
	}
      else if (ret < 0 && errno == EINTR)
        continue;
      else if (ret < 0)
        {
          // EOF or some error
          if (errno != EAGAIN)
	    error = true;
	}
      else if (ret == 0)
	eof = true;
      break;
    }
  inofs = buf - inbuf;
  update_state ();
  return !error;
}

void
MsgChannel::update_state (void)
{
  switch (instate)
    {
    case NEED_LEN:
      if (inofs - intogo >= 4)
        {
	  readuint32 (inmsglen);
	  if (inbuflen - intogo < inmsglen)
	    {
	      inbuflen = (inmsglen + intogo + 127) & ~(size_t)127;
	      inbuf = (char *) realloc (inbuf, inbuflen);
	    }
          instate = FILL_BUF;
          /* FALLTHROUGH */
	}
      else
        break;
    case FILL_BUF:
      if (inofs - intogo >= inmsglen)
        instate = HAS_MSG;
        /* FALLTHROUGH */
      else
        break;
    case HAS_MSG:
      /* handled elsewere */
      break;
    }
}

void
MsgChannel::chop_input (void)
{
  /* Make buffer smaller, if there's much already read in front
     of it, or it is cheap to do.  */
  if (intogo > 8192
      || inofs - intogo <= 16)
    {
      if (inofs - intogo != 0)
        memmove (inbuf, inbuf + intogo, inofs - intogo);
      inofs -= intogo;
      intogo = 0;
    }
}

void
MsgChannel::chop_output (void)
{
  if (msgofs > 8192
      || msgtogo <= 16)
    {
      if (msgtogo)
        memmove (msgbuf, msgbuf + msgofs, msgtogo);
      msgofs = 0;
    }
}

void
MsgChannel::writefull (const void *_buf, size_t count)
{
  if (msgtogo + count >= msgbuflen)
    {
      /* Realloc to a multiple of 128.  */
      msgbuflen = (msgtogo + count + 127) & ~(size_t)127;
      msgbuf = (char *) realloc (msgbuf, msgbuflen);
    }
  memcpy (msgbuf + msgtogo, _buf, count);
  msgtogo += count;
}

bool
MsgChannel::flush_writebuf (bool blocking)
{
  const char *buf = msgbuf + msgofs;
  bool error = false;
  while (msgtogo)
    {
      ssize_t ret = write (fd, buf, msgtogo);
      if (ret < 0)
        {
	  if (errno == EINTR)
	    continue;
	  /* If we want to write blocking, but couldn't write anything,
	     select on the fd.  */
	  if (blocking && errno == EAGAIN)
	    {
	      fd_set write_set;
	      FD_ZERO (&write_set);
	      FD_SET (fd, &write_set);
	      struct timeval tv;
	      tv.tv_sec = 5 * 60;
	      tv.tv_usec = 0;
	      if (select (fd + 1, NULL, &write_set, NULL, &tv) > 0)
	        continue;
	      /* Timeout or real error --> error.  */
	    }
	  // XXX handle EPIPE ?
	  error = true;
	  break;
	}
      else if (ret == 0)
        {
	  // EOF while writing --> error
	  error = true;
	  break;
	}
      msgtogo -= ret;
      buf += ret;
    }
  msgofs = buf - msgbuf;
  chop_output ();
  return !error;
}

void
MsgChannel::readuint32 (uint32_t &buf)
{
  if (inofs >= intogo + 4)
    {
      buf = *(uint32_t *)(inbuf + intogo);
      intogo += 4;
      buf = ntohl (buf);
    }
  else
    buf = 0;
}

void
MsgChannel::writeuint32 (uint32_t i)
{
  i = htonl (i);
  writefull (&i, 4);
}

void
MsgChannel::read_string (string &s)
{
  char *buf;
  // len is including the (also saved) 0 Byte
  uint32_t len;
  readuint32 (len);
  if (!len || inofs < intogo + len)
    s = "";
  else
    {
      buf = inbuf + intogo;
      intogo += len;
      s = buf;
    }
}

void
MsgChannel::write_string (const string &s)
{
  uint32_t len = 1 + s.length();
  writeuint32 (len);
  writefull (s.c_str(), len);
}

void
MsgChannel::read_strlist (list<string> &l)
{
  uint32_t len;
  l.clear();
  readuint32 (len);
  while (len--)
    {
      string s;
      read_string (s);
      l.push_back (s);
    }
}

void
MsgChannel::write_strlist (const list<string> &l)
{
  writeuint32 ((uint32_t) l.size());
  for (list<string>::const_iterator it = l.begin();
       it != l.end(); ++it )
    write_string (*it);
}

void
MsgChannel::readcompressed (unsigned char **uncompressed_buf,
			    size_t &_uclen, size_t &_clen)
{
  lzo_uint uncompressed_len;
  lzo_uint compressed_len;
  readuint32 (uncompressed_len);
  readuint32 (compressed_len);
  /* If there was some input, but nothing compressed, or we don't have
     everything to uncompress, there was an error.  */
  if ((uncompressed_len && !compressed_len)
      || inofs < intogo + compressed_len)
    {
      *uncompressed_buf = 0;
      uncompressed_len = 0;
      _uclen = uncompressed_len;
      _clen = compressed_len;
      return;
    }

  *uncompressed_buf = new unsigned char[uncompressed_len];
  if (uncompressed_len && compressed_len)
    {
      const lzo_byte *compressed_buf = (lzo_byte *) (inbuf + intogo);
      lzo_voidp wrkmem = (lzo_voidp) malloc (LZO1X_MEM_COMPRESS);
      int ret = lzo1x_decompress (compressed_buf, compressed_len,
			      *uncompressed_buf, &uncompressed_len, wrkmem);
      free (wrkmem);
      if (ret != LZO_E_OK)
        {
          /* This should NEVER happen.
	     Remove the buffer, and indicate there is nothing in it,
	     but don't reset the compressed_len, so our caller know,
	     that there actually was something read in.  */
          printf("internal error - decompression failed: %d\n", ret);
	  delete [] *uncompressed_buf;
	  *uncompressed_buf = 0;
	  uncompressed_len = 0;
        }
    }
  /* Read over everything used, _also_ if there was some error.
     If we couldn't decode it now, it won't get better in the future,
     so just ignore this hunk.  */
  intogo += compressed_len;
  _uclen = uncompressed_len;
  _clen = compressed_len;
}

void
MsgChannel::writecompressed (const unsigned char *in_buf,
			     size_t _in_len, size_t &_out_len)
{
  lzo_uint in_len = _in_len;
  lzo_uint out_len = _out_len;
  out_len = in_len + in_len / 64 + 16 + 3;
  writeuint32 (in_len);
  size_t msgtogo_old = msgtogo;
  writeuint32 (0); // will be out_len
  if (msgtogo + out_len >= msgbuflen)
    {
      /* Realloc to a multiple of 128.  */
      msgbuflen = (msgtogo + out_len + 127) & ~(size_t)127;
      msgbuf = (char *) realloc (msgbuf, msgbuflen);
    }
  lzo_byte *out_buf = (lzo_byte *) (msgbuf + msgtogo);
  lzo_voidp wrkmem = (lzo_voidp) malloc (LZO1X_MEM_COMPRESS);
  int ret = lzo1x_1_compress (in_buf, in_len, out_buf, &out_len, wrkmem);
  free (wrkmem);
  if (ret != LZO_E_OK)
    {
      /* this should NEVER happen */
      printf ("internal error - compression failed: %d\n", ret);
      out_len = 0;
    }
  uint32_t _olen = htonl (out_len);
  memcpy (msgbuf + msgtogo_old, &_olen, 4);
  msgtogo += out_len;
  _out_len = out_len;
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
  new MsgChannel (remote_fd, this); // sets c
}

Service::~Service ()
{
  if (addr)
    free (addr);
  if ( c )
    {
      assert( c->other_end == this );
      c->other_end = 0;
    }
  delete c;
}

bool
Service::eq_ip (const Service &s)
{
  struct sockaddr_in *s1, *s2;
  s1 = (struct sockaddr_in *) addr;
  s2 = (struct sockaddr_in *) s.addr;
  return (len == s.len
          && memcmp (&s1->sin_addr, &s2->sin_addr, sizeof (s1->sin_addr)) == 0);
}

MsgChannel::MsgChannel (int _fd)
  : other_end(0), fd(_fd), msgbuf(0), msgbuflen(0), msgofs(0), msgtogo(0),
    inbuf(0), inbuflen(0), inofs(0), intogo(0), instate(NEED_LEN), eof(false)
{
  abort (); // currently not tested
}

MsgChannel::MsgChannel (int _fd, Service *serv)
  : other_end(serv), fd(_fd)
{
  assert (!serv->c);
  serv->c = this;
  // not using new/delete because of the need of realloc()
  msgbuf = (char *) malloc (128);
  msgbuflen = 128;
  msgofs = 0;
  msgtogo = 0;
  inbuf = (char *) malloc (128);
  inbuflen = 128;
  inofs = 0;
  intogo = 0;
  instate = NEED_LEN;
  eof = false;
  if (fcntl (fd, F_SETFL, O_NONBLOCK) < 0)
    perror ("fcntl()"); // XXX
}

MsgChannel::~MsgChannel()
{
  close (fd);
  fd = 0;
  if (other_end)
    {
      assert( !other_end->c || other_end->c == this );
      other_end->c = 0;
    }
  delete other_end;
  if (msgbuf)
    free (msgbuf);
  if (inbuf)
    free (inbuf);
}

/* This waits indefinitely (well, 5 minutes) for some a complete
   message to arrive.  Returns false if there was some error.  */
bool
MsgChannel::wait_for_msg (void)
{
  if (has_msg ())
    return true;
  if (!read_a_bit ()) {
    trace() << "!read_a_bit\n";
    return false;
  }
  while (!has_msg ())
    {
      fd_set read_set;
      FD_ZERO (&read_set);
      FD_SET (fd, &read_set);
      struct timeval tv;
      tv.tv_sec = 5 * 60;
      tv.tv_usec = 0;
      if (select (fd + 1, &read_set, NULL, NULL, &tv) <= 0) {
        if ( errno == EINTR )
          continue;
        perror( "select" );
	/* Either timeout or real error.  For this function also
	   a timeout is an error.  */
        return false;
      }
      if (!read_a_bit ()) {
        trace() << "!read_a_bit 2\n";
        return false;
      }
    }
  return true;
}

Msg *
MsgChannel::get_msg(bool blocking)
{
  Msg *m;
  enum MsgType type;
  unsigned int t;
  if (blocking && !wait_for_msg () ) {
    trace() << "blocking && !waiting_for_msg()\n";
    return 0;
  }
  /* If we've seen the EOF, and we don't have a complete message,
     then we won't see it anymore.  Return that to the caller.
     Don't use has_msg() here, as it returns true for eof.  */
  if (eof && instate != HAS_MSG) {
    trace() << "eof && !HAS_MSG\n";
    return 0;
  }
  if (!has_msg ()) {
    trace() << "saw eof without msg! " << eof << " " << instate << endl;
    return 0;
    abort (); // XXX but what else?
  }
  readuint32 (t);
  type = (enum MsgType) t;
  switch (type)
    {
    case M_UNKNOWN: return 0;
    case M_PING: m = new PingMsg; break;
    case M_END:  m = new EndMsg; break;
    case M_TIMEOUT: m = new TimeoutMsg; break;
    case M_GET_CS: m = new GetCSMsg; break;
    case M_USE_CS: m = new UseCSMsg; break;
    case M_COMPILE_FILE: m = new CompileFileMsg (new CompileJob, true); break;
    case M_FILE_CHUNK: m = new FileChunkMsg; break;
    case M_COMPILE_RESULT: m = new CompileResultMsg; break;
    case M_JOB_BEGIN: m = new JobBeginMsg; break;
    case M_JOB_DONE: m = new JobDoneMsg; break;
    case M_LOGIN: m = new LoginMsg; break;
    case M_STATS: m = new StatsMsg; break;
    case M_GET_SCHEDULER: m = new GetSchedulerMsg; break;
    case M_USE_SCHEDULER: m = new UseSchedulerMsg; break;
    case M_MON_LOGIN: m = new MonLoginMsg; break;
    case M_MON_GET_CS: m = new MonGetCSMsg; break;
    case M_MON_JOB_BEGIN: m = new MonJobBeginMsg; break;
    case M_MON_JOB_DONE: m = new MonJobDoneMsg; break;
    case M_MON_STATS: m = new MonStatsMsg; break;
    case M_JOB_LOCAL_BEGIN: m = new JobLocalBeginMsg; break;
    case M_JOB_LOCAL_ID: m = new JobLocalId; break;
    case M_JOB_LOCAL_DONE: m = new JobLocalDoneMsg; break;
    case M_MON_LOCAL_JOB_BEGIN: m = new MonLocalJobBeginMsg; break;
    case M_MON_LOCAL_JOB_DONE: m = new MonLocalJobDoneMsg; break;
    case M_TRANFER_ENV: m = new EnvTransferMsg; break;
    }
  m->fill_from_channel (this);
  instate = NEED_LEN;
  update_state ();
  return m;
}

bool
MsgChannel::send_msg (const Msg &m, bool blocking)
{
  chop_output ();
  size_t msgtogo_old = msgtogo;
  writeuint32 (0);  // filled out later with the overall len
  m.send_to_channel (this);
  uint32_t len = htonl (msgtogo - msgtogo_old - 4);
  memcpy (msgbuf + msgtogo_old, &len, 4);
  return flush_writebuf (blocking);
}

MsgChannel *Service::createChannel( int remote_fd )
{
  if (channel())
    {
      assert( remote_fd == c->fd );
      return channel();
    }
  new MsgChannel( remote_fd, this ); // sets c
  assert( channel() );
  return channel();
}

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>

/* Returns a filedesc. or a negative value for errors.  */
static int
open_send_broadcast (void)
{
  int ask_fd;
  struct sockaddr_in remote_addr;
  if ((ask_fd = socket (PF_INET, SOCK_DGRAM, 0)) < 0)
    {
      perror ("socket()");
      return -1;
    }

  int optval = 1;
  if (setsockopt (ask_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0)
    {
      perror ("setsockopt()");
      close (ask_fd);
      return -1;
    }

  struct ifaddrs *addrs;
  int ret = getifaddrs(&addrs);

  if ( ret < 0 )
    return ret;

  char buf = 42;
  for (struct ifaddrs *addr = addrs; addr != NULL; addr = addr->ifa_next)
    {
      /*
       * See if this interface address is IPv4...
       */

      if (addr->ifa_addr == NULL || addr->ifa_addr->sa_family != AF_INET ||
	  addr->ifa_netmask == NULL || addr->ifa_name == NULL)
	continue;

      if ( ntohl( ( ( struct sockaddr_in* ) addr->ifa_addr )->sin_addr.s_addr ) == 0x7f000001 )
	{
	  trace() << "ignoring localhost " << addr->ifa_name << endl;
	  continue;
	}

      if ( ( addr->ifa_flags & IFF_POINTOPOINT ) || !( addr->ifa_flags & IFF_BROADCAST) )
	{
	  log_info() << "ignoring tunnels " << addr->ifa_name << endl;
	  continue;
	}

      if ( addr->ifa_broadaddr )
	{
	  log_info() << "broadcast "
		     << addr->ifa_name << " "
		     << inet_ntoa( ( ( sockaddr_in* )addr->ifa_broadaddr )->sin_addr )
		     << endl;

	  remote_addr.sin_family = AF_INET;
	  remote_addr.sin_port = htons (8765);
	  remote_addr.sin_addr = ( ( sockaddr_in* )addr->ifa_broadaddr )->sin_addr ;

	  if (sendto (ask_fd, &buf, 1, 0, (struct sockaddr*)&remote_addr,
		    sizeof (remote_addr)) != 1)
	    {
	      perror ("sendto()");
	    }
	}
    }
  freeifaddrs (addrs);
  return ask_fd;
}

#define BROAD_BUFLEN 16

static bool
get_broad_answer (int ask_fd, int timeout, char *buf2,
		  struct sockaddr_in *remote_addr,
		  socklen_t *remote_len)
{
  char buf = 42;
  fd_set read_set;
  FD_ZERO (&read_set);
  FD_SET (ask_fd, &read_set);
  struct timeval tv;
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = 1000 * (timeout % 1000);
  errno = 0;
  if (select (ask_fd + 1, &read_set, NULL, NULL, &tv) <= 0)
    {
      /* Normally this is a timeout, i.e. no scheduler there.  */
      if (errno)
	perror ("waiting for scheduler");
      return false;
    }
  *remote_len = sizeof (struct sockaddr_in);
  if (recvfrom (ask_fd, buf2, BROAD_BUFLEN, 0, (struct sockaddr*) remote_addr,
		remote_len) != BROAD_BUFLEN)
    {
      perror ("recvfrom()");
      return false;
    }
  if (buf + 1 != buf2[0])
    {
      fprintf (stderr, "wrong answer\n");
      return false;
    }
  buf2[BROAD_BUFLEN - 1] = 0;
  return true;
}

MsgChannel *
connect_scheduler (const string &_netname, int timeout)
{
  const char *get = getenv( "USE_SCHEDULER" );
  string hostname;
  unsigned int sport = 8765;
  char buf2[BROAD_BUFLEN];

  string netname = _netname;
  if (netname.empty())
    netname = "ICECREAM";

  if (get)
    {
      hostname = get;
      netname = "";
    }
  else
    {
      int ask_fd;
      struct sockaddr_in remote_addr;
      socklen_t remote_len;
      time_t time0 = time (0);
      bool found = false;

      ask_fd = open_send_broadcast ();

      do
        {
	  bool first = true;
	  /* Read/test all arriving packages.  */
	  while (!found
	  	 && get_broad_answer (ask_fd, first ? timeout : 0, buf2,
	  			      &remote_addr, &remote_len))
	    {
	      first = false;
	      if (strcasecmp (netname.c_str(), buf2 + 1) == 0)
	        found = true;
	    }
	  if (found)
	    break;
	}
      while (time (0) - time0 < (timeout / 1000));
      close (ask_fd);
      if (!found)
        return 0;
      hostname = inet_ntoa (remote_addr.sin_addr);
      sport = ntohs (remote_addr.sin_port);
      netname = buf2 + 1;
    }

  printf ("scheduler is on %s:%d (net %s)\n", hostname.c_str(), sport,
	  netname.c_str());
  Service *sched = new Service (hostname, sport);
  return sched->channel();
}

list<string>
get_netnames (int timeout)
{
  list<string> l;
  int ask_fd;
  struct sockaddr_in remote_addr;
  socklen_t remote_len;
  time_t time0 = time (0);

  ask_fd = open_send_broadcast ();

  do
    {
      char buf2[BROAD_BUFLEN];
      bool first = true;
      /* Read/test all arriving packages.  */
      while (get_broad_answer (ask_fd, first ? timeout : 0, buf2,
			       &remote_addr, &remote_len))
	{
	  first = false;
	  l.push_back (buf2 + 1);
	}
    }
  while (time (0) - time0 < (timeout / 1000));
  close (ask_fd);
  return l;
}

void
Msg::fill_from_channel (MsgChannel *)
{
}

void
Msg::send_to_channel (MsgChannel *c) const
{
  c->writeuint32 ((uint32_t) type);
}

void
GetCSMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  unsigned int _lang;
  c->read_string (version);
  c->read_string (filename);
  c->readuint32 (_lang);
  lang = static_cast<CompileJob::Language>( _lang );
}

void
GetCSMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->write_string (version);
  c->write_string (filename);
  c->writeuint32 ((uint32_t) lang);
}

void
UseCSMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->readuint32 (port);
  c->read_string (hostname);
  c->read_string (environment);
  c->readuint32( got_env );
}

void
UseCSMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 (port);
  c->write_string (hostname);
  c->write_string (environment);
  c->writeuint32( got_env );
}

void
CompileFileMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  unsigned int id, lang;
  list<string> _l1, _l2;
  string version;
  c->readuint32 (lang);
  c->readuint32 (id);
  c->read_strlist (_l1);
  c->read_strlist (_l2);
  c->read_string (version);
  job->setLanguage ((CompileJob::Language) lang);
  job->setJobID (id);
  ArgumentsList l;
  for (list<string>::const_iterator it = _l1.begin(); it != _l1.end(); ++it)
    l.append( *it, Arg_Remote );
  for (list<string>::const_iterator it = _l2.begin(); it != _l2.end(); ++it)
    l.append( *it, Arg_Rest );
  job->setFlags (l);
  job->setEnvironmentVersion (version);
}

void
CompileFileMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 ((uint32_t) job->language());
  c->writeuint32 (job->jobID());
  c->write_strlist (job->remoteFlags());
  c->write_strlist (job->restFlags());
  c->write_string (job->environmentVersion());
}

CompileJob *
CompileFileMsg::takeJob()
{
  assert (deleteit);
  deleteit = false;
  return job;
}

void
FileChunkMsg::fill_from_channel (MsgChannel *c)
{
  if (del_buf)
    delete [] buffer;
  buffer = 0;
  del_buf = true;

  Msg::fill_from_channel (c);
  c->readcompressed (&buffer, len, compressed);
}

void
FileChunkMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writecompressed (buffer, len, compressed);
}

FileChunkMsg::~FileChunkMsg()
{
  if (del_buf)
    delete [] buffer;
}

void
CompileResultMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  uint32_t _status = 0;
  c->read_string (err);
  c->read_string (out);
  c->readuint32 (_status);
  status = _status;
}

void
CompileResultMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->write_string (err);
  c->write_string (out);
  c->writeuint32 (status);
}

void
JobBeginMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->readuint32 (stime);
}

void
JobBeginMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 (stime);
}

void JobLocalBeginMsg::fill_from_channel( MsgChannel *c )
{
  Msg::fill_from_channel(c);
  c->readuint32(stime);
}

void JobLocalBeginMsg::send_to_channel( MsgChannel *c ) const
{
  Msg::send_to_channel( c );
  c->writeuint32(stime);
}

JobLocalDoneMsg::JobLocalDoneMsg (int id, int exit)
  : Msg(M_JOB_LOCAL_DONE), exitcode( exit ), job_id( id )
{
}

void JobLocalDoneMsg::fill_from_channel( MsgChannel *c )
{
  Msg::fill_from_channel(c);
  unsigned int error = 255;
  c->readuint32(error);
  c->readuint32(job_id);
  exitcode = ( int )error;
}

void JobLocalDoneMsg::send_to_channel( MsgChannel *c ) const
{
  Msg::send_to_channel( c );
  c->writeuint32(( int )exitcode);
  c->writeuint32(job_id);
}

void JobLocalId::fill_from_channel( MsgChannel *c )
{
  Msg::fill_from_channel(c);
  c->readuint32(job_id);
}

void JobLocalId::send_to_channel( MsgChannel *c ) const
{
  Msg::send_to_channel( c );
  c->writeuint32(job_id);
}

JobDoneMsg::JobDoneMsg (int id, int exit)
  : Msg(M_JOB_DONE),  exitcode( exit ), job_id( id )
{
  real_msec = 0;
  user_msec = 0;
  sys_msec = 0;
  maxrss = 0;
  idrss = 0;
  majflt = 0;
  nswap = 0;
  in_compressed = 0;
  in_uncompressed = 0;
  out_compressed = 0;
  out_uncompressed = 0;
}

void
JobDoneMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  uint32_t _exitcode = 255;
  c->readuint32 (job_id);
  c->readuint32 (_exitcode);
  c->readuint32 (real_msec);
  c->readuint32 (user_msec);
  c->readuint32 (sys_msec);
  c->readuint32 (maxrss);
  c->readuint32 (idrss);
  c->readuint32 (majflt);
  c->readuint32 (nswap);
  c->readuint32 (in_compressed);
  c->readuint32 (in_uncompressed);
  c->readuint32 (out_compressed);
  c->readuint32 (out_uncompressed);
  exitcode = (int) _exitcode;
}

void
JobDoneMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 ((uint32_t) exitcode);
  c->writeuint32 (real_msec);
  c->writeuint32 (user_msec);
  c->writeuint32 (sys_msec);
  c->writeuint32 (maxrss);
  c->writeuint32 (idrss);
  c->writeuint32 (majflt);
  c->writeuint32 (nswap);
  c->writeuint32 (in_compressed);
  c->writeuint32 (in_uncompressed);
  c->writeuint32 (out_compressed);
  c->writeuint32 (out_uncompressed);
}

void
LoginMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (port);
  c->readuint32 (max_kids);
  c->read_strlist (envs);
}

void
LoginMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (port);
  c->writeuint32 (max_kids);
  c->write_strlist (envs);
}

void
StatsMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (load);
  c->readuint32 (niceLoad);
  c->readuint32 (sysLoad);
  c->readuint32 (userLoad);
  c->readuint32 (idleLoad);
  c->readuint32 (loadAvg1);
  c->readuint32 (loadAvg5);
  c->readuint32 (loadAvg10);
  c->readuint32 (freeMem);
}

void
StatsMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (load);
  c->writeuint32 (niceLoad);
  c->writeuint32 (sysLoad);
  c->writeuint32 (userLoad);
  c->writeuint32 (idleLoad);
  c->writeuint32 (loadAvg1);
  c->writeuint32 (loadAvg5);
  c->writeuint32 (loadAvg10);
  c->writeuint32 (freeMem);
}

void
GetSchedulerMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
}

void
GetSchedulerMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
}

void
UseSchedulerMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (port);
  c->read_string (hostname);
}

void
UseSchedulerMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (port);
  c->write_string (hostname);
}

void
MonGetCSMsg::fill_from_channel (MsgChannel *c)
{
  GetCSMsg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->read_string (client);
}

void
MonGetCSMsg::send_to_channel (MsgChannel *c) const
{
  GetCSMsg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->write_string (client);
}

void
MonJobBeginMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->readuint32 (stime);
  c->read_string (host);
}

void
MonJobBeginMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 (stime);
  c->write_string (host);
}

void MonLocalJobBeginMsg::fill_from_channel (MsgChannel * c)
{
  Msg::fill_from_channel(c);
  c->read_string(host);
  c->readuint32( job_id );
  c->readuint32( stime );
}

void MonLocalJobBeginMsg::send_to_channel (MsgChannel * c) const
{
  Msg::send_to_channel(c);
  c->write_string( host );
  c->writeuint32( job_id );
  c->writeuint32( stime );
}

void
MonStatsMsg::fill_from_channel (MsgChannel *c)
{
  StatsMsg::fill_from_channel (c);
  c->read_string(host);
  c->readuint32(max_kids);
}

void
MonStatsMsg::send_to_channel (MsgChannel *c) const
{
  StatsMsg::send_to_channel (c);
  c->write_string(host);
  c->writeuint32(max_kids);
}

void
EnvTransferMsg::fill_from_channel (MsgChannel *c)
{
  EnvTransferMsg::fill_from_channel (c);
  c->read_string(name);
}

void
EnvTransferMsg::send_to_channel (MsgChannel *c) const
{
  EnvTransferMsg::send_to_channel (c);
  c->write_string(name);
}
