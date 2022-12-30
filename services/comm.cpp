/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>
                  2007 Dirk Mueller <dmueller@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <config.h>

#include <signal.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef HAVE_NETINET_TCP_VAR_H
#include <sys/socketvar.h>
#include <netinet/tcp_var.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <iostream>
#include <assert.h>
#include <lzo/lzo1x.h>
#include <zstd.h>
#include <stdio.h>
#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#endif
#include "getifaddrs.h"
#include <net/if.h>
#include <sys/ioctl.h>

#include "logging.h"
#include "job.h"
#include "comm.h"

using namespace std;

// Prefer least amount of CPU use
#undef ZSTD_CLEVEL_DEFAULT
#define ZSTD_CLEVEL_DEFAULT 1

// old libzstd?
#ifndef ZSTD_COMPRESSBOUND
#define ZSTD_COMPRESSBOUND(n) ZSTD_compressBound(n)
#endif

static int zstd_compression()
{
    const char *level = getenv("ICECC_COMPRESSION");
    if (!level || !*level)
        return ZSTD_CLEVEL_DEFAULT;

    char *endptr;
    int n = strtol(level, &endptr, 0);
    if (*endptr)
        return ZSTD_CLEVEL_DEFAULT;
    return n;
}

/*
 * A generic DoS protection. The biggest messages are of type FileChunk
 * which shouldn't be larger than 100kb. so anything bigger than 10 times
 * of that is definitely fishy, and we must reject it (we're running as root,
 * so be cautious).
 */

#define MAX_MSG_SIZE 1 * 1024 * 1024

/*
 * On a slow and congested network it's possible for a send call to get starved.
 * This will happen especially when trying to send a huge number of bytes over at
 * once. We can avoid this situation to a large extend by sending smaller
 * chunks of data over.
 */
#define MAX_SLOW_WRITE_SIZE 10 * 1024

/* TODO
 * buffered in/output per MsgChannel
    + move read* into MsgChannel, create buffer-fill function
    + add timeouting poll() there, handle it in the different
    + read* functions.
    + write* unbuffered / or per message buffer (flush in send_msg)
 * think about error handling
    + saving errno somewhere (in MsgChannel class)
 * handle unknown messages (implement a UnknownMsg holding the content
    of the whole data packet?)
 */

/* Tries to fill the inbuf completely.  */
bool MsgChannel::read_a_bit()
{
    chop_input();
    size_t count = inbuflen - inofs;

    if (count < 128) {
        inbuflen = (inbuflen + 128 + 127) & ~(size_t) 127;
        inbuf = (char *) realloc(inbuf, inbuflen);
        assert(inbuf); // Probably unrecoverable if realloc fails anyway.
        count = inbuflen - inofs;
    }

    char *buf = inbuf + inofs;
    bool error = false;

    while (count) {
        if (eof) {
            break;
        }

        ssize_t ret = read(fd, buf, count);

        if (ret > 0) {
            count -= ret;
            buf += ret;
        } else if (ret < 0 && errno == EINTR) {
            continue;
        } else if (ret < 0) {
            // EOF or some error
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                error = true;
            }
        } else if (ret == 0) {
            eof = true;
        }

        break;
    }

    inofs = buf - inbuf;

    if (!update_state()) {
        error = true;
    }

    if (error) {
        // Daemons sometimes successfully do accept() but then the connection
        // gets ECONNRESET. Probably a spurious result from accept(), so
        // just be silent about it in this case.
        set_error( instate == NEED_PROTO );
        return false;
    }
    return true;
}

bool MsgChannel::update_state()
{
    switch (instate) {
    case NEED_PROTO:

        while (inofs - intogo >= 4) {
            if (protocol == 0) {
                return false;
            }

            uint32_t remote_prot = 0;
            unsigned char vers[4];
            //readuint32 (remote_prot);
            memcpy(vers, inbuf + intogo, 4);
            intogo += 4;

            for (int i = 0; i < 4; ++i) {
                remote_prot |= vers[i] << (i * 8);
            }

            if (protocol == -1) {
                /* The first time we read the remote protocol.  */
                protocol = 0;

                if (remote_prot < MIN_PROTOCOL_VERSION || remote_prot > (1 << 20)) {
                    remote_prot = 0;
                    set_error();
                    return false;
                }

                maximum_remote_protocol = remote_prot;

                if (remote_prot > PROTOCOL_VERSION) {
                    remote_prot = PROTOCOL_VERSION;    // ours is smaller
                }

                for (int i = 0; i < 4; ++i) {
                    vers[i] = remote_prot >> (i * 8);
                }

                writefull(vers, 4);

                if (!flush_writebuf(true)) {
                    set_error();
                    return false;
                }

                protocol = -1 - remote_prot;
            } else if (protocol < -1) {
                /* The second time we read the remote protocol.  */
                protocol = - (protocol + 1);

                if ((int)remote_prot != protocol) {
                    protocol = 0;
                    set_error();
                    return false;
                }

                instate = NEED_LEN;
                /* Don't consume bytes from messages.  */
                break;
            } else {
                trace() << "NEED_PROTO but protocol > 0" << endl;
                set_error();
                return false;
            }
        }

        /* FALLTHROUGH if the protocol setup was complete (instate was changed
        to NEED_LEN then).  */
        if (instate != NEED_LEN) {
            break;
        }
        // fallthrough
    case NEED_LEN:

        if (text_based) {
            // Skip any leading whitespace
            for (; inofs < intogo; ++inofs)
                if (inbuf[inofs] >= ' ') {
                    break;
                }

            // Skip until next newline
            for (inmsglen = 0; inmsglen < inofs - intogo; ++inmsglen)
                if (inbuf[intogo + inmsglen] < ' ') {
                    instate = HAS_MSG;
                    break;
                }

            break;
        } else if (inofs - intogo >= 4) {
            (*this) >> inmsglen;

            if (inmsglen > MAX_MSG_SIZE) {
                log_error() << "received a too large message (size " << inmsglen << "), ignoring" << endl;
                set_error();
                return false;
            }

            if (inbuflen - intogo < inmsglen) {
                inbuflen = (inmsglen + intogo + 127) & ~(size_t)127;
                inbuf = (char *) realloc(inbuf, inbuflen);
                assert(inbuf); // Probably unrecoverable if realloc fails anyway.
            }

            instate = FILL_BUF;
            /* FALLTHROUGH */
        } else {
            break;
        }
        /* FALLTHROUGH */
    case FILL_BUF:

        if (inofs - intogo >= inmsglen) {
            instate = HAS_MSG;
        }
        /* FALLTHROUGH */
        else {
            break;
        }

    case HAS_MSG:
        /* handled elsewhere */
        break;

    case ERROR:
        return false;
    }

    return true;
}

void MsgChannel::chop_input()
{
    /* Make buffer smaller, if there's much already read in front
       of it, or it is cheap to do.  */
    if (intogo > 8192 || inofs - intogo <= 16) {
        if (inofs - intogo != 0) {
            memmove(inbuf, inbuf + intogo, inofs - intogo);
        }

        inofs -= intogo;
        intogo = 0;
    }
}

void MsgChannel::chop_output()
{
    if (msgofs > 8192 || msgtogo <= 16) {
        if (msgtogo) {
            memmove(msgbuf, msgbuf + msgofs, msgtogo);
        }

        msgofs = 0;
    }
}

void MsgChannel::writefull(const void *_buf, size_t count)
{
    if (msgtogo + count >= msgbuflen) {
        /* Realloc to a multiple of 128.  */
        msgbuflen = (msgtogo + count + 127) & ~(size_t)127;
        msgbuf = (char *) realloc(msgbuf, msgbuflen);
        assert(msgbuf); // Probably unrecoverable if realloc fails anyway.
    }

    memcpy(msgbuf + msgtogo, _buf, count);
    msgtogo += count;
}

static size_t get_max_write_size()
{
    if( const char* icecc_slow_network = getenv( "ICECC_SLOW_NETWORK" ))
        if( icecc_slow_network[ 0 ] == '1' )
            return MAX_SLOW_WRITE_SIZE;
    return MAX_MSG_SIZE;
}

bool MsgChannel::flush_writebuf(bool blocking)
{
    const char *buf = msgbuf + msgofs;
    bool error = false;

    while (msgtogo) {
        int send_errno;
        static size_t max_write_size = get_max_write_size();
#ifdef MSG_NOSIGNAL
        ssize_t ret = send(fd, buf, min( msgtogo, max_write_size ), MSG_NOSIGNAL);
        send_errno = errno;
#else
        void (*oldsigpipe)(int);

        oldsigpipe = signal(SIGPIPE, SIG_IGN);
        ssize_t ret = send(fd, buf, min( msgtogo, max_write_size ), 0);
        send_errno = errno;
        signal(SIGPIPE, oldsigpipe);
#endif

        if (ret < 0) {
            if (send_errno == EINTR) {
                continue;
            }

            /* If we want to write blocking, but couldn't write anything,
               select on the fd.  */
            if (blocking && ( send_errno == EAGAIN || send_errno == ENOTCONN || send_errno == EWOULDBLOCK )) {
                int ready;

                for (;;) {
                    pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = POLLOUT;
                    ready = poll(&pfd, 1, 30 * 1000);

                    if (ready < 0 && errno == EINTR) {
                        continue;
                    }

                    break;
                }

                /* socket ready now for writing ? */
                if (ready > 0) {
                    continue;
                }
                if (ready == 0) {
                    log_error() << "timed out while trying to send data" << endl;
                }

                /* Timeout or real error --> error.  */
            }

            errno = send_errno;
            log_perror("flush_writebuf() failed");
            error = true;
            break;
        } else if (ret == 0) {
            // EOF while writing --> error
            error = true;
            break;
        }

        msgtogo -= ret;
        buf += ret;
    }

    msgofs = buf - msgbuf;
    chop_output();
    if(error) {
        set_error();
        return false;
    }
    return true;
}

MsgChannel &MsgChannel::operator>>(uint32_t &buf)
{
    if (inofs >= intogo + 4) {
        if (ptrdiff_t(inbuf + intogo) % 4) {
            uint32_t t_buf[1];
            memcpy(t_buf, inbuf + intogo, 4);
            buf = t_buf[0];
        } else {
            buf = *(uint32_t *)(inbuf + intogo);
        }

        intogo += 4;
        buf = ntohl(buf);
    } else {
        buf = 0;
    }

    return *this;
}

MsgChannel &MsgChannel::operator<<(uint32_t i)
{
    i = htonl(i);
    writefull(&i, 4);
    return *this;
}

MsgChannel &MsgChannel::operator>>(string &s)
{
    char *buf;
    // len is including the (also saved) 0 Byte
    uint32_t len;
    *this >> len;

    if (!len || len > inofs - intogo) {
        s = "";
    } else {
        buf = inbuf + intogo;
        intogo += len;
        s = buf;
    }

    return *this;
}

MsgChannel &MsgChannel::operator<<(const std::string &s)
{
    uint32_t len = 1 + s.length();
    *this << len;
    writefull(s.c_str(), len);
    return *this;
}

MsgChannel &MsgChannel::operator>>(list<string> &l)
{
    uint32_t len;
    l.clear();
    *this >> len;

    while (len--) {
        string s;
        *this >> s;
        l.push_back(s);

        if (inofs == intogo) {
            break;
        }
    }

    return *this;
}

MsgChannel &MsgChannel::operator<<(const std::list<std::string> &l)
{
    *this << (uint32_t) l.size();

    for (const std::string &s : l) {
        *this << s;
    }

    return *this;
}

void MsgChannel::write_environments(const Environments &envs)
{
    *this << envs.size();

    for (const std::pair<std::string, std::string> &env : envs) {
        *this << env.first;
        *this << env.second;
    }
}

void MsgChannel::read_environments(Environments &envs)
{
    envs.clear();
    uint32_t count;
    *this >> count;

    for (unsigned int i = 0; i < count; i++) {
        string plat;
        string vers;
        *this >> plat;
        *this >> vers;
        envs.push_back(make_pair(plat, vers));
    }
}

void MsgChannel::readcompressed(unsigned char **uncompressed_buf, size_t &_uclen, size_t &_clen)
{
    lzo_uint uncompressed_len;
    lzo_uint compressed_len;
    uint32_t tmp;
    *this >> tmp;
    uncompressed_len = tmp;
    *this >> tmp;
    compressed_len = tmp;

    uint32_t proto = C_LZO;
    if (IS_PROTOCOL_VERSION(40, this)) {
        *this >> proto;
        if (proto != C_LZO && proto != C_ZSTD) {
            log_error() << "Unknown compression protocol " << proto << endl;
            *uncompressed_buf = nullptr;
            _uclen = 0;
            _clen = compressed_len;
            set_error();
            return;
        }
    }

    /* If there was some input, but nothing compressed,
       or lengths are bigger than the whole chunk message
       or we don't have everything to uncompress, there was an error.  */
    if (uncompressed_len > MAX_MSG_SIZE
            || compressed_len > (inofs - intogo)
            || (uncompressed_len && !compressed_len)
            || inofs < intogo + compressed_len) {
        log_error() << "failure in readcompressed() length checking" << endl;
        *uncompressed_buf = nullptr;
        uncompressed_len = 0;
        _uclen = uncompressed_len;
        _clen = compressed_len;
        set_error();
        return;
    }

    *uncompressed_buf = new unsigned char[uncompressed_len];

    if (proto == C_ZSTD && uncompressed_len && compressed_len) {
        const void *compressed_buf = inbuf + intogo;
        size_t ret = ZSTD_decompress(*uncompressed_buf, uncompressed_len,
                                     compressed_buf, compressed_len);
        if (ZSTD_isError(ret)) {
            log_error() << "internal error - decompression of data from " << dump().c_str()
                        << " failed: " << ZSTD_getErrorName(ret) << endl;
            delete[] *uncompressed_buf;
            *uncompressed_buf = nullptr;
            uncompressed_len = 0;
        }
    } else if (proto == C_LZO && uncompressed_len && compressed_len) {
        const lzo_byte *compressed_buf = (lzo_byte *)(inbuf + intogo);
        lzo_voidp wrkmem = (lzo_voidp) malloc(LZO1X_MEM_COMPRESS);
        int ret = lzo1x_decompress(compressed_buf, compressed_len,
                                   *uncompressed_buf, &uncompressed_len, wrkmem);
        free(wrkmem);

        if (ret != LZO_E_OK) {
            /* This should NEVER happen.
            Remove the buffer, and indicate there is nothing in it,
            but don't reset the compressed_len, so our caller know,
            that there actually was something read in.  */
            log_error() << "internal error - decompression of data from " << dump().c_str()
                        << " failed: " << ret << endl;
            delete [] *uncompressed_buf;
            *uncompressed_buf = nullptr;
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

void MsgChannel::writecompressed(const unsigned char *in_buf, size_t _in_len, size_t &_out_len)
{
    uint32_t proto = C_LZO;
    if (IS_PROTOCOL_VERSION(40, this))
        proto = C_ZSTD;

    lzo_uint in_len = _in_len;
    lzo_uint out_len = _out_len;
    if (proto == C_LZO)
        out_len = in_len + in_len / 64 + 16 + 3;
    else if (proto == C_ZSTD)
        out_len = ZSTD_COMPRESSBOUND(in_len);
    *this << in_len;
    size_t msgtogo_old = msgtogo;
    *this << (uint32_t) 0;

    if (IS_PROTOCOL_VERSION(40, this))
        *this << proto;

    if (msgtogo + out_len >= msgbuflen) {
        /* Realloc to a multiple of 128.  */
        msgbuflen = (msgtogo + out_len + 127) & ~(size_t)127;
        msgbuf = (char *) realloc(msgbuf, msgbuflen);
        assert(msgbuf); // Probably unrecoverable if realloc fails anyway.
    }

    if (proto == C_LZO) {
        lzo_byte *out_buf = (lzo_byte *)(msgbuf + msgtogo);
        lzo_voidp wrkmem = (lzo_voidp) malloc(LZO1X_MEM_COMPRESS);
        int ret = lzo1x_1_compress(in_buf, in_len, out_buf, &out_len, wrkmem);
        free(wrkmem);

        if (ret != LZO_E_OK) {
            /* this should NEVER happen */
            log_error() << "internal error - compression failed: " << ret << endl;
            out_len = 0;
        }
    } else if (proto == C_ZSTD) {
        void *out_buf = msgbuf + msgtogo;
        size_t ret = ZSTD_compress(out_buf, out_len, in_buf, in_len, zstd_compression());
        if (ZSTD_isError(ret)) {
            /* this should NEVER happen */
            log_error() << "internal error - compression failed: " << ZSTD_getErrorName(ret) << endl;
            out_len = 0;
        }

        out_len = ret;
    }

    uint32_t _olen = htonl(out_len);
    if(out_len > MAX_MSG_SIZE) {
        log_error() << "internal error - size of compressed message to write exceeds max size:" << out_len << endl;
    }
    memcpy(msgbuf + msgtogo_old, &_olen, 4);
    msgtogo += out_len;
    _out_len = out_len;
}

void MsgChannel::read_line(string &line)
{
    /* XXX handle DOS and MAC line endings and null bytes as string endings.  */
    if (!text_based || inofs < intogo) {
        line = "";
    } else {
        line = string(inbuf + intogo, inmsglen);
        intogo += inmsglen;

        while (intogo < inofs && inbuf[intogo] < ' ') {
            intogo++;
        }
    }
}

void MsgChannel::write_line(const string &line)
{
    size_t len = line.length();
    writefull(line.c_str(), len);

    if (line[len - 1] != '\n') {
        char c = '\n';
        writefull(&c, 1);
    }
}

void MsgChannel::set_error(bool silent)
{
    if( instate == ERROR ) {
        return;
    }
    if( !silent && !set_error_recursion ) {
        trace() << "setting error state for channel " << dump() << endl;
        // After the state is set to error, get_msg() will not return anything anymore,
        // so try to fetch last status from the other side, if available.
        set_error_recursion = true;
        Msg* msg = get_msg( 2, true );
        if (msg && *msg == Msg::STATUS_TEXT) {
            log_error() << "remote status: "
                << static_cast<StatusTextMsg*>(msg)->text << endl;
        }
        set_error_recursion = false;
    }
    instate = ERROR;
    eof = true;
}

static int prepare_connect(const string &hostname, unsigned short p,
                           struct sockaddr_in &remote_addr)
{
    int remote_fd;
    int i = 1;

    if ((remote_fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("socket()");
        return -1;
    }

    struct hostent *host = gethostbyname(hostname.c_str());

    if (!host) {
        log_error() << "Connecting to " << hostname << " failed: " << hstrerror( h_errno ) << endl;
        if ((-1 == close(remote_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
        return -1;
    }

    if (host->h_length != 4) {
        log_error() << "Invalid address length" << endl;
        if ((-1 == close(remote_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
        return -1;
    }

    setsockopt(remote_fd, IPPROTO_TCP, TCP_NODELAY, (char *) &i, sizeof(i));

    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(p);
    memcpy(&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);

    return remote_fd;
}

static bool connect_async(int remote_fd, struct sockaddr *remote_addr, size_t remote_size,
                          int timeout)
{
    fcntl(remote_fd, F_SETFL, O_NONBLOCK);

    // code majorly derived from lynx's http connect (GPL)
    int status = connect(remote_fd, remote_addr, remote_size);

    if ((status < 0) && (errno == EINPROGRESS || errno == EAGAIN)) {
        pollfd pfd;
        pfd.fd = remote_fd;
        pfd.events = POLLOUT;
        int ret;

        do {
            /* we poll for a specific time and if that succeeds, we connect one
               final time. Everything else we ignore */
            ret = poll(&pfd, 1, timeout * 1000);

            if (ret < 0 && errno == EINTR) {
                continue;
            }

            break;
        } while (1);

        if (ret > 0) {
            /*
            **  Extra check here for connection success, if we try to
            **  connect again, and get EISCONN, it means we have a
            **  successful connection.  But don't check with SOCKS.
            */
            status = connect(remote_fd, remote_addr, remote_size);

            if ((status < 0) && (errno == EISCONN)) {
                status = 0;
            }
        }
    }

    if (status < 0) {
        /*
        **  The connect attempt failed or was interrupted,
        **  so close up the socket.
        */
        if ((-1 == close(remote_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
        return false;
    } else {
        /*
        **  Make the socket blocking again on good connect.
        */
        fcntl(remote_fd, F_SETFL, 0);
    }

    return true;
}

MsgChannel *Service::createChannel(const string &hostname, unsigned short p, int timeout)
{
    int remote_fd;
    struct sockaddr_in remote_addr;

    if ((remote_fd = prepare_connect(hostname, p, remote_addr)) < 0) {
        return nullptr;
    }

    if (timeout) {
        if (!connect_async(remote_fd, (struct sockaddr *) &remote_addr, sizeof(remote_addr), timeout)) {
            return nullptr;    // remote_fd is already closed
        }
    } else {
        int i = 2048;
        setsockopt(remote_fd, SOL_SOCKET, SO_SNDBUF, &i, sizeof(i));

        if (connect(remote_fd, (struct sockaddr *) &remote_addr, sizeof(remote_addr)) < 0) {
            log_perror_trace("connect");
            trace() << "connect failed on " << hostname << endl;
            if (-1 == close(remote_fd) && (errno != EBADF)){
                log_perror("close failed");
            }
            return nullptr;
        }
    }

    trace() << "connected to " << hostname << endl;
    return createChannel(remote_fd, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

MsgChannel *Service::createChannel(const string &socket_path)
{
    int remote_fd;
    struct sockaddr_un remote_addr;

    if ((remote_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        log_perror("socket()");
        return nullptr;
    }

    remote_addr.sun_family = AF_UNIX;
    strncpy(remote_addr.sun_path, socket_path.c_str(), sizeof(remote_addr.sun_path) - 1);
    remote_addr.sun_path[sizeof(remote_addr.sun_path) - 1] = '\0';
    if(socket_path.length() > sizeof(remote_addr.sun_path) - 1) {
        log_error() << "socket_path path too long for sun_path" << endl;
    }

    if (connect(remote_fd, (struct sockaddr *) &remote_addr, sizeof(remote_addr)) < 0) {
        log_perror_trace("connect");
        trace() << "connect failed on " << socket_path << endl;
        if ((-1 == close(remote_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
        return nullptr;
    }

    trace() << "connected to " << socket_path << endl;
    return createChannel(remote_fd, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

static std::string shorten_filename(const std::string &str)
{
    std::string::size_type ofs = str.rfind('/');

    for (int i = 2; i--;) {
        if (ofs != string::npos) {
            ofs = str.rfind('/', ofs - 1);
        }
    }

    return str.substr(ofs + 1);
}

bool MsgChannel::eq_ip(const MsgChannel &s) const
{
    struct sockaddr_in *s1, *s2;
    s1 = (struct sockaddr_in *) addr;
    s2 = (struct sockaddr_in *) s.addr;
    return (addr_len == s.addr_len
            && memcmp(&s1->sin_addr, &s2->sin_addr, sizeof(s1->sin_addr)) == 0);
}

MsgChannel *Service::createChannel(int fd, struct sockaddr *_a, socklen_t _l)
{
    MsgChannel *c = new MsgChannel(fd, _a, _l, false);

    if (!c->wait_for_protocol()) {
        delete c;
        c = nullptr;
    }

    return c;
}

MsgChannel::MsgChannel(int _fd, struct sockaddr *_a, socklen_t _l, bool text)
    : fd(_fd)
{
    addr_len = (sizeof(struct sockaddr) > _l) ? sizeof(struct sockaddr) : _l;

    if (addr_len && _a) {
        addr = (struct sockaddr *)malloc(addr_len);
        memcpy(addr, _a, _l);
        if(addr->sa_family == AF_UNIX) {
            name = "local unix domain socket";
        } else {
            char buf[16384] = "";
            if(int error = getnameinfo(addr, _l, buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST))
                log_error() << "getnameinfo(): " << error << endl;
            name = buf;
        }
    } else {
        addr = nullptr;
        name = "";
    }

    // not using new/delete because of the need of realloc()
    msgbuf = (char *) malloc(128);
    msgbuflen = 128;
    msgofs = 0;
    msgtogo = 0;
    inbuf = (char *) malloc(128);
    inbuflen = 128;
    inofs = 0;
    intogo = 0;
    eof = false;
    text_based = text;
    set_error_recursion = false;
    maximum_remote_protocol = -1;

    int on = 1;

    if (!setsockopt(_fd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on))) {
#if defined( TCP_KEEPIDLE ) || defined( TCPCTL_KEEPIDLE )
#if defined( TCP_KEEPIDLE )
        int keepidle = TCP_KEEPIDLE;
#else
        int keepidle = TCPCTL_KEEPIDLE;
#endif

        int sec;
        sec = MAX_SCHEDULER_PING - 3 * MAX_SCHEDULER_PONG;
        setsockopt(_fd, IPPROTO_TCP, keepidle, (char *) &sec, sizeof(sec));
#endif

#if defined( TCP_KEEPINTVL ) || defined( TCPCTL_KEEPINTVL )
#if defined( TCP_KEEPINTVL )
        int keepintvl = TCP_KEEPINTVL;
#else
        int keepintvl = TCPCTL_KEEPINTVL;
#endif

        sec = MAX_SCHEDULER_PONG;
        setsockopt(_fd, IPPROTO_TCP, keepintvl, (char *) &sec, sizeof(sec));
#endif

#ifdef TCP_KEEPCNT
        sec = 3;
        setsockopt(_fd, IPPROTO_TCP, TCP_KEEPCNT, (char *) &sec, sizeof(sec));
#endif
    }

#ifdef TCP_USER_TIMEOUT
    int timeout = 3 * 3 * 1000; // matches the timeout part of keepalive above, in milliseconds
    setsockopt(_fd, IPPROTO_TCP, TCP_USER_TIMEOUT, (char *) &timeout, sizeof(timeout));
#endif

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        log_perror("MsgChannel fcntl()");
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        log_perror("MsgChannel fcntl() 2");
    }

    if (text_based) {
        instate = NEED_LEN;
        protocol = PROTOCOL_VERSION;
    } else {
        instate = NEED_PROTO;
        protocol = -1;
        unsigned char vers[4] = {PROTOCOL_VERSION, 0, 0, 0};
        //writeuint32 ((uint32_t) PROTOCOL_VERSION);
        writefull(vers, 4);

        if (!flush_writebuf(true)) {
            protocol = 0;    // unusable
            set_error();
        }
    }

    last_talk = time(nullptr);
}

MsgChannel::~MsgChannel()
{
    if (fd >= 0) {
        if ((-1 == close(fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }

    fd = -1;

    if (msgbuf) {
        free(msgbuf);
    }

    if (inbuf) {
        free(inbuf);
    }

    if (addr) {
        free(addr);
    }
}

string MsgChannel::dump() const
{
    return name + ": (" + char((int)instate + 'A') + " eof: " + char(eof + '0') + ")";
}

/* Wait blocking until the protocol setup for this channel is complete.
   Returns false if an error occurred.  */
bool MsgChannel::wait_for_protocol()
{
    /* protocol is 0 if we couldn't send our initial protocol version.  */
    if (protocol == 0 || instate == ERROR) {
        return false;
    }

    while (instate == NEED_PROTO) {
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 15 * 1000); // 15s

        if (ret < 0 && errno == EINTR) {
            continue;
        }

        if (ret == 0) {
            log_warning() << "no response within timeout" << endl;
            set_error();
            return false; /* timeout. Consider it a fatal error. */
        }

        if (ret < 0) {
            log_perror("select in wait_for_protocol()");
            set_error();
            return false;
        }

        if (!read_a_bit() || eof) {
            return false;
        }
    }

    return true;
}

void MsgChannel::setBulkTransfer()
{
    if (fd < 0) {
        return;
    }

    int i = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &i, sizeof(i));

    // would be nice but not portable across non-linux
#ifdef __linux__
    i = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, (char *) &i, sizeof(i));
#endif
    i = 65536;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &i, sizeof(i));
}

/* This waits indefinitely (well, TIMEOUT seconds) for a complete
   message to arrive.  Returns false if there was some error.  */
bool MsgChannel::wait_for_msg(int timeout)
{
    if (instate == ERROR) {
        return false;
    }

    if (has_msg()) {
        return true;
    }

    if (!read_a_bit()) {
        trace() << "!read_a_bit\n";
        set_error();
        return false;
    }

    if (timeout <= 0) {
        // trace() << "timeout <= 0\n";
        return has_msg();
    }

    while (!has_msg()) {
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, timeout * 1000) <= 0) {
            if (errno == EINTR) {
                continue;
            }

            /* Either timeout or real error.  For this function also
               a timeout is an error.  */
            return false;
        }

        if (!read_a_bit()) {
            trace() << "!read_a_bit 2\n";
            set_error();
            return false;
        }
    }

    return true;
}

Msg *MsgChannel::get_msg(int timeout, bool eofAllowed)
{
    Msg *m = nullptr;
    Msg::Value type;

    if (!wait_for_msg(timeout)) {
        // trace() << "!wait_for_msg()\n";
        return nullptr;
    }

    /* If we've seen the EOF, and we don't have a complete message,
       then we won't see it anymore.  Return that to the caller.
       Don't use has_msg() here, as it returns true for eof.  */
    if (at_eof()) {
        if (!eofAllowed) {
            trace() << "saw eof without complete msg! " << instate << endl;
            set_error();
        }
        return nullptr;
    }

    if (!has_msg()) {
        trace() << "saw eof without msg! " << eof << " " << instate << endl;
        set_error();
        return nullptr;
    }

    size_t intogo_old = intogo;

    if (text_based) {
        type = Msg::TEXT;
    } else {
        uint32_t t;
        *this >> t;
        type = (Msg::Value) t;
    }

    switch (type) {
    case Msg::UNKNOWN:
        set_error();
        return nullptr;
    case Msg::PING:
        m = new PingMsg;
        break;
    case Msg::END:
        m = new EndMsg;
        break;
    case Msg::GET_CS:
        m = new GetCSMsg;
        break;
    case Msg::USE_CS:
        m = new UseCSMsg;
        break;
    case Msg::NO_CS:
        m = new NoCSMsg;
        break;
    case Msg::COMPILE_FILE:
        m = new CompileFileMsg(new CompileJob, true);
        break;
    case Msg::FILE_CHUNK:
        m = new FileChunkMsg;
        break;
    case Msg::COMPILE_RESULT:
        m = new CompileResultMsg;
        break;
    case Msg::JOB_BEGIN:
        m = new JobBeginMsg;
        break;
    case Msg::JOB_DONE:
        m = new JobDoneMsg;
        break;
    case Msg::LOGIN:
        m = new LoginMsg;
        break;
    case Msg::STATS:
        m = new StatsMsg;
        break;
    case Msg::GET_NATIVE_ENV:
        m = new GetNativeEnvMsg;
        break;
    case Msg::NATIVE_ENV:
        m = new UseNativeEnvMsg;
        break;
    case Msg::MON_LOGIN:
        m = new MonLoginMsg;
        break;
    case Msg::MON_GET_CS:
        m = new MonGetCSMsg;
        break;
    case Msg::MON_JOB_BEGIN:
        m = new MonJobBeginMsg;
        break;
    case Msg::MON_JOB_DONE:
        m = new MonJobDoneMsg;
        break;
    case Msg::MON_STATS:
        m = new MonStatsMsg;
        break;
    case Msg::JOB_LOCAL_BEGIN:
        m = new JobLocalBeginMsg;
        break;
    case Msg::JOB_LOCAL_DONE :
        m = new JobLocalDoneMsg;
        break;
    case Msg::MON_LOCAL_JOB_BEGIN:
        m = new MonLocalJobBeginMsg;
        break;
    case Msg::TRANFER_ENV:
        m = new EnvTransferMsg;
        break;
    case Msg::TEXT:
        m = new TextMsg;
        break;
    case Msg::GET_INTERNALS:
        m = new GetInternalStatus;
        break;
    case Msg::STATUS_TEXT:
        m = new StatusTextMsg;
        break;
    case Msg::CS_CONF:
        m = new ConfCSMsg;
        break;
    case Msg::VERIFY_ENV:
        m = new VerifyEnvMsg;
        break;
    case Msg::VERIFY_ENV_RESULT:
        m = new VerifyEnvResultMsg;
        break;
    case Msg::BLACKLIST_HOST_ENV:
        m = new BlacklistHostEnvMsg;
        break;
    case Msg::TIMEOUT:
        break;
    }

    if (!m) {
        trace() << "no message type" << endl;
        set_error();
        return nullptr;
    }

    m->fill_from_channel(this);

    if (!text_based) {
        if( intogo - intogo_old != inmsglen ) {
            log_error() << "internal error - message (" << m->to_string() << ") not read correctly, message size " << inmsglen
                << " read " << (intogo - intogo_old) << endl;
            delete m;
            set_error();
            return nullptr;
        }
    }

    instate = NEED_LEN;
    update_state();

    return m;
}

bool MsgChannel::send_msg(const Msg &m, int flags)
{
    if (instate == ERROR) {
        return false;
    }
    if (instate == NEED_PROTO && !wait_for_protocol()) {
        return false;
    }

    chop_output();
    size_t msgtogo_old = msgtogo;

    if (text_based) {
        m.send_to_channel(this);
    } else {
        *this << (uint32_t) 0;
        m.send_to_channel(this);
        uint32_t out_len = msgtogo - msgtogo_old - 4;
        if(out_len > MAX_MSG_SIZE) {
            log_error() << "internal error - size of message to write exceeds max size:" << out_len << endl;
            set_error();
            return false;
        }
        uint32_t len = htonl(out_len);
        memcpy(msgbuf + msgtogo_old, &len, 4);
    }

    if ((flags & SendBulkOnly) && msgtogo < 4096) {
        return true;
    }

    return flush_writebuf((flags & SendBlocking));
}

static int get_second_port_for_debug( int port )
{
    // When running tests, we want to check also interactions between 2 schedulers, but
    // when they are both local, they cannot bind to the same port. So make sure to
    // send all broadcasts to both.
    static bool checkedDebug = false;
    static int debugPort1 = 0;
    static int debugPort2 = 0;
    if( !checkedDebug ) {
        checkedDebug = true;
        if( const char* env = getenv( "ICECC_TEST_SCHEDULER_PORTS" )) {
            debugPort1 = atoi( env );
            const char* env2 = strchr( env, ':' );
            if( env2 != nullptr )
                debugPort2 = atoi( env2 + 1 );
        }
    }
    int secondPort = 0;
    if( port == debugPort1 )
        secondPort = debugPort2;
    else if( port == debugPort2 )
        secondPort = debugPort1;
    return secondPort ? secondPort : -1;
}

void Broadcasts::broadcastSchedulerVersion(int scheduler_port, const char* netname, time_t starttime)
{
    // Code for older schedulers than version 38. Has endianness problems, the message size
    // is not BROAD_BUFLEN and the netname is possibly not null-terminated.
    const char length_netname = strlen(netname);
    const int schedbuflen = 5 + sizeof(uint64_t) + length_netname;
    char *buf = new char[ schedbuflen ];
    buf[0] = 'I';
    buf[1] = 'C';
    buf[2] = 'E';
    buf[3] = PROTOCOL_VERSION;
    uint64_t tmp_time = starttime;
    memcpy(buf + 4, &tmp_time, sizeof(uint64_t));
    buf[4 + sizeof(uint64_t)] = length_netname;
    strncpy(buf + 5 + sizeof(uint64_t), netname, length_netname - 1);
    buf[ schedbuflen - 1 ] = '\0';
    broadcastData(scheduler_port, buf, schedbuflen);
    delete[] buf;
    // Latest version.
    buf = new char[ BROAD_BUFLEN ];
    memset(buf, 0, BROAD_BUFLEN );
    buf[0] = 'I';
    buf[1] = 'C';
    buf[2] = 'F'; // one up
    buf[3] = PROTOCOL_VERSION;
    uint32_t tmp_time_low = starttime & 0xffffffffUL;
    uint32_t tmp_time_high = uint64_t(starttime) >> 32;
    tmp_time_low = htonl( tmp_time_low );
    tmp_time_high = htonl( tmp_time_high );
    memcpy(buf + 4, &tmp_time_high, sizeof(uint32_t));
    memcpy(buf + 4 + sizeof(uint32_t), &tmp_time_low, sizeof(uint32_t));
    const int OFFSET = 4 + 2 * sizeof(uint32_t);
    snprintf(buf + OFFSET, BROAD_BUFLEN - OFFSET, "%s", netname);
    buf[BROAD_BUFLEN - 1] = 0;
    broadcastData(scheduler_port, buf, BROAD_BUFLEN);
    delete[] buf;
}

bool Broadcasts::isSchedulerVersion(const char* buf, int buflen)
{
    if( buflen != BROAD_BUFLEN )
        return false;
    // Ignore versions older than 38, they are older than us anyway, so not interesting.
    return buf[0] == 'I' && buf[1] == 'C' && buf[2] == 'F';
}

void Broadcasts::getSchedulerVersionData( const char* buf, int* protocol, time_t* time, string* netname )
{
    assert( isSchedulerVersion( buf, BROAD_BUFLEN ));
    const unsigned char other_scheduler_protocol = buf[3];
    uint32_t tmp_time_low, tmp_time_high;
    memcpy(&tmp_time_high, buf + 4, sizeof(uint32_t));
    memcpy(&tmp_time_low, buf + 4 + sizeof(uint32_t), sizeof(uint32_t));
    tmp_time_low = ntohl( tmp_time_low );
    tmp_time_high = ntohl( tmp_time_high );
    time_t other_time = ( uint64_t( tmp_time_high ) << 32 ) | tmp_time_low;;
    string recv_netname = string(buf + 4 + 2 * sizeof(uint32_t));
    if( protocol != nullptr )
        *protocol = other_scheduler_protocol;
    if( time != nullptr )
        *time = other_time;
    if( netname != nullptr )
        *netname = recv_netname;
}

/* Returns a filedesc. or a negative value for errors.  */
static int open_send_broadcast(int port, const char* buf, int size)
{
    int ask_fd;
    struct sockaddr_in remote_addr;

    if ((ask_fd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        log_perror("open_send_broadcast socket");
        return -1;
    }

    if (fcntl(ask_fd, F_SETFD, FD_CLOEXEC) < 0) {
        log_perror("open_send_broadcast fcntl");
        if (-1 == close(ask_fd)){
            log_perror("close failed");
        }
        return -1;
    }

    int optval = 1;

    if (setsockopt(ask_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0) {
        log_perror("open_send_broadcast setsockopt");
        if (-1 == close(ask_fd)){
            log_perror("close failed");
        }
        return -1;
    }

    struct kde_ifaddrs *addrs;

    int ret = kde_getifaddrs(&addrs);

    if (ret < 0) {
        return ret;
    }

    for (struct kde_ifaddrs *addr = addrs; addr != nullptr; addr = addr->ifa_next) {
        /*
         * See if this interface address is IPv4...
         */

        if (addr->ifa_addr == nullptr || addr->ifa_addr->sa_family != AF_INET
                || addr->ifa_netmask == nullptr || addr->ifa_name == nullptr) {
            continue;
        }

        static bool in_tests = getenv( "ICECC_TESTS" ) != nullptr;
        if (!in_tests) {
            if (ntohl(((struct sockaddr_in *) addr->ifa_addr)->sin_addr.s_addr) == 0x7f000001) {
                trace() << "ignoring localhost " << addr->ifa_name << " for broadcast" << endl;
                continue;
            }

            if ((addr->ifa_flags & IFF_POINTOPOINT) || !(addr->ifa_flags & IFF_BROADCAST)) {
                log_info() << "ignoring tunnels " << addr->ifa_name << " for broadcast" << endl;
                continue;
            }
        } else {
            if (ntohl(((struct sockaddr_in *) addr->ifa_addr)->sin_addr.s_addr) != 0x7f000001) {
                trace() << "ignoring non-localhost " << addr->ifa_name << " for broadcast" << endl;
                continue;
            }
        }

        if (addr->ifa_broadaddr) {
            log_info() << "broadcast "
                       << addr->ifa_name << " "
                       << inet_ntoa(((sockaddr_in *)addr->ifa_broadaddr)->sin_addr)
                       << endl;

            remote_addr.sin_family = AF_INET;
            remote_addr.sin_port = htons(port);
            remote_addr.sin_addr = ((sockaddr_in *)addr->ifa_broadaddr)->sin_addr;

            if (sendto(ask_fd, buf, size, 0, (struct sockaddr *)&remote_addr,
                       sizeof(remote_addr)) != size) {
                log_perror("open_send_broadcast sendto");
            }
        }
    }

    kde_freeifaddrs(addrs);
    return ask_fd;
}

void Broadcasts::broadcastData(int port, const char* buf, int len)
{
    int fd = open_send_broadcast(port, buf, len);
    if (fd >= 0) {
        if ((-1 == close(fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }
    int secondPort = get_second_port_for_debug( port );
    if( secondPort > 0 ) {
        int fd2 = open_send_broadcast(secondPort, buf, len);
        if (fd2 >= 0) {
            if ((-1 == close(fd2)) && (errno != EBADF)){
                log_perror("close failed");
            }
        }
    }
}

DiscoverSched::DiscoverSched(const std::string &_netname, int _timeout,
                             const std::string &_schedname, int port)
    : netname(_netname)
    , schedname(_schedname)
    , timeout(_timeout)
    , ask_fd(-1)
    , ask_second_fd(-1)
    , sport(port)
    , best_version(0)
    , best_start_time(0)
    , best_port(0)
    , multiple(false)
{
    time0 = time(nullptr);

    if (schedname.empty()) {
        const char *get = getenv("ICECC_SCHEDULER");
        if( get == nullptr )
            get = getenv("USE_SCHEDULER");

        if (get) {
            string scheduler = get;
            size_t colon = scheduler.rfind( ':' );
            if( colon == string::npos ) {
                schedname = scheduler;
            } else {
                schedname = scheduler.substr(0, colon);
                sport = atoi( scheduler.substr( colon + 1 ).c_str());
            }
        }
    }

    if (netname.empty()) {
        netname = "ICECREAM";
    }
    if (sport == 0 ) {
        sport = 8765;
    }

    if (!schedname.empty()) {
        netname = ""; // take whatever the machine is giving us
        attempt_scheduler_connect();
    } else {
        sendSchedulerDiscovery( PROTOCOL_VERSION );
    }
}

DiscoverSched::~DiscoverSched()
{
    if (ask_fd >= 0) {
        if ((-1 == close(ask_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }
    if (ask_second_fd >= 0) {
        if ((-1 == close(ask_second_fd)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }
}

bool DiscoverSched::timed_out()
{
    return (time(nullptr) - time0 >= timeout);
}

void DiscoverSched::attempt_scheduler_connect()
{
    time0 = time(nullptr) + MAX_SCHEDULER_PONG;
    log_info() << "scheduler is on " << schedname << ":" << sport << " (net " << netname << ")" << endl;

    if ((ask_fd = prepare_connect(schedname, sport, remote_addr)) >= 0) {
        fcntl(ask_fd, F_SETFL, O_NONBLOCK);
    }
}

void DiscoverSched::sendSchedulerDiscovery( int version )
{
        assert( version < 128 );
        char buf = version;
        ask_fd = open_send_broadcast(sport, &buf, 1);
        int secondPort = get_second_port_for_debug( sport );
        if( secondPort > 0 )
            ask_second_fd = open_send_broadcast(secondPort, &buf, 1);
}

bool DiscoverSched::isSchedulerDiscovery(const char* buf, int buflen, int* daemon_version)
{
    if( buflen != 1 )
        return false;
    if( daemon_version != nullptr ) {
        *daemon_version = buf[ 0 ];
    }
    return true;
}

static const int BROAD_BUFLEN = 268;
static const int BROAD_BUFLEN_OLD_2 = 32;
static const int BROAD_BUFLEN_OLD_1 = 16;

int DiscoverSched::prepareBroadcastReply(char* buf, const char* netname, time_t starttime)
{
    if (buf[0] < 33) { // old client
        buf[0]++;
        memset(buf + 1, 0, BROAD_BUFLEN_OLD_1 - 1);
        snprintf(buf + 1, BROAD_BUFLEN_OLD_1 - 1, "%s", netname);
        buf[BROAD_BUFLEN_OLD_1 - 1] = 0;
        return BROAD_BUFLEN_OLD_1;
    } else if (buf[0] < 36) {
        // This is like 36, but 36 silently changed the size of BROAD_BUFLEN from 32 to 268.
        // Since get_broad_answer() explicitly null-terminates the data, this wouldn't lead
        // to those receivers reading a shorter string that would not be null-terminated,
        // but still, this is what versions 33-35 actually worked with.
        buf[0] += 2;
        memset(buf + 1, 0, BROAD_BUFLEN_OLD_2 - 1);
        uint32_t tmp_version = PROTOCOL_VERSION;
        uint64_t tmp_time = starttime;
        memcpy(buf + 1, &tmp_version, sizeof(uint32_t));
        memcpy(buf + 1 + sizeof(uint32_t), &tmp_time, sizeof(uint64_t));
        const int OFFSET = 1 + sizeof(uint32_t) + sizeof(uint64_t);
        snprintf(buf + OFFSET, BROAD_BUFLEN_OLD_2 - OFFSET, "%s", netname);
        buf[BROAD_BUFLEN_OLD_2 - 1] = 0;
        return BROAD_BUFLEN_OLD_2;
    } else if (buf[0] < 38) { // exposes endianess because of not using htonl()
        buf[0] += 2;
        memset(buf + 1, 0, BROAD_BUFLEN - 1);
        uint32_t tmp_version = PROTOCOL_VERSION;
        uint64_t tmp_time = starttime;
        memcpy(buf + 1, &tmp_version, sizeof(uint32_t));
        memcpy(buf + 1 + sizeof(uint32_t), &tmp_time, sizeof(uint64_t));
        const int OFFSET = 1 + sizeof(uint32_t) + sizeof(uint64_t);
        snprintf(buf + OFFSET, BROAD_BUFLEN - OFFSET, "%s", netname);
        buf[BROAD_BUFLEN - 1] = 0;
        return BROAD_BUFLEN;
    } else { // latest version
        buf[0] += 3;
        memset(buf + 1, 0, BROAD_BUFLEN - 1);
        uint32_t tmp_version = PROTOCOL_VERSION;
        uint32_t tmp_time_low = starttime & 0xffffffffUL;
        uint32_t tmp_time_high = uint64_t(starttime) >> 32;
        tmp_version = htonl( tmp_version );
        tmp_time_low = htonl( tmp_time_low );
        tmp_time_high = htonl( tmp_time_high );
        memcpy(buf + 1, &tmp_version, sizeof(uint32_t));
        memcpy(buf + 1 + sizeof(uint32_t), &tmp_time_high, sizeof(uint32_t));
        memcpy(buf + 1 + 2 * sizeof(uint32_t), &tmp_time_low, sizeof(uint32_t));
        const int OFFSET = 1 + 3 * sizeof(uint32_t);
        snprintf(buf + OFFSET, BROAD_BUFLEN - OFFSET, "%s", netname);
        buf[BROAD_BUFLEN - 1] = 0;
        return BROAD_BUFLEN;
    }
}

void DiscoverSched::get_broad_data(const char* buf, const char** name, int* version, time_t* start_time)
{
    if (buf[0] == PROTOCOL_VERSION + 1) {
        // Scheduler version 32 or older, didn't send us its version, assume it's 32.
        if (name != nullptr)
            *name = buf + 1;
        if (version != nullptr)
            *version = 32;
        if (start_time != nullptr)
            *start_time = 0; // Unknown too.
    } else if(buf[0] == PROTOCOL_VERSION + 2) {
        if (version != nullptr) {
            uint32_t tmp_version;
            memcpy(&tmp_version, buf + 1, sizeof(uint32_t));
            *version = tmp_version;
        }
        if (start_time != nullptr) {
            uint64_t tmp_time;
            memcpy(&tmp_time, buf + 1 + sizeof(uint32_t), sizeof(uint64_t));
            *start_time = tmp_time;
        }
        if (name != nullptr)
            *name = buf + 1 + sizeof(uint32_t) + sizeof(uint64_t);
    } else if(buf[0] == PROTOCOL_VERSION + 3) {
        if (version != nullptr) {
            uint32_t tmp_version;
            memcpy(&tmp_version, buf + 1, sizeof(uint32_t));
            *version = ntohl( tmp_version );
        }
        if (start_time != nullptr) {
            uint32_t tmp_time_low, tmp_time_high;
            memcpy(&tmp_time_high, buf + 1 + sizeof(uint32_t), sizeof(uint32_t));
            memcpy(&tmp_time_low, buf + 1 + 2 * sizeof(uint32_t), sizeof(uint32_t));
            tmp_time_low = ntohl( tmp_time_low );
            tmp_time_high = ntohl( tmp_time_high );
            *start_time = ( uint64_t( tmp_time_high ) << 32 ) | tmp_time_low;;
        }
        if (name != nullptr)
            *name = buf + 1 + 3 * sizeof(uint32_t);
    } else {
        abort();
    }
}

MsgChannel *DiscoverSched::try_get_scheduler()
{
    if (schedname.empty()) {
        socklen_t remote_len;
        char buf2[BROAD_BUFLEN];
        /* Try to get the scheduler with the newest version, and if there
           are several with the same version, choose the one that's been running
           for the longest time. It should work like this (and it won't work
           perfectly if there are schedulers and/or daemons with old (<33) version):

           Whenever a daemon starts, it broadcasts for a scheduler. Schedulers all
           see the broadcast and respond with their version, start time and netname.
           Here we select the best one.
           If a new scheduler is started, it'll broadcast its version and all
           other schedulers will drop their daemon connections if they have an older
           version. If the best scheduler quits, all daemons will get their connections
           closed and will re-discover and re-connect.
        */

        /* Read/test all packages arrived until now.  */
        while (get_broad_answer(ask_fd, 0/*timeout*/, buf2, (struct sockaddr_in *) &remote_addr, &remote_len)
                || ( ask_second_fd != -1 && get_broad_answer(ask_second_fd, 0/*timeout*/, buf2,
                                            (struct sockaddr_in *) &remote_addr, &remote_len))) {
            int version;
            time_t start_time;
            const char* name;
            get_broad_data(buf2, &name, &version, &start_time);
            if (strcasecmp(netname.c_str(), name) == 0) {
                if( version >= 128 || version < 1 ) {
                    log_warning() << "Ignoring bogus version " << version << " from scheduler found at " << inet_ntoa(remote_addr.sin_addr)
                        << ":" << ntohs(remote_addr.sin_port) << endl;
                    continue;
                }
                else if (version < 33) {
                    log_info() << "Suitable scheduler found at " << inet_ntoa(remote_addr.sin_addr)
                        << ":" << ntohs(remote_addr.sin_port) << " (unknown version)" << endl;
                } else {
                    log_info() << "Suitable scheduler found at " << inet_ntoa(remote_addr.sin_addr)
                        << ":" << ntohs(remote_addr.sin_port) << " (version: " << version << ")" << endl;
                }
                if (best_version != 0)
                    multiple = true;
                if (best_version < version || (best_version == version && best_start_time > start_time)) {
                    best_schedname = inet_ntoa(remote_addr.sin_addr);
                    best_port = ntohs(remote_addr.sin_port);
                    best_version = version;
                    best_start_time = start_time;
                }
            } else {
                log_info() << "Ignoring scheduler at " << inet_ntoa(remote_addr.sin_addr)
                    << ":" << ntohs(remote_addr.sin_port) << " because of a different netname ("
                    << name << ")" << endl;
            }
        }

        if (timed_out()) {
            if (best_version == 0) {
                return nullptr;
            }
            schedname = best_schedname;
            sport = best_port;
            if (multiple)
                log_info() << "Selecting scheduler at " << schedname << ":" << sport << endl;

            if (-1 == close(ask_fd)){
                log_perror("close failed");
            }
            ask_fd = -1;
            if( get_second_port_for_debug( sport ) > 0 ) {
                if (-1 == close(ask_second_fd)){
                    log_perror("close failed");
                }
                ask_second_fd = -1;
            } else {
                assert( ask_second_fd == -1 );
            }
            attempt_scheduler_connect();

            if (ask_fd >= 0) {
                int status = connect(ask_fd, (struct sockaddr *) &remote_addr, sizeof(remote_addr));

                if (status == 0 || (status < 0 && (errno == EISCONN || errno == EINPROGRESS))) {
                    int fd = ask_fd;
                    ask_fd = -1;
                    return Service::createChannel(fd,
                                                  (struct sockaddr *) &remote_addr, sizeof(remote_addr));
                }
            }
        }
    }
    else if (ask_fd >= 0) {
        assert( ask_second_fd == -1 );
        int status = connect(ask_fd, (struct sockaddr *) &remote_addr, sizeof(remote_addr));

        if (status == 0 || (status < 0 && errno == EISCONN)) {
            int fd = ask_fd;
            ask_fd = -1;
            return Service::createChannel(fd,
                                          (struct sockaddr *) &remote_addr, sizeof(remote_addr));
        }
    }

    return nullptr;
}

bool DiscoverSched::get_broad_answer(int ask_fd, int timeout, char *buf2, struct sockaddr_in *remote_addr,
                 socklen_t *remote_len)
{
    char buf = PROTOCOL_VERSION;
    pollfd pfd;
    assert(ask_fd > 0);
    pfd.fd = ask_fd;
    pfd.events = POLLIN;
    errno = 0;

    if (poll(&pfd, 1, timeout) <= 0 || (pfd.revents & POLLIN) == 0) {
        /* Normally this is a timeout, i.e. no scheduler there.  */
        if (errno && errno != EINTR) {
            log_perror("waiting for scheduler");
        }

        return false;
    }

    *remote_len = sizeof(struct sockaddr_in);

    int len = recvfrom(ask_fd, buf2, BROAD_BUFLEN, 0, (struct sockaddr *) remote_addr, remote_len);
    if (len != BROAD_BUFLEN && len != BROAD_BUFLEN_OLD_1 && len != BROAD_BUFLEN_OLD_2) {
        log_perror("get_broad_answer recvfrom()");
        return false;
    }

    if (! ((len == BROAD_BUFLEN_OLD_1 && buf2[0] == buf + 1)   // PROTOCOL <= 32 scheduler
          || (len == BROAD_BUFLEN_OLD_2 && buf2[0] == buf + 2) // PROTOCOL >= 33 && < 36 scheduler
          || (len == BROAD_BUFLEN && buf2[0] == buf + 2)       // PROTOCOL >= 36 && < 38 scheduler
          || (len == BROAD_BUFLEN && buf2[0] == buf + 3))) {   // PROTOCOL >= 38 scheduler
        log_error() << "Wrong scheduler discovery answer (size " << len << ", mark " << int(buf2[0]) << ")" << endl;
        return false;
    }

    buf2[len - 1] = 0;
    return true;
}

list<string> DiscoverSched::getNetnames(int timeout, int port)
{
    list<string> l;
    int ask_fd;
    struct sockaddr_in remote_addr;
    socklen_t remote_len;
    time_t time0 = time(nullptr);

    char buf = PROTOCOL_VERSION;
    ask_fd = open_send_broadcast(port, &buf, 1);

    do {
        char buf2[BROAD_BUFLEN];
        bool first = true;
        /* Wait at least two seconds to give all schedulers a chance to answer
           (unless that'd be longer than the timeout).*/
        time_t timeout_time = time(nullptr) + min(2 + 1, timeout);

        /* Read/test all arriving packages.  */
        while (get_broad_answer(ask_fd, first ? timeout : 0, buf2,
                                &remote_addr, &remote_len)
               && time(nullptr) < timeout_time) {
            first = false;
            const char* name;
            get_broad_data(buf2, &name, nullptr, nullptr);
            l.push_back(name);
        }
    } while (time(nullptr) - time0 < (timeout / 1000));

    if ((-1 == close(ask_fd)) && (errno != EBADF)){
        log_perror("close failed");
    }
    return l;
}

list<string> get_netnames(int timeout, int port)
{
    return DiscoverSched::getNetnames(timeout, port);
}

void Msg::fill_from_channel(MsgChannel *)
{
}

void Msg::send_to_channel(MsgChannel *c) const
{
    if (c->is_text_based()) {
        return;
    }

    *c << (uint32_t) *this;
}

GetCSMsg::GetCSMsg(const Environments &envs, const std::string &f,
     CompileJob::Language _lang, unsigned int _count,
     std::string _target, unsigned int _arg_flags,
     const std::string &host, int _minimal_host_version,
     unsigned int _required_features,
     int _niceness,
     unsigned int _client_count)
    : Msg(Msg::GET_CS)
    , versions(envs)
    , filename(f)
    , lang(_lang)
    , count(_count)
    , target(_target)
    , arg_flags(_arg_flags)
    , client_id(0)
    , preferred_host(host)
    , minimal_host_version(_minimal_host_version)
    , required_features(_required_features)
    , client_count(_client_count)
    , niceness(_niceness)
{
    // These have been introduced in protocol version 42.
    if( required_features & ( NODE_FEATURE_ENV_XZ | NODE_FEATURE_ENV_ZSTD ))
        minimal_host_version = max( minimal_host_version, 42 );
    assert( _niceness >= 0 && _niceness <= 20 );
}

void GetCSMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    c->read_environments(versions);
    *c >> filename;
    uint32_t _lang;
    *c >> _lang;
    *c >> count;
    *c >> target;
    lang = static_cast<CompileJob::Language>(_lang);
    *c >> arg_flags;
    *c >> client_id;
    preferred_host = string();

    if (IS_PROTOCOL_VERSION(22, c)) {
        *c >> preferred_host;
    }

    minimal_host_version = 0;
    if (IS_PROTOCOL_VERSION(31, c)) {
        uint32_t ign;
        *c >> ign;
        // Versions 31-33 had this as a separate field, now set a minimal
        // remote version if needed.
        if (ign != 0 && minimal_host_version < 31)
            minimal_host_version = 31;
    }
    if (IS_PROTOCOL_VERSION(34, c)) {
        uint32_t version;
        *c >> version;
        minimal_host_version = max( minimal_host_version, int( version ));
    }

    if (IS_PROTOCOL_VERSION(39, c)) {
        *c >> client_count;
    }

    required_features = 0;
    if (IS_PROTOCOL_VERSION(42, c)) {
        *c >> required_features;
    }

    niceness = 0;
    if (IS_PROTOCOL_VERSION(43, c)) {
        *c >> niceness;
    }
}

void GetCSMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    c->write_environments(versions);
    *c << shorten_filename(filename);
    *c << (uint32_t) lang;
    *c << count;
    *c << target;
    *c << arg_flags;
    *c << client_id;

    if (IS_PROTOCOL_VERSION(22, c)) {
        *c << preferred_host;
    }

    if (IS_PROTOCOL_VERSION(31, c)) {
        *c << uint32_t(minimal_host_version >= 31 ? 1 : 0);
    }
    if (IS_PROTOCOL_VERSION(34, c)) {
        *c << minimal_host_version;
    }

    if (IS_PROTOCOL_VERSION(39, c)) {
        *c << client_count;
    }
    if (IS_PROTOCOL_VERSION(42, c)) {
        *c << required_features;
    }
    if (IS_PROTOCOL_VERSION(43, c)) {
        *c << niceness;
    }
}

void UseCSMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> job_id;
    *c >> port;
    *c >> hostname;
    *c >> host_platform;
    *c >> got_env;
    *c >> client_id;

    if (IS_PROTOCOL_VERSION(28, c)) {
        *c >> matched_job_id;
    } else {
        matched_job_id = 0;
    }
}

void UseCSMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << job_id;
    *c << port;
    *c << hostname;
    *c << host_platform;
    *c << got_env;
    *c << client_id;

    if (IS_PROTOCOL_VERSION(28, c)) {
        *c << matched_job_id;
    }
}

void NoCSMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> job_id;
    *c >> client_id;
}

void NoCSMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << job_id;
    *c << client_id;
}


void CompileFileMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    uint32_t id, lang;
    string version;
    *c >> lang;
    *c >> id;
    ArgumentsList l;
    if( IS_PROTOCOL_VERSION(41, c)) {
        list<string> largs;
        *c >> largs;
        // Whe compiling remotely, we no longer care about the Arg_Remote vs Arg_Rest
        // difference, so treat them all as Arg_Remote.
        for (list<string>::const_iterator it = largs.begin(); it != largs.end(); ++it)
            l.append(*it, Arg_Remote);
    } else {
        list<string> _l1, _l2;
        *c >> _l1;
        *c >> _l2;
        for (list<string>::const_iterator it = _l1.begin(); it != _l1.end(); ++it)
            l.append(*it, Arg_Remote);
        for (list<string>::const_iterator it = _l2.begin(); it != _l2.end(); ++it)
            l.append(*it, Arg_Rest);
    }
    *c >> version;
    job->setLanguage((CompileJob::Language) lang);
    job->setJobID(id);

    job->setFlags(l);
    job->setEnvironmentVersion(version);

    string target;
    *c >> target;
    job->setTargetPlatform(target);

    if (IS_PROTOCOL_VERSION(30, c)) {
        string compilerName;
        *c >> compilerName;
        job->setCompilerName(compilerName);
    }
    if( IS_PROTOCOL_VERSION(34, c)) {
        string inputFile;
        string workingDirectory;
        *c >> inputFile;
        *c >> workingDirectory;
        job->setInputFile(inputFile);
        job->setWorkingDirectory(workingDirectory);
    }
    if (IS_PROTOCOL_VERSION(35, c)) {
        string outputFile;
        uint32_t dwarfFissionEnabled = 0;
        *c >> outputFile;
        *c >> dwarfFissionEnabled;
        job->setOutputFile(outputFile);
        job->setDwarfFissionEnabled(dwarfFissionEnabled);
    }
}

void CompileFileMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << (uint32_t) job->language();
    *c << job->jobID();

    if (IS_PROTOCOL_VERSION(41, c)) {
        // By the time we're compiling, the args are all Arg_Remote or Arg_Rest and
        // we no longer care about the differences, but we may care about the ordering.
        // So keep them all in one list.
        *c << job->nonLocalFlags();
    } else {
        if (IS_PROTOCOL_VERSION(30, c)) {
            *c << job->remoteFlags();
        } else {
            if (job->compilerName().find("clang") != string::npos) {
                // Hack for compilerwrapper.
                std::list<std::string> flags = job->remoteFlags();
                flags.push_front("clang");
                *c << flags;
            } else {
                *c << job->remoteFlags();
            }
        }
        *c << job->restFlags();
    }

    *c << job->environmentVersion();
    *c << job->targetPlatform();

    if (IS_PROTOCOL_VERSION(30, c)) {
        *c << remote_compiler_name();
    }
    if( IS_PROTOCOL_VERSION(34, c)) {
        *c << job->inputFile();
        *c << job->workingDirectory();
    }
    if (IS_PROTOCOL_VERSION(35, c)) {
        *c << job->outputFile();
        *c << (uint32_t) job->dwarfFissionEnabled();
    }
}

// Environments created by icecc-create-env always use the same binary name
// for compilers, so even if local name was e.g. c++, remote needs to
// be g++ (before protocol version 30 remote CS even had /usr/bin/{gcc|g++}
// hardcoded).  For clang, the binary is just clang for both C/C++.
string CompileFileMsg::remote_compiler_name() const
{
    if (job->compilerName().find("clang") != string::npos) {
        return "clang";
    }

    return job->language() == CompileJob::Lang_CXX ? "g++" : "gcc";
}

CompileJob *CompileFileMsg::takeJob()
{
    assert(deleteit);
    deleteit = false;
    return job;
}

void FileChunkMsg::fill_from_channel(MsgChannel *c)
{
    if (del_buf) {
        delete [] buffer;
    }

    buffer = nullptr;
    del_buf = true;

    Msg::fill_from_channel(c);
    c->readcompressed(&buffer, len, compressed);
}

void FileChunkMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    c->writecompressed(buffer, len, compressed);
}

FileChunkMsg::~FileChunkMsg()
{
    if (del_buf) {
        delete [] buffer;
    }
}

void CompileResultMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    uint32_t _status = 0;
    *c >> err;
    *c >> out;
    *c >> _status;
    status = _status;
    uint32_t was = 0;
    *c >> was;
    was_out_of_memory = was;
    if (IS_PROTOCOL_VERSION(35, c)) {
        uint32_t dwo = 0;
        *c >> dwo;
        have_dwo_file = dwo;
    }
}

void CompileResultMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << err;
    *c << out;
    *c << status;
    *c << (uint32_t) was_out_of_memory;
    if (IS_PROTOCOL_VERSION(35, c)) {
        *c << (uint32_t) have_dwo_file;
    }
}

void JobBeginMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> job_id;
    *c >> stime;
    if (IS_PROTOCOL_VERSION(39, c)) {
        *c >> client_count;
    }
}

void JobBeginMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << job_id;
    *c << stime;
    if (IS_PROTOCOL_VERSION(39, c)) {
        *c << client_count;
    }
}

void JobLocalBeginMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> stime;
    *c >> outfile;
    *c >> id;
    if (IS_PROTOCOL_VERSION(44, c)) {
        uint32_t full;
        *c >> full;
        fulljob = full;
    }
}

void JobLocalBeginMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << stime;
    *c << outfile;
    *c << id;
    if (IS_PROTOCOL_VERSION(44, c)) {
        *c << (uint32_t) fulljob;
    }
}

void JobLocalDoneMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> job_id;
}

void JobLocalDoneMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << job_id;
}

JobDoneMsg::JobDoneMsg(int id, int exit, unsigned int _flags, unsigned int _client_count)
    : Msg(Msg::JOB_DONE)
    , exitcode(exit)
    , flags(_flags)
    , job_id(id)
    , client_count(_client_count)
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

void JobDoneMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    uint32_t _exitcode = 255;
    *c >> job_id;
    *c >> _exitcode;
    *c >> real_msec;
    *c >> user_msec;
    *c >> sys_msec;
    *c >> pfaults;
    *c >> in_compressed;
    *c >> in_uncompressed;
    *c >> out_compressed;
    *c >> out_uncompressed;
    *c >> flags;
    exitcode = (int) _exitcode;
    // Older versions used this special exit code to identify
    // EndJob messages for jobs with unknown job id.
    if (!IS_PROTOCOL_VERSION(39, c) && exitcode == 200) {
        flags |= UnknownJobId;
    }
    if (IS_PROTOCOL_VERSION(39, c)) {
        *c >> client_count;
    }
}

void JobDoneMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << job_id;
    if (!IS_PROTOCOL_VERSION(39, c) && (flags & UnknownJobId)) {
        *c << (uint32_t) 200;
    } else {
        *c << (uint32_t) exitcode;
    }
    *c << real_msec;
    *c << user_msec;
    *c << sys_msec;
    *c << pfaults;
    *c << in_compressed;
    *c << in_uncompressed;
    *c << out_compressed;
    *c << out_uncompressed;
    *c << flags;
    if (IS_PROTOCOL_VERSION(39, c)) {
        *c << client_count;
    }
}

void JobDoneMsg::set_unknown_job_client_id( uint32_t clientId )
{
    flags |= UnknownJobId;
    job_id = clientId;
}

uint32_t JobDoneMsg::unknown_job_client_id() const
{
    if( flags & UnknownJobId ) {
        return job_id;
    }
    return 0;
}

void JobDoneMsg::set_job_id( uint32_t jobId )
{
    job_id = jobId;
    flags &= ~ (uint32_t) UnknownJobId;
}

LoginMsg::LoginMsg(unsigned int myport, const std::string &_nodename, const std::string &_host_platform,
    unsigned int myfeatures)
    : Msg(Msg::LOGIN)
    , port(myport)
    , max_kids(0)
    , noremote(false)
    , chroot_possible(false)
    , nodename(_nodename)
    , host_platform(_host_platform)
    , supported_features(myfeatures)
{
#ifdef HAVE_LIBCAP_NG
    chroot_possible = capng_have_capability(CAPNG_EFFECTIVE, CAP_SYS_CHROOT);
#else
    // check if we're root
    chroot_possible = (geteuid() == 0);
#endif
}

void LoginMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> port;
    *c >> max_kids;
    c->read_environments(envs);
    *c >> nodename;
    *c >> host_platform;
    uint32_t net_chroot_possible = 0;
    *c >> net_chroot_possible;
    chroot_possible = net_chroot_possible != 0;
    uint32_t net_noremote = 0;

    if (IS_PROTOCOL_VERSION(26, c)) {
        *c >> net_noremote;
    }

    noremote = (net_noremote != 0);

    supported_features = 0;
    if (IS_PROTOCOL_VERSION(42, c)) {
        *c >> supported_features;
    }
}

void LoginMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << port;
    *c << max_kids;
    c->write_environments(envs);
    *c << nodename;
    *c << host_platform;
    *c << chroot_possible;

    if (IS_PROTOCOL_VERSION(26, c)) {
        *c << noremote;
    }
    if (IS_PROTOCOL_VERSION(42, c)) {
        *c << supported_features;
    }
}

void ConfCSMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> max_scheduler_pong;
    *c >> max_scheduler_ping;
    string bench_source; // unused, kept for backwards compatibility
    *c >> bench_source;
}

void ConfCSMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << max_scheduler_pong;
    *c << max_scheduler_ping;
    string bench_source;
    *c << bench_source;
}

void StatsMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> load;
    *c >> loadAvg1;
    *c >> loadAvg5;
    *c >> loadAvg10;
    *c >> freeMem;
}

void StatsMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << load;
    *c << loadAvg1;
    *c << loadAvg5;
    *c << loadAvg10;
    *c << freeMem;
}

void GetNativeEnvMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);

    if (IS_PROTOCOL_VERSION(32, c)) {
        *c >> compiler;
        *c >> extrafiles;
    }
    compression = string();
    if (IS_PROTOCOL_VERSION(42, c))
        *c >> compression;
}

void GetNativeEnvMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);

    if (IS_PROTOCOL_VERSION(32, c)) {
        *c << compiler;
        *c << extrafiles;
    }
    if (IS_PROTOCOL_VERSION(42, c))
        *c << compression;
}

void UseNativeEnvMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> nativeVersion;
}

void UseNativeEnvMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << nativeVersion;
}

void EnvTransferMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> name;
    *c >> target;
}

void EnvTransferMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << name;
    *c << target;
}

void MonGetCSMsg::fill_from_channel(MsgChannel *c)
{
    if (IS_PROTOCOL_VERSION(29, c)) {
        Msg::fill_from_channel(c);
        *c >> filename;
        uint32_t _lang;
        *c >> _lang;
        lang = static_cast<CompileJob::Language>(_lang);
    } else {
        GetCSMsg::fill_from_channel(c);
    }

    *c >> job_id;
    *c >> clientid;
}

void MonGetCSMsg::send_to_channel(MsgChannel *c) const
{
    if (IS_PROTOCOL_VERSION(29, c)) {
        Msg::send_to_channel(c);
        *c << shorten_filename(filename);
        *c << (uint32_t) lang;
    } else {
        GetCSMsg::send_to_channel(c);
    }

    *c << job_id;
    *c << clientid;
}

void MonJobBeginMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> job_id;
    *c >> stime;
    *c >> hostid;
}

void MonJobBeginMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << job_id;
    *c << stime;
    *c << hostid;
}

void MonLocalJobBeginMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> hostid;
    *c >> job_id;
    *c >> stime;
    *c >> file;
}

void MonLocalJobBeginMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << hostid;
    *c << job_id;
    *c << stime;
    *c << shorten_filename(file);
}

void MonStatsMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> hostid;
    *c >> statmsg;
}

void MonStatsMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << hostid;
    *c << statmsg;
}

void TextMsg::fill_from_channel(MsgChannel *c)
{
    c->read_line(text);
}

void TextMsg::send_to_channel(MsgChannel *c) const
{
    c->write_line(text);
}

void StatusTextMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> text;
}

void StatusTextMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << text;
}

void VerifyEnvMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> environment;
    *c >> target;
}

void VerifyEnvMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << environment;
    *c << target;
}

void VerifyEnvResultMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    uint32_t read_ok;
    *c >> read_ok;
    ok = read_ok != 0;
}

void VerifyEnvResultMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << uint32_t(ok);
}

void BlacklistHostEnvMsg::fill_from_channel(MsgChannel *c)
{
    Msg::fill_from_channel(c);
    *c >> environment;
    *c >> target;
    *c >> hostname;
}

void BlacklistHostEnvMsg::send_to_channel(MsgChannel *c) const
{
    Msg::send_to_channel(c);
    *c << environment;
    *c << target;
    *c << hostname;
}

/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
