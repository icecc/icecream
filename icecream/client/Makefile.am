INCLUDES = -I$(srcdir)/../services

bin_PROGRAMS = icecc
icecc_SOURCES = main.cpp arg.cpp clinet.cpp \
	cpp.cpp filename.cpp netutil.cpp io.cpp \
	remote.cpp sendfile.cpp tempfile.cpp util.cpp local.cpp
icecc_LDADD = ../services/libcommon.la

CLEANFILES = icecc++

all-local: icecc++

icecc++: icecc
	-rm -f icecc++
	$(LN_S) icecc icecc++

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	rm -f $(DESTDIR)$(bindir)/icecc++
	$(LN_S) icecc $(DESTDIR)$(bindir)/icecc++

uninstall-local:
	rm $(DESTDIR)$(bindir)/icecc++

