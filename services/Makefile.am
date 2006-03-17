INCLUDES = -I$(srcdir)/../minilzo

noinst_LTLIBRARIES = libcommon.la

libcommon_la_SOURCES = job.cpp comm.cpp getifaddrs.cpp logging.cpp tempfile.c
libcommon_la_LIBADD = ../minilzo/libminilzo.la

sbin_PROGRAMS = scheduler
scheduler_SOURCES = scheduler.cpp
scheduler_LDADD = libcommon.la

