bin_PROGRAMS = icecc
bin_SCRIPTS = icecc-create-env icecc-test-env

noinst_LIBRARIES = libclient.a
libclient_a_SOURCES = \
        src/arg.cpp \
        src/argv.c \
        src/cpp.cpp \
        src/local.cpp \
        src/remote.cpp \
        src/util.cpp \
        src/md5.c \
        src/safeguard.cpp

icecc_SOURCES = \
	src/main.cpp
icecc_LDADD = \
	libclient.a \
	../services/libicecc.la

noinst_HEADERS = \
	src/argv.h \
	src/client.h \
	src/md5.h \
	src/util.h
AM_CPPFLAGS = \
	-DPLIBDIR=\"$(pkglibexecdir)\" \
	-I$(top_srcdir)/services/include \
	-I$(top_srcdir)/ \
	-include $(top_srcdir)/config.h

EXTRA_DIST = icecc-create-env

AM_LIBTOOLFLAGS = --silent

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	rm -f $(DESTDIR)$(bindir)/icerun
	$(LN_S) $(bindir)/icecc $(DESTDIR)$(bindir)/icerun
	$(mkinstalldirs) $(DESTDIR)$(pkglibexecdir)
	rm -f $(DESTDIR)$(pkglibexecdir)/icecc-create-env
	$(LN_S) $(bindir)/icecc-create-env $(DESTDIR)$(pkglibexecdir)/icecc-create-env

	$(mkinstalldirs) $(DESTDIR)$(pkglibexecdir)/bin
	for link in g++ gcc c++ cc $(CLANG_SYMLINK_WRAPPERS); do \
		rm -f $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
		$(LN_S) $(bindir)/icecc $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
	done

uninstall-local:
	rm $(DESTDIR)$(bindir)/icerun
	rm $(DESTDIR)$(pkglibexecdir)/icecc-create-env

	for link in g++ gcc c++ cc $(CLANG_SYMLINK_WRAPPERS); do \
		rm $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
	done
