INCLUDES = -I$(srcdir)/../common -I$(srcdir)/../services

bin_PROGRAMS = icecc
icecc_LDADD = ../common/libcommon.la
icecc_SOURCES = arg.c climasq.c clinet.c clirpc.c cpp.c main.c implicit.c strip.c

all-local: icecc++

icecc++: icecc
	-rm -f icecc++
	$(LN_S) icecc icecc++

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	$(LN_S) icecc $(DESTDIR)$(bindir)/icecc++

uninstall-local:
	rm $(DESTDIR)$(bindir)/icecc++

