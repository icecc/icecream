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


#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <iostream>
#include <assert.h>
#include <minilzo.h>
#include <stdio.h>
#include <sys/utsname.h>

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
MsgChannel::read_a_bit ()
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
      if (eof)
          break;
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
  if (!update_state ())
    error = true;
  return !error;
}

bool
MsgChannel::update_state (void)
{
  switch (instate)
    {
    case NEED_PROTO:
      while (inofs - intogo >= 4)
	{
	  if (protocol == 0)
	    return false;
	  uint32_t remote_prot;
	  unsigned char vers[4];
	  //readuint32 (remote_prot);
	  memcpy(vers, inbuf + intogo, 4);
	  intogo += 4;
	  remote_prot = vers[0];
	  if (protocol == -1)
	    {
	      /* The first time we read the remote protocol.  */
	      protocol = 0;
	      if (remote_prot < MIN_PROTOCOL_VERSION)
		remote_prot = 0;
	      else if (remote_prot > PROTOCOL_VERSION)
		remote_prot = PROTOCOL_VERSION; // ours is smaller
	      if (remote_prot == 0)
		return false;
	      else
		{
		  vers[0] = remote_prot;
		  //writeuint32 (remote_prot);
		  writefull (vers, 4);
		  if (!flush_writebuf (true))
		    return false;
		  protocol = -1 - remote_prot;
		}
	    }
	  else if (protocol < -1)
	    {
	      /* The second time we read the remote protocol.  */
	      protocol = - (protocol + 1);
	      if ((int)remote_prot != protocol)
		{
		  protocol = 0;
		  return false;
		}
	      instate = NEED_LEN;
	      /* Don't consume bytes from messages.  */
	      break;
	    }
	  else
	    trace() << "NEED_PROTO but protocol > 0" << endl;
	}
      /* FALLTHROUGH if the protocol setup was complete (instate was changed
	 to NEED_LEN then).  */
      if (instate != NEED_LEN)
	break;
    case NEED_LEN:
      if (text_based)
        {
          // Skip any leading whitespace
          for (;inofs < intogo; ++inofs)
             if (inbuf[inofs] >= ' ') break;

          // Skip until next newline
          for(inmsglen = 0; inmsglen < inofs - intogo; ++inmsglen)
             if (inbuf[intogo+inmsglen] < ' ')
               {
                 instate = HAS_MSG;
                 break;
               }
          break;
	}
      else if (inofs - intogo >= 4)
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
  return true;
}

void
MsgChannel::chop_input ()
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
MsgChannel::chop_output ()
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
#ifdef MSG_NOSIGNAL
      ssize_t ret = send (fd, buf, msgtogo, MSG_NOSIGNAL);
#else
      void (*oldsigpipe)(int);

      oldsigpipe = signal (SIGPIPE, SIG_IGN);
      ssize_t ret = send (fd, buf, msgtogo, 0);
      signal (SIGPIPE, oldsigpipe);
#endif
      if (ret < 0)
        {
	  if (errno == EINTR)
	    continue;
	  /* If we want to write blocking, but couldn't write anything,
	     select on the fd.  */
	  if (blocking && errno == EAGAIN)
	    {
              int ready;
              for(;;)
                {
                  fd_set write_set;
                  FD_ZERO (&write_set);
                  FD_SET (fd, &write_set);
                  struct timeval tv;
                  tv.tv_sec = 20;
                  tv.tv_usec = 0;
                  ready = select (fd + 1, NULL, &write_set, NULL, &tv);
                  if ( ready < 0 && errno == EINTR)
                    continue;
                  break;
                }
              /* socket ready now for writing ? */
              if (ready > 0)
	        continue;
	      /* Timeout or real error --> error.  */
	    }
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
      if ( ptrdiff_t(inbuf + intogo) % 4 ) {
        char t_buf[4];
        memcpy(t_buf, inbuf + intogo, 4);
        buf = *(uint32_t *)t_buf;
      } else
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
MsgChannel::write_environments( const Environments &envs )
{
  writeuint32( envs.size() );
  for ( Environments::const_iterator it = envs.begin(); it != envs.end(); ++it )
    {
      write_string( it->first );
      write_string( it->second );
    }
}

void
MsgChannel::read_environments( Environments &envs )
{
  envs.clear();
  uint32_t count;
  readuint32( count );
  for ( unsigned int i = 0; i < count; i++ )
    {
      string plat;
      string vers;
      read_string( plat );
      read_string( vers );
      envs.push_back( make_pair( plat, vers ) );
    }
}

void
MsgChannel::readcompressed (unsigned char **uncompressed_buf,
			    size_t &_uclen, size_t &_clen)
{
  lzo_uint uncompressed_len;
  lzo_uint compressed_len;
  uint32_t tmp;
  readuint32 (tmp);
  uncompressed_len = tmp;
  readuint32 (tmp);
  compressed_len = tmp;
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
          log_error() << "internal error - decompression of data from " << dump().c_str() 
              << " failed: " << ret << endl;
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
      log_error() << "internal error - compression failed: " << ret << endl;
      out_len = 0;
    }
  uint32_t _olen = htonl (out_len);
  memcpy (msgbuf + msgtogo_old, &_olen, 4);
  msgtogo += out_len;
  _out_len = out_len;
}

void
MsgChannel::read_line (string &line)
{
  /* XXX handle DOS and MAC line endings and null bytes as string endings.  */
  if (!text_based || inofs < intogo)
    {
     log_error() << "urgs" << endl;
      line = "";
    }
  else
    {
	  line = string(inbuf + intogo, inmsglen);
          log_error() << "inmsglen: " << inmsglen << endl;
          log_error() << "string : *" << line << "*" << endl;
	  intogo += inmsglen;
          while (intogo < inofs && inbuf[intogo] < ' ')
            intogo++;
    }
}

void
MsgChannel::write_line (const string &line)
{
  size_t len = line.length();
  writefull (line.c_str(), len);
  if (line[len-1] != '\n')
    {
      char c = '\n';
      writefull (&c, 1);
    }
}

static bool connect_async( int remote_fd, struct sockaddr *remote_addr, size_t remote_size, int timeout  )
{
  fcntl(remote_fd, F_SETFL, O_NONBLOCK);

  // code majorly derived from lynx's http connect (GPL)
  int status = connect (remote_fd, remote_addr, remote_size );
  if ( ( status < 0 ) && ( errno == EINPROGRESS || errno == EAGAIN ) )
    {
      struct timeval select_timeout;
      fd_set writefds;

      /* we select for a specific time and if that succeeds, we connect one
         final time. Everything else we ignore */

      select_timeout.tv_sec = timeout;
      select_timeout.tv_usec = 0;
      FD_ZERO(&writefds);
      FD_SET(remote_fd, &writefds);
      int ret = select(remote_fd + 1, NULL, &writefds, NULL, &select_timeout);

      /*
      **  If we suspend, then it is possible that select will be
      **  interrupted.  Allow for this possibility. - JED
      */
      if ((ret == -1) && (errno == EINTR))
        return connect_async( remote_fd, remote_addr, remote_size, timeout );

      if (ret > 0)
        {
          /*
          **  Extra check here for connection success, if we try to
          **  connect again, and get EISCONN, it means we have a
          **  successful connection.  But don't check with SOCKS.
          */
          status = connect(remote_fd, remote_addr, remote_size );
          if ((status < 0) && (errno == EISCONN))
            {
              status = 0;
            }
        }
    }

  if (status < 0)
    {
      /*
      **  The connect attempt failed or was interrupted,
      **  so close up the socket.
      */
      close(remote_fd);
      return false;
    }
  else {
    /*
    **  Make the socket blocking again on good connect.
    */
    fcntl(remote_fd, F_SETFL, 0);
  }
  return true;
}

MsgChannel *Service::createChannel (const string &hostname, unsigned short p, int timeout)
{
  int remote_fd;
  int i = 1;
  struct sockaddr_in remote_addr;

  if ((remote_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      log_perror("socket()");
      return 0;
    }
  struct hostent *host = gethostbyname (hostname.c_str());
  if (!host)
    {
      log_perror("Unknown host");
      close (remote_fd);
      return 0;
    }
  if (host->h_length != 4)
    {
      log_error() << "Invalid address length" << endl;
      close (remote_fd);
      return 0;
    }

  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons (p);
  memcpy (&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
  setsockopt (remote_fd, IPPROTO_TCP, TCP_NODELAY, (char*) &i, sizeof(i));

  if ( timeout )
    {
      if ( !connect_async( remote_fd, (struct sockaddr *) &remote_addr, sizeof( remote_addr ), timeout ) )
        return 0; // remote_fd is already closed
    }
  else
    {
      i = 2048;
      setsockopt(remote_fd, SOL_SOCKET, SO_SNDBUF, &i, sizeof(i));
      if (connect (remote_fd, (struct sockaddr *) &remote_addr, sizeof (remote_addr)) < 0)
        {
          close( remote_fd );
	  trace() << "connect failed\n";
          return 0;
        }
    }
  MsgChannel * c = new MsgChannel( remote_fd, (struct sockaddr *)&remote_addr, sizeof( remote_addr ) );
  c->port = p;
  if (!c->wait_for_protocol ())
    {
      delete c;
      c = 0;
      trace() << "not the same protocol\n";
    }
  return c;
}

bool
MsgChannel::eq_ip (const MsgChannel &s)
{
  struct sockaddr_in *s1, *s2;
  s1 = (struct sockaddr_in *) addr;
  s2 = (struct sockaddr_in *) s.addr;
  return (len == s.len
          && memcmp (&s1->sin_addr, &s2->sin_addr, sizeof (s1->sin_addr)) == 0);
}

MsgChannel *Service::createChannel (int fd, struct sockaddr *_a, socklen_t _l)
{
  MsgChannel * c = new MsgChannel( fd, _a, _l, false );
  if (!c->wait_for_protocol ())
    {
      delete c;
      c = 0;
    }
  return c;
}

MsgChannel::MsgChannel (int _fd, struct sockaddr *_a, socklen_t _l, bool text)
  : fd(_fd)
{
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

  // not using new/delete because of the need of realloc()
  msgbuf = (char *) malloc (128);
  msgbuflen = 128;
  msgofs = 0;
  msgtogo = 0;
  inbuf = (char *) malloc (128);
  inbuflen = 128;
  inofs = 0;
  intogo = 0;
  eof = false;
  text_based = text;

  if (fcntl (fd, F_SETFL, O_NONBLOCK) < 0)
    log_perror("MsgChannel fcntl()");

  if (fcntl (fd, F_SETFD, FD_CLOEXEC) < 0)
    log_perror("MsgChannel fcntl() 2");

  if (text_based)
    {
      instate = NEED_LEN;
      protocol = PROTOCOL_VERSION;
    }
  else
    {
      instate = NEED_PROTO;
      protocol = -1;
      unsigned char vers[4] = {PROTOCOL_VERSION, 0, 0, 0};
      //writeuint32 ((uint32_t) PROTOCOL_VERSION);
      writefull (vers, 4);
      if (!flush_writebuf (true))
	protocol = 0; // unusable
    }

  last_talk = time (0);
}

MsgChannel::~MsgChannel()
{
  if (fd >= 0)
    close (fd);
  fd = -1;
  if (msgbuf)
    free (msgbuf);
  if (inbuf)
    free (inbuf);
  if (addr)
    free (addr);
}

string MsgChannel::dump() const
{
  return name + ":" + toString( port ) + " (" +
    char((int)instate+'A') + " eof: " + char(eof +'0') + ")";
}

/* Wait blocking until the protocol setup for this channel is complete.
   Returns false if an error occured.  */
bool
MsgChannel::wait_for_protocol ()
{
  /* protocol is 0 if we couldn't send our initial protocol version.  */
  if (protocol == 0)
    return false;
  while (instate == NEED_PROTO)
    {
      fd_set set;
      FD_ZERO (&set);
      FD_SET (fd, &set);
      struct timeval tv;
      tv.tv_sec = 5;
      tv.tv_usec = 0;
      int ret = select (fd + 1, &set, NULL, NULL, &tv);
      if (ret < 0 && errno == EINTR)
        continue;
      if (ret == 0)
        return false; /* timeout. Consider it a fatal error. */
      if (ret < 0)
        {
          log_perror("select in wait_for_protocol()");
          return false;
        }
      if (!read_a_bit () || eof)
        return false;
    }
  return true;
}

/* This waits indefinitely (well, TIMEOUT seconds) for a complete
   message to arrive.  Returns false if there was some error.  */
bool
MsgChannel::wait_for_msg (int timeout)
{
  if (has_msg ())
    return true;
  if (!read_a_bit () || timeout <= 0)
    {
      trace() << "!read_a_bit || timeout <= 0\n";
      return false;
    }
  while (!has_msg ())
    {
      fd_set read_set;
      FD_ZERO (&read_set);
      FD_SET (fd, &read_set);
      struct timeval tv;
      tv.tv_sec = timeout;
      tv.tv_usec = 0;
      if (select (fd + 1, &read_set, NULL, NULL, &tv) <= 0) {
        if ( errno == EINTR )
          continue;
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
MsgChannel::get_msg(int timeout)
{
  Msg *m = 0;
  enum MsgType type;
  uint32_t t;
  if (timeout > 0 && !wait_for_msg (timeout) ) {
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
  }
  if (text_based)
    type = M_TEXT;
  else
    {
      readuint32 (t);
      type = (enum MsgType) t;
    }
  switch (type)
    {
    case M_UNKNOWN: return 0;
    case M_PING: m = new PingMsg; break;
    case M_END:  m = new EndMsg; break;
    case M_GET_CS: m = new GetCSMsg; break;
    case M_USE_CS: m = new UseCSMsg; break;
    case M_COMPILE_FILE: m = new CompileFileMsg (new CompileJob, true); break;
    case M_FILE_CHUNK: m = new FileChunkMsg; break;
    case M_COMPILE_RESULT: m = new CompileResultMsg; break;
    case M_JOB_BEGIN: m = new JobBeginMsg; break;
    case M_JOB_DONE: m = new JobDoneMsg; break;
    case M_LOGIN: m = new LoginMsg; break;
    case M_STATS: m = new StatsMsg; break;
    case M_GET_NATIVE_ENV: m = new GetNativeEnvMsg; break;
    case M_NATIVE_ENV: m = new UseNativeEnvMsg; break;
    case M_MON_LOGIN: m = new MonLoginMsg; break;
    case M_MON_GET_CS: m = new MonGetCSMsg; break;
    case M_MON_JOB_BEGIN: m = new MonJobBeginMsg; break;
    case M_MON_JOB_DONE: m = new MonJobDoneMsg; break;
    case M_MON_STATS: m = new MonStatsMsg; break;
    case M_JOB_LOCAL_BEGIN: m = new JobLocalBeginMsg; break;
    case M_JOB_LOCAL_DONE : m = new JobLocalDoneMsg; break;
    case M_MON_LOCAL_JOB_BEGIN: m = new MonLocalJobBeginMsg; break;
    case M_TRANFER_ENV: m = new EnvTransferMsg; break;
    case M_TEXT: m = new TextMsg; break;
    case M_GET_INTERNALS: m = new GetInternalStatus; break;
    case M_STATUS_TEXT: m = new StatusTextMsg; break;
    case M_CS_CONF: m = new ConfCSMsg; break;
    case M_TIMEOUT: break;
    }
  if (!m) {
      trace() << "no message type" << endl;
    return 0;
  }
  m->fill_from_channel (this);
  instate = NEED_LEN;
  update_state ();
  return m;
}

bool
MsgChannel::send_msg (const Msg &m, bool blocking)
{
  if (instate == NEED_PROTO
      && !wait_for_protocol ())
    return false;
  chop_output ();
  size_t msgtogo_old = msgtogo;
  if (text_based)
    {
      m.send_to_channel (this);
    }
  else
    {
      writeuint32 (0);  // filled out later with the overall len
      m.send_to_channel (this);
      uint32_t len = htonl (msgtogo - msgtogo_old - 4);
      memcpy (msgbuf + msgtogo_old, &len, 4);
    }
  return flush_writebuf (blocking);
}

#include "getifaddrs.h"
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
      log_perror("open_send_broadcast socket");
      return -1;
    }

  int optval = 1;
  if (setsockopt (ask_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0)
    {
      log_perror("open_send_broadcast setsockopt");
      close (ask_fd);
      return -1;
    }

  struct kde_ifaddrs *addrs;
  int ret = kde_getifaddrs(&addrs);

  if ( ret < 0 )
    return ret;

  char buf = PROTOCOL_VERSION;
  for (struct kde_ifaddrs *addr = addrs; addr != NULL; addr = addr->ifa_next)
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
	      log_perror("open_send_broadcast sendto");
	    }
	}
    }
  kde_freeifaddrs (addrs);
  return ask_fd;
}

#define BROAD_BUFLEN 16

static bool
get_broad_answer (int ask_fd, int timeout, char *buf2,
		  struct sockaddr_in *remote_addr,
		  socklen_t *remote_len)
{
  char buf = PROTOCOL_VERSION;
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
	log_perror("waiting for scheduler");
      return false;
    }
  *remote_len = sizeof (struct sockaddr_in);
  if (recvfrom (ask_fd, buf2, BROAD_BUFLEN, 0, (struct sockaddr*) remote_addr,
		remote_len) != BROAD_BUFLEN)
    {
      log_perror("get_broad_answer recvfrom()");
      return false;
    }
  if (buf + 1 != buf2[0])
    {
      log_error() << "wrong answer" << endl;
      return false;
    }
  buf2[BROAD_BUFLEN - 1] = 0;
  return true;
}

DiscoverSched::DiscoverSched (const std::string &_netname,
			      int _timeout,
			      const std::string &_schedname)
  : netname(_netname), schedname(_schedname), timeout(_timeout), ask_fd(-1),
    sport(8765)
{
  time0 = time (0);
  if (schedname.empty())
    {
      const char *get = getenv( "USE_SCHEDULER" );
      if (get)
	schedname = get;
    }

  if (netname.empty())
    netname = "ICECREAM";

  if (!schedname.empty())
    netname = ""; // take whatever the machine is giving us
  else
    ask_fd = open_send_broadcast ();
}

DiscoverSched::~DiscoverSched ()
{
  if (ask_fd >= 0)
    close (ask_fd);
}

bool
DiscoverSched::timed_out ()
{
  return (time (0) - time0 >= (timeout / 1000));
}

MsgChannel *
DiscoverSched::try_get_scheduler ()
{
  if (schedname.empty())
    {
      struct sockaddr_in remote_addr;
      socklen_t remote_len;
      bool found = false;
      char buf2[BROAD_BUFLEN];

      /* Read/test all packages arrived until now.  */
      while (!found
	     && get_broad_answer (ask_fd, 0/*timeout*/, buf2,
                                  &remote_addr, &remote_len))
        if (strcasecmp (netname.c_str(), buf2 + 1) == 0)
          found = true;
      if (!found)
        return 0;
      schedname = inet_ntoa (remote_addr.sin_addr);
      sport = ntohs (remote_addr.sin_port);
      netname = buf2 + 1;
    }
  log_info() << "scheduler is on " << schedname << ":" << sport << " (net " << netname << ")\n";
  return Service::createChannel( schedname, sport, 0/*timeout*/);
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
  if (c->is_text_based())
    return;
  c->writeuint32 ((uint32_t) type);
}

void
GetCSMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->read_environments( versions );
  c->read_string (filename);
  uint32_t _lang;
  c->readuint32 (_lang);
  c->readuint32( count );
  c->read_string( target );
  lang = static_cast<CompileJob::Language>( _lang );
  c->readuint32( arg_flags );
  c->readuint32( client_id );
  preferred_host = string();
  if (IS_PROTOCOL_22(c))
    c->read_string( preferred_host );
}

void
GetCSMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->write_environments( versions );
  c->write_string (filename);
  c->writeuint32 ((uint32_t) lang);
  c->writeuint32( count );
  c->write_string( target );
  c->writeuint32( arg_flags );
  c->writeuint32( client_id );
  if (IS_PROTOCOL_22(c))
    c->write_string( preferred_host);
}

void
UseCSMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->readuint32 (port);
  c->read_string (hostname);
  c->read_string (host_platform);
  c->readuint32( got_env );
  c->readuint32( client_id );
}

void
UseCSMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 (port);
  c->write_string (hostname);
  c->write_string (host_platform);
  c->writeuint32( got_env );
  c->writeuint32( client_id );
}

void
CompileFileMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  uint32_t id, lang;
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

  string target;
  c->read_string( target );
  job->setTargetPlatform( target );
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
  c->write_string( job->targetPlatform() );
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
  uint32_t was = 0;
  c->readuint32( was );
  was_out_of_memory = was;
}

void
CompileResultMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->write_string (err);
  c->write_string (out);
  c->writeuint32 (status);
  c->writeuint32( was_out_of_memory ? 1 : 0 );
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
  c->read_string( outfile );
  c->readuint32(id);
}

void JobLocalBeginMsg::send_to_channel( MsgChannel *c ) const
{
  Msg::send_to_channel( c );
  c->writeuint32(stime);
  c->write_string( outfile );
  c->writeuint32(id);
}

void JobLocalDoneMsg::fill_from_channel( MsgChannel *c )
{
  Msg::fill_from_channel(c);
  c->readuint32(job_id);
}

void JobLocalDoneMsg::send_to_channel( MsgChannel *c ) const
{
  Msg::send_to_channel( c );
  c->writeuint32(job_id);
}

JobDoneMsg::JobDoneMsg (int id, int exit, unsigned int _flags)
  : Msg(M_JOB_DONE),  exitcode( exit ), flags (_flags), job_id( id )
{
  real_msec = 0;
  user_msec = 0;
  sys_msec = 0;
  pfaults = 0;
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
  c->readuint32 (pfaults);
  c->readuint32 (in_compressed);
  c->readuint32 (in_uncompressed);
  c->readuint32 (out_compressed);
  c->readuint32 (out_uncompressed);
  c->readuint32 (flags);
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
  c->writeuint32 (pfaults);
  c->writeuint32 (in_compressed);
  c->writeuint32 (in_uncompressed);
  c->writeuint32 (out_compressed);
  c->writeuint32 (out_uncompressed);
  c->writeuint32 (flags);
}

LoginMsg::LoginMsg(unsigned int myport, const std::string &_nodename, const std::string _host_platform)
  : Msg(M_LOGIN), port( myport ), noremote( false ), chroot_possible( false ), nodename( _nodename ), host_platform( _host_platform )
{
  // check if we're root
  chroot_possible = ( geteuid() == 0 );
}

void
LoginMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (port);
  c->readuint32 (max_kids);
  c->read_environments( envs );
  c->read_string( nodename );
  c->read_string( host_platform );
  uint32_t net_chroot_possible = 0;
  c->readuint32 (net_chroot_possible);
  chroot_possible = net_chroot_possible != 0;
  uint32_t net_noremote = 0;
  if (IS_PROTOCOL_26( c )) c->readuint32 (net_noremote);
  noremote = (net_noremote != 0);
}

void
LoginMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (port);
  c->writeuint32 (max_kids);
  c->write_environments( envs );
  c->write_string( nodename );
  c->write_string( host_platform );
  c->writeuint32( chroot_possible );
  if (IS_PROTOCOL_26( c )) c->writeuint32( noremote );
}

void
ConfCSMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (max_scheduler_pong);
  c->readuint32 (max_scheduler_ping);
  c->read_string (bench_source);
}

void
ConfCSMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (max_scheduler_pong);
  c->writeuint32 (max_scheduler_ping);
  c->write_string (bench_source);
}

void
StatsMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (load);
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
  c->writeuint32 (loadAvg1);
  c->writeuint32 (loadAvg5);
  c->writeuint32 (loadAvg10);
  c->writeuint32 (freeMem);
}

void
UseNativeEnvMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->read_string( nativeVersion );
}

void
UseNativeEnvMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->write_string( nativeVersion );
}

void
EnvTransferMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->read_string(name);
  c->read_string( target );
}

void
EnvTransferMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->write_string(name);
  c->write_string( target );
}

void
MonGetCSMsg::fill_from_channel (MsgChannel *c)
{
  GetCSMsg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->readuint32 (clientid);
}

void
MonGetCSMsg::send_to_channel (MsgChannel *c) const
{
  GetCSMsg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 (clientid);
}

void
MonJobBeginMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32 (job_id);
  c->readuint32 (stime);
  c->readuint32 (hostid);
}

void
MonJobBeginMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32 (job_id);
  c->writeuint32 (stime);
  c->writeuint32 (hostid);
}

void MonLocalJobBeginMsg::fill_from_channel (MsgChannel * c)
{
  Msg::fill_from_channel(c);
  c->readuint32 (hostid );
  c->readuint32( job_id );
  c->readuint32( stime );
  c->read_string( file );
}

void MonLocalJobBeginMsg::send_to_channel (MsgChannel * c) const
{
  Msg::send_to_channel(c);
  c->writeuint32( hostid );
  c->writeuint32( job_id );
  c->writeuint32( stime );
  c->write_string( file );
}

void
MonStatsMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel (c);
  c->readuint32( hostid );
  c->read_string( statmsg );
}

void
MonStatsMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel (c);
  c->writeuint32(hostid);
  c->write_string(statmsg);
}

void
TextMsg::fill_from_channel (MsgChannel *c)
{
  c->read_line (text);
}

void
TextMsg::send_to_channel (MsgChannel *c) const
{
  c->write_line (text);
}

void
StatusTextMsg::fill_from_channel (MsgChannel *c)
{
  Msg::fill_from_channel( c );
  c->read_string (text);
}

void
StatusTextMsg::send_to_channel (MsgChannel *c) const
{
  Msg::send_to_channel( c );
  c->write_string (text);
}

/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
