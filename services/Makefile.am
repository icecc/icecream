lib_LTLIBRARIES = libicecc.la
libicecc_la_SOURCES = job.cpp comm.cpp exitcode.cpp getifaddrs.cpp logging.cpp ncpus.c pipes.cpp tempfile.c platform.cpp gcc.cpp util.cpp
libicecc_la_LIBADD = \
	$(LZO_LDADD) \
	$(LIBZSTD_LIBS) \
	$(LIBCAP_NG_LIBS) \
	$(DL_LDADD)

libicecc_la_CFLAGS = -fPIC -DPIC
libicecc_la_CXXFLAGS = -fPIC -DPIC

icedir = $(includedir)/icecc
ice_HEADERS = \
	job.h \
	comm.h \
	logging.h

noinst_HEADERS = \
	exitcode.h \
	getifaddrs.h \
	logging.h \
	ncpus.h \
	pipes.h \
	tempfile.h \
	platform.h \
	util.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = icecc.pc

AM_LIBTOOLFLAGS = --silent
