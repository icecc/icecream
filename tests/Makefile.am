# By default be lenient and don't fail if some tests are skipped.
# Strict mode will fail in such case.

test: test-full
test-strict:
	$(MAKE) test STRICT=1

test-prepare:
	if test -n "$(VALGRIND)"; then \
	    true; \
	elif test -x /sbin/setcap; then \
	    sudo /sbin/setcap cap_sys_chroot+ep ${sbindir}/iceccd ; \
	elif command -v filecap >/dev/null 2>/dev/null; then \
	    sudo filecap ${sbindir}/iceccd sys_chroot ; \
	else \
	    true ; \
	fi

test-full: test-prepare
	$(MAKE) test-run

test-run: test-setup.sh
	results=`realpath -s ${builddir}/results` && builddir2=`realpath -s ${builddir}` && cd ${srcdir} && /bin/bash test.sh ${prefix} $$results --builddir=$$builddir2 --strict=$(STRICT) --valgrind=$(VALGRIND)

TESTS = testargs

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services
testargs_LDADD = ../client/libclient.a ../services/libicecc.la $(LIBRSYNC)

check_PROGRAMS = testargs
testargs_SOURCES = args.cpp

check_SCRIPTS = test.sh test-setup.sh
