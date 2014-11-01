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
	results=`realpath -s ${builddir}/results` && builddir2=`realpath -s ${builddir}` && cd ${srcdir} && ./test.sh ${prefix} $$results --builddir=$$builddir2

# Automake's conditionals are dumb and adding 'test-run: clangplugin' would make it warn about
# being defined in two contexts, even though in this context it's harmless and intended.
test-run: @HAVE_CLANG_DEVEL_DEP@

clangplugin: ${builddir}/clangplugin.so

# It appears there's no way to force libtool to create a shared library if AC_DISABLE_SHARED is used
# in configure (and it is, as icecream uses static libs). So just do it manually. If this matters
# for anybody, feel free to do this in a better way.
${builddir}/clangplugin.so: clangplugin.cpp
	$(CXX) -shared -fPIC -o $@ $^ -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -fno-rtti

clean: clean-clangplugin

clean-clangplugin:
	rm -f ${builddir}/clangplugin.so

TESTS = testargs

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services
testargs_LDADD = ../client/libclient.a ../services/libicecc.la $(LIBRSYNC)

check_PROGRAMS = testargs
testargs_SOURCES = args.cpp
