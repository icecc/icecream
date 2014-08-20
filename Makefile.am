EXTRA_DIST = README.md

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = \
	services \
	daemon \
	client \
	doc \
	suse \
	compilerwrapper \
	scheduler \
	tests

dist-hook:
	( cd $(top_srcdir) && git log --date=short --pretty="format:@%cd  %an  <%ae>  [%H]%n%n%s%n%n%e%b" | sed -e "s|^\([^@]\)|\t\1|" -e "s|^@||" ) >$(distdir)/ChangeLog

test: install
	$(MAKE) -C tests $@
