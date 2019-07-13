sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	main.cpp \
	serve.cpp \
	workit.cpp \
	environment.cpp \
	load.cpp \
	file_util.cpp

iceccd_LDADD = \
	../services/libicecc.la \
	$(LIB_KINFO) \
	$(CAPNG_LDADD) \
	$(ARCHIVE_LDADD)

AM_CPPFLAGS = \
	-I$(top_srcdir)/services

AM_LIBTOOLFLAGS = --silent

noinst_HEADERS = \
	environment.h \
	load.h \
	serve.h \
	workit.h \
	file_util.h
