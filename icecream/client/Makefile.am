INCLUDES = -I$(srcdir)/../services

bin_PROGRAMS = icecc
icecc_SOURCES = main.cpp arg.cpp argutil.cpp climasq.cpp clinet.cpp \
	cpp.cpp exec.cpp filename.cpp implicit.cpp netutil.cpp io.cpp \
	safeguard.cpp sendfile.cpp strip.cpp tempfile.cpp timeval.cpp util.cpp

CLEANFILES = icecc++

all-local: icecc++

icecc++: icecc
	-rm -f icecc++
	$(LN_S) icecc icecc++

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	$(LN_S) icecc $(DESTDIR)$(bindir)/icecc++

uninstall-local:
	rm $(DESTDIR)$(bindir)/icecc++

