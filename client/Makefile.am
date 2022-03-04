bin_PROGRAMS = icecc
bin_SCRIPTS = icecc-create-env icecc-test-env

noinst_LIBRARIES = libclient.a
libclient_a_SOURCES = \
        arg.cpp \
        argv.c \
        cpp.cpp \
        local.cpp \
        remote.cpp \
        util.cpp \
        md5.c \
        safeguard.cpp

icecc_SOURCES = \
	main.cpp 
icecc_LDADD = \
	libclient.a \
	../services/libicecc.la

noinst_HEADERS = \
	argv.h \
	client.h \
	md5.h \
	util.h
AM_CPPFLAGS = \
	-DPLIBDIR=\"$(pkglibexecdir)\" \
	-I$(top_srcdir)/services \
	-I$(top_srcdir)/

EXTRA_DIST = $(bin_SCRIPTS)

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
