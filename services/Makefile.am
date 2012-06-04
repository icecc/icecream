INCLUDES = -I$(srcdir)/../minilzo

lib_LTLIBRARIES = libicecc.la

libicecc_la_SOURCES = job.cpp comm.cpp getifaddrs.cpp logging.cpp tempfile.c platform.cpp gcc.cpp
libicecc_la_LIBADD = ../minilzo/libminilzo.la -ldl
libicecc_la_CFLAGS = -fPIC -DPIC
libicecc_la_CXXFLAGS = -fPIC -DPIC

ice_HEADERS = job.h comm.h
noinst_HEADERS = bench.h exitcode.h getifaddrs.h logging.h tempfile.h platform.h
icedir = $(includedir)/icecc

sbin_PROGRAMS = scheduler
scheduler_SOURCES = scheduler.cpp
scheduler_LDADD = libicecc.la

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = icecc.pc
