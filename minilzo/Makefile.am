## Process this file with automake to create Makefile.in
#
# Copyright (C) 1996-2002 Markus F.X.J. Oberhumer
#

AUTOMAKE_OPTIONS = 1.6

# /***********************************************************************
# //
# ************************************************************************/

INCLUDES = -I$(top_srcdir)/include

AM_CPPFLAGS = -DMINILZO_HAVE_CONFIG_H

libminilzo_la_SOURCES = minilzo.c
libminilzo_la_CFLAGS = -fPIC -DPIC
noinst_LTLIBRARIES = libminilzo.la
noinst_HEADERS = lzoconf.h minilzo.h lzodefs.h

EXTRA_DIST = README.LZO 

