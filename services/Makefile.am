INCLUDES = -I$(srcdir)/../minilzo

lib_LTLIBRARIES = libicecream.la

libicecream_la_SOURCES = job.cpp comm.cpp getifaddrs.cpp logging.cpp tempfile.c
libicecream_la_LIBADD = ../minilzo/libminilzo.la

ice_HEADERS = job.h comm.h
icedir = $(includedir)/icecream

sbin_PROGRAMS = scheduler
scheduler_SOURCES = scheduler.cpp
scheduler_LDADD = libicecream.la

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = icecream.pc

