INCLUDES = -I$(srcdir)/../minilzo

noinst_LTLIBRARIES = libcommon.la

libcommon_la_SOURCES = job.cpp comm.cpp getifaddrs.cpp logging.cpp tempfile.c
libcommon_la_LIBADD = ../minilzo/libminilzo.la

bin_PROGRAMS = scheduler
scheduler_SOURCES = scheduler.cpp
scheduler_LDADD = libcommon.la

EXTRA_PROGRAMS = receiver submitter
receiver_SOURCES = receiver.cpp
receiver_LDADD = libcommon.la
submitter_SOURCES = submitter.cpp
submitter_LDADD = libcommon.la
