## Process this file with automake to create Makefile.in
#
# Copyright (C) 1996-2002 Markus F.X.J. Oberhumer
#

AUTOMAKE_OPTIONS = 1.6


# /***********************************************************************
# // compiler-architecture-CPU specific CFLAGS
# ************************************************************************/

LZO_CFLAGS_GCC   =
LZO_CFLAGS_GCC_O =
LZO_CFLAGS_GCC_W = \
    -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
    -Wno-uninitialized

LZO_CFLAGS_GCC_i386   =
LZO_CFLAGS_GCC_i386_O = @CFLAGS_GCC_OMIT_FRAME_POINTER@ -fforce-addr
LZO_CFLAGS_GCC_i386_W =

LZO_CFLAGS_GCC_i386_i386_O = -m386
LZO_CFLAGS_GCC_i386_i486_O = -m486
LZO_CFLAGS_GCC_i386_i586_O =
LZO_CFLAGS_GCC_i386_i686_O =

L_CFLAGS = $(LZO_CFLAGS_@M_CC@) $(LZO_CFLAGS_@M_CC@_@M_ARCH@) $(LZO_CFLAGS_@M_CC@_@M_ARCH@_@M_CPU@)
L_CFLAGS_O = $(LZO_CFLAGS_@M_CC@_O) $(LZO_CFLAGS_@M_CC@_@M_ARCH@_O) $(LZO_CFLAGS_@M_CC@_@M_ARCH@_@M_CPU@_O)
L_CFLAGS_W = $(LZO_CFLAGS_@M_CC@_W) $(LZO_CFLAGS_@M_CC@_@M_ARCH@_W) $(LZO_CFLAGS_@M_CC@_@M_ARCH@_@M_CPU@_W)


# /***********************************************************************
# //
# ************************************************************************/

### LZO_DEBUG = -DLZO_DEBUG

INCLUDES = -I$(top_srcdir)/include

AM_CPPFLAGS = -DMINILZO_HAVE_CONFIG_H
AM_CFLAGS = $(L_CFLAGS) $(A_CFLAGS_W) $(L_CFLAGS_W) $(LZO_CFLAGS) $(LZO_DEBUG)


libminilzo_la_SOURCES = minilzo.c
noinst_LTLIBRARIES = libminilzo.la
