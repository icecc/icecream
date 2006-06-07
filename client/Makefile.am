INCLUDES = -I$(srcdir)/../services

bin_PROGRAMS = icecc
bin_SCRIPTS = icecc-create-env
icecc_SOURCES = main.cpp arg.cpp cpp.cpp local.cpp remote.cpp util.cpp md5.c safeguard.cpp
icecc_LDADD = ../services/libicecream.la

CLEANFILES = icecc++

all-local: icecc++

icecc++:
	-rm -f icecc++
	$(LN_S) icecc icecc++

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	for link in g++ gcc c++ cc; do \
	  rm -f $(DESTDIR)$(bindir)/$$link ;\
	  $(LN_S) icecc $(DESTDIR)$(bindir)/$$link ;\
	done

uninstall-local:
	for link in g++ gcc c++ cc; do \
	  rm $(DESTDIR)$(bindir)/$$link ;\
	done
