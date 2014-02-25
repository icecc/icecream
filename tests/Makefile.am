test: test-full

test-prepare:
	if test -x /sbin/setcap; then \
	    sudo /sbin/setcap cap_sys_chroot+ep ${sbindir}/iceccd ; \
	elif which filecap >/dev/null 2>/dev/null; then \
	    sudo filecap ${sbindir}/iceccd sys_chroot ; \
	else \
	    exit 1 ; \
	fi

test-full: test-prepare
	$(MAKE) test-run

test-run:
	results=`realpath -s ${builddir}/results` && cd ${srcdir} && ./test.sh ${prefix} $$results
