sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	src/main.cpp \
	src/serve.cpp \
	src/workit.cpp \
	src/environment.cpp \
	src/load.cpp \
	src/file_util.cpp \
	src/client.cpp \
	src/daemon.cpp

iceccd_LDADD = \
	../services/libicecc.la \
	$(LIB_KINFO) \
	$(LIBCAP_NG_LIBS) \
	$(LIBARCHIVE_LIBS)

AM_CPPFLAGS = \
	-I$(top_srcdir)/services/include \
    -include $(top_srcdir)/config.h

AM_LIBTOOLFLAGS = --silent

noinst_HEADERS = \
	src/environment.h \
	src/load.h \
	src/serve.h \
	src/workit.h \
	src/file_util.h \
	src/client.hpp \
	src/clients.hpp \
	src/daemon.hpp
