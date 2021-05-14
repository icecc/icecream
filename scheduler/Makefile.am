
sbin_PROGRAMS = icecc-scheduler
icecc_scheduler_SOURCES = src/compileserver.cpp src/job.cpp src/jobstat.cpp src/scheduler.cpp
icecc_scheduler_LDADD = ../services/libicecc.la

AM_LIBTOOLFLAGS = --silent
AM_CPPFLAGS = \
    -I$(top_srcdir)/services/include \
    -include $(top_srcdir)/config.h

noinst_HEADERS = \
    src/compileserver.h \
    src/job.h \
    src/jobstat.h \
    src/scheduler.h
