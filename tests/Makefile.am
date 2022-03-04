# By default be lenient and don't fail if some tests are skipped.
# Strict mode will fail in such case.

test: test-full
test-strict:
	$(MAKE) test STRICT=1

# The 'sudo true' for valgrind and/or sudo is because it sets up the password for further sudo
# uses in the test.sh script.
test-prepare:
	if test -n "$(VALGRIND)"; then \
	    sudo true; \
	elif test -n "$(SUDO)"; then \
	    sudo true; \
	elif test -x /sbin/setcap; then \
	    sudo /sbin/setcap cap_sys_chroot+ep ${sbindir}/iceccd ; \
	elif test -x /usr/sbin/setcap; then \
	    sudo /usr/sbin/setcap cap_sys_chroot+ep ${sbindir}/iceccd ; \
	elif command -v filecap >/dev/null 2>/dev/null; then \
	    sudo filecap ${sbindir}/iceccd sys_chroot ; \
	else \
	    true ; \
	fi

test-full: test-prepare
	$(MAKE) test-run

test-run: test-setup.sh
	results=`realpath -s ${builddir}/results` && builddir2=`realpath -s ${builddir}` && cd ${srcdir} && /bin/bash test.sh ${prefix} $$results --builddir=$$builddir2 --strict=$(STRICT) --valgrind=$(VALGRIND) --sudo=$(SUDO)

check_SCRIPTS = test.sh test-setup.sh

EXTRA_DIST = $(check_SCRIPTS)
