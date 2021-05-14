lib_LTLIBRARIES = libicecc.la
libicecc_la_SOURCES = src/job.cpp src/comm.cpp src/exitcode.cpp src/getifaddrs.cpp src/logging.cpp src/ncpus.c src/pipes.cpp src/tempfile.c src/platform.cpp src/gcc.cpp src/util.cpp
libicecc_la_LIBADD = \
	$(LZO_LDADD) \
	$(LIBZSTD_LIBS) \
	$(LIBCAP_NG_LIBS) \
	$(DL_LDADD)

libicecc_la_CFLAGS = -fPIC -DPIC
libicecc_la_CXXFLAGS = -fPIC -DPIC

icedir = $(includedir)/icecc
ice_HEADERS = \
	include/services/job.h \
	include/services/comm.h \
	include/services/logging.h

noinst_HEADERS = \
	include/services/exitcode.h \
	include/services/getifaddrs.h \
	include/services/logging.h \
	include/services/ncpus.h \
	include/services/pipes.h \
	include/services/tempfile.h \
	include/services/platform.h \
	include/services/util.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = icecc.pc

AM_LIBTOOLFLAGS = --silent
AM_CPPFLAGS = \
    -I$(top_srcdir)/services/include \
	-include "config.h"