# TODO: noinst, but for now a shared one is easier
lib_LTLIBRARIES = libcommon.la

libcommon_la_SOURCES = job.cpp comm.cpp 

bin_PROGRAMS = scheduler
scheduler_SOURCES = scheduler.cpp
scheduler_LDADD = libcommon.la
