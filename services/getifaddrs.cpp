/* getifaddrs -- get names and addresses of all network interfaces
   Copyright (C) 1999,2002 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

/**
 * 02-12-26, tim@tjansen.de: put in kde_ namespace, C++ fixes,
 *                           included ifreq.h
 *                           removed glibc dependencies
 */

#include "config.h"

#ifndef HAVE_IFADDRS_H

#include "getifaddrs.h"
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>

#ifndef IF_NAMESIZE
#define IF_NAMESIZE IFNAMSIZ
#endif

#ifdef SIOCGIFCONF

#define old_siocgifconf 0

static inline void
__ifreq (struct ifreq **ifreqs, int *num_ifs, int sockfd)
{
  int fd = sockfd;
  struct ifconf ifc;
  int rq_len;
  int nifs;
# define RQ_IFS	4

  if (fd < 0)
    fd = socket (AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      *num_ifs = 0;
      *ifreqs = NULL;
      return;
    }

  ifc.ifc_buf = NULL;

  /* We may be able to get the needed buffer size directly, rather than
     guessing.  */
  if (! old_siocgifconf)
    {
      ifc.ifc_buf = NULL;
      ifc.ifc_len = 0;
      if (ioctl (fd, SIOCGIFCONF, &ifc) < 0 || ifc.ifc_len == 0)
	{
	  rq_len = RQ_IFS * sizeof (struct ifreq);
	}
      else
	rq_len = ifc.ifc_len;
    }
  else
    rq_len = RQ_IFS * sizeof (struct ifreq);

  /* Read all the interfaces out of the kernel.  */
  while (1)
    {
      ifc.ifc_len = rq_len;
      ifc.ifc_buf = (char*) realloc (ifc.ifc_buf, ifc.ifc_len);
      if (ifc.ifc_buf == NULL || ioctl (fd, SIOCGIFCONF, &ifc) < 0)
	{
	  if (ifc.ifc_buf)
	    free (ifc.ifc_buf);

	  if (fd != sockfd)
	    close (fd);

	  *num_ifs = 0;
	  *ifreqs = NULL;
	  return;
	}

      if (!old_siocgifconf || ifc.ifc_len < rq_len)
	break;

      rq_len *= 2;
    }

  nifs = ifc.ifc_len / sizeof (struct ifreq);

  if (fd != sockfd)
    close (fd);

  *num_ifs = nifs;
  *ifreqs = (ifreq*)realloc (ifc.ifc_buf, nifs * sizeof (struct ifreq));
}

static inline struct ifreq *
__if_nextreq (struct ifreq *ifr)
{
  return ifr + 1;
}

static inline void
__if_freereq (struct ifreq *ifreqs, int num_ifs)
{
  free (ifreqs);
}

/* Create a linked list of `struct kde_ifaddrs' structures, one for each
   network interface on the host machine.  If successful, store the
   list in *IFAP and return 0.  On errors, return -1 and set `errno'.  */
int
kde_getifaddrs (struct kde_ifaddrs **ifap)
{
  /* This implementation handles only IPv4 interfaces.
     The various ioctls below will only work on an AF_INET socket.
     Some different mechanism entirely must be used for IPv6.  */
  int fd = socket (AF_INET, SOCK_DGRAM, 0);
  struct ifreq *ifreqs;
  int nifs;

  if (fd < 0)
    return -1;

  __ifreq (&ifreqs, &nifs, fd);
  if (ifreqs == NULL)		/* XXX doesn't distinguish error vs none */
    {
      close (fd);
      return -1;
    }

  /* Now we have the list of interfaces and each one's address.
     Put it into the expected format and fill in the remaining details.  */
  if (nifs == 0)
    *ifap = NULL;
  else
    {
      struct Storage
      {
	struct kde_ifaddrs ia;
	struct sockaddr addr, netmask, broadaddr;
	char name[IF_NAMESIZE];
      } *storage;
      struct ifreq *ifr;
      int i;

      storage = (Storage*) malloc (nifs * sizeof storage[0]);
      if (storage == NULL)
	{
	  close (fd);
	  __if_freereq (ifreqs, nifs);
	  return -1;
	}

      i = 0;
      ifr = ifreqs;
      do
	{
	  /* Fill in all pointers to the storage we've already allocated.  */
	  storage[i].ia.ifa_next = &storage[i + 1].ia;
	  storage[i].ia.ifa_addr = &storage[i].addr;
	  storage[i].ia.ifa_netmask = &storage[i].netmask;
	  storage[i].ia.ifa_broadaddr = &storage[i].broadaddr; /* & dstaddr */

	  /* Now copy the information we already have from SIOCGIFCONF.  */
	  storage[i].ia.ifa_name = strncpy (storage[i].name, ifr->ifr_name,
					    sizeof storage[i].name);
	  storage[i].addr = ifr->ifr_addr;

	  /* The SIOCGIFCONF call filled in only the name and address.
	     Now we must also ask for the other information we need.  */

	  if (ioctl (fd, SIOCGIFFLAGS, ifr) < 0)
	    break;
	  storage[i].ia.ifa_flags = ifr->ifr_flags;

	  ifr->ifr_addr = storage[i].addr;

	  if (ioctl (fd, SIOCGIFNETMASK, ifr) < 0)
	    break;
	  storage[i].netmask = ifr->ifr_netmask;

	  if (ifr->ifr_flags & IFF_BROADCAST)
	    {
	      ifr->ifr_addr = storage[i].addr;
	      if (ioctl (fd, SIOCGIFBRDADDR, ifr) < 0)
		break;
	      storage[i].broadaddr = ifr->ifr_broadaddr;
	    }
	  else if (ifr->ifr_flags & IFF_POINTOPOINT)
	    {
	      ifr->ifr_addr = storage[i].addr;
// Needed on Cygwin
#ifndef SIOCGIFDSTADDR 
  #define SIOCGIFDSTADDR 0x8917
#endif
	      if (ioctl (fd, SIOCGIFDSTADDR, ifr) < 0)
		break;
	      storage[i].broadaddr = ifr->ifr_dstaddr;
	      // FIXME on Cygwin you need to comment the line above
              // and uncomment the line below
	      // storage[i].broadaddr = ifr->ifr_broadaddr;
	    }
	  else
	    /* Just 'cause.  */
	    memset (&storage[i].broadaddr, 0, sizeof storage[i].broadaddr);

	  storage[i].ia.ifa_data = NULL; /* Nothing here for now.  */

	  ifr = __if_nextreq (ifr);
	} while (++i < nifs);
      if (i < nifs)		/* Broke out early on error.  */
	{
	  close (fd);
	  free (storage);
	  __if_freereq (ifreqs, nifs);
	  return -1;
	}

      storage[i - 1].ia.ifa_next = NULL;

      *ifap = &storage[0].ia;

      close (fd);
      __if_freereq (ifreqs, nifs);
    }

  return 0;
}

void
kde_freeifaddrs (struct kde_ifaddrs *ifa)
{
  free (ifa);
}

#else
int kde_getifaddrs(struct kde_ifaddrs **) {
	return 1;
}
void kde_freeifaddrs(struct kde_ifaddrs *) {
}
struct { } kde_ifaddrs;

#endif

#endif
