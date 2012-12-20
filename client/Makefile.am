bin_PROGRAMS = icecc
pkglibexec_SCRIPTS = icecc-create-env
icecc_SOURCES = \
	main.cpp \
	arg.cpp \
	cpp.cpp \
	local.cpp \
	remote.cpp \
	util.cpp \
	md5.c \
	safeguard.cpp
icecc_LDADD = \
	../services/libicecc.la \
	$(LIBRSYNC)

noinst_HEADERS = \
	client.h \
	md5.h \
	util.h
AM_CPPFLAGS = \
	-DPLIBDIR=\"$(pkglibexecdir)\" \
	-I$(top_srcdir)/services

EXTRA_DIST = icecc-create-env

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	rm -f $(DESTDIR)$(bindir)/icerun
	$(LN_S) $(bindir)/icecc $(DESTDIR)$(bindir)/icerun

	$(mkinstalldirs) $(DESTDIR)$(pkglibexecdir)/bin
	for link in g++ gcc c++ cc $(CLANG_SYMLINK_WRAPPERS); do \
		rm -f $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
		$(LN_S) $(bindir)/icecc $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
	done

uninstall-local:
	rm $(DESTDIR)$(bindir)/icerun

	for link in g++ gcc c++ cc $(CLANG_SYMLINK_WRAPPERS); do \
		rm $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
	done
