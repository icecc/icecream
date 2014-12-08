# quite some macros are taken from distcc

AC_PREREQ([2.63])

# ====================
# Version informations
# ====================
m4_define([icecream_version_major],[1])
m4_define([icecream_version_minor],[1rc1])
#m4_define([icecream_version_micro],[98])
m4_ifval([icecream_version_micro],
	m4_define([icecream_version],[icecream_version_major.icecream_version_minor.icecream_version_micro]),
	m4_define([icecream_version],[icecream_version_major.icecream_version_minor]))

# =============
# Automake init
# =============
AC_INIT([icecc], [icecream_version])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([1.11 foreign dist-bzip2])
AM_SILENT_RULES([yes])
AC_LANG([C++])

# ===========================
# Find required base packages
# ===========================
AC_PROG_CC
AC_PROG_CXX
if test "$GCC" = yes; then
    cast_align=-Wcast-align
    wshadow=
    AC_MSG_CHECKING([if GCC is actually Clang])
    AC_TRY_COMPILE(
        [],
        [
            #ifdef __clang__
            return 0;
            #else
            fail
            #endif
        ],
        [
            AC_MSG_RESULT(yes)
            # The code has numerous cast alignment warnings that only clang warns about,
            # and it probably(?) doesn't matter in practice.
            cast_align=
            # Clang is not so overzealous like GCC and doesn't warn about cases which
            # are very unlikely to be problems (e.g. function argument shadowing
            # a function elsewhere), so it's not so annoying.
            wshadow=-Wshadow
        ],
        [ AC_MSG_RESULT(no) ])
    CFLAGS="-g -W -Wall \
-Wshadow -Wpointer-arith $cast_align -Wwrite-strings \
-Waggregate-return -Wstrict-prototypes -Wmissing-prototypes \
-Wnested-externs $CFLAGS"
    CXXFLAGS=" -g -W -Wall -Wpointer-arith $cast_align $wshadow -Wwrite-strings $CXXFLAGS"
    AC_MSG_NOTICE([Adding gcc options: $CFLAGS])
fi

AC_PROG_MAKE_SET
AC_PROG_INSTALL
AC_C_INLINE
AC_DISABLE_SHARED
AC_ENABLE_STATIC
AC_PROG_LIBTOOL

ICECC_LIBCAP_NG_PATH

# =============
# Documentation
# =============
AC_ARG_WITH(man,
    [AS_HELP_STRING([--without-man], [Do not build manpage])],
    [with_man="$withval"],
    [with_man=yes]
)
AS_IF([test "x$with_man" != "xno"], [
    build_man=yes
    AC_PATH_PROGS(DOCBOOK2X,
        [docbook2x-man db2x_docbook2man docbook-to-man docbook2man.pl docbook2man])
    AS_IF([test -z "$DOCBOOK2X"], [
        AC_MSG_WARN([docbook2x is missing. Install docbook2x package.])
        DOCBOOK2X='echo docbook2x is missing. Install docbook2x package.'])
], [build_man=no])
AC_SUBST(DOCBOOK2X)
AM_CONDITIONAL([WITH_ICECREAM_MAN], [test "x$build_man" != "xno"])

########################################################################
### Checks for header files

# Some of these are needed by popt (or other libraries included in the future).

AC_CHECK_HEADERS([unistd.h stdint.h signal.h sys/types.h sys/signal.h ifaddrs.h kinfo.h sys/param.h devstat.h])
AC_CHECK_HEADERS([ctype.h sys/resource.h sys/socket.h sys/socketvar.h sys/stat.h sys/select.h sys/vfs.h])
AC_CHECK_HEADERS([mach/host_info.h])
AC_CHECK_HEADERS([netinet/in.h], [], [],
[#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
])
AC_CHECK_HEADERS([arpa/nameser.h], [], [],
[#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
])
AC_CHECK_HEADERS([resolv.h], [], [],
[#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#if HAVE_ARPA_NAMESER_H
# include <arpa/nameser.h>
#endif
])

AC_ARG_VAR(TAR, [Specifies tar path])
AC_PATH_PROG(TAR, [tar])
AC_DEFINE_UNQUOTED([TAR], ["$TAR"], [Define path to tar])
AC_CHECK_HEADERS([float.h mcheck.h alloca.h sys/mman.h netinet/tcp.h])
AC_CHECK_HEADERS([netinet/tcp_var.h], [], [],
[#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#if HAVE_SYS_SOCKETVAR_H
# include <sys/socketvar.h>
#endif
#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#if HAVE_NETINET_TCP_H
# include <netinet/tcp.h>
#endif
])

AC_CHECK_HEADERS([sys/user.h])

######################################################################
dnl Checks for types

AC_CHECK_TYPES([sa_family_t, socklen_t, in_port_t, in_addr_t], , ,
	       [
#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#if HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#if HAVE_ARPA_NAMESER_H
# include <arpa/nameser.h>
#endif
])

AC_CHECK_MEMBER([struct ifreq.ifr_dstaddr],
                [AC_DEFINE(HAVE_IFR_DSTADDR, 1, [Set to 1 if struct ifr_ifru has member ifr_dstaddr] )],
                [AC_DEFINE(HAVE_IFR_DSTADDR, 0, [Set to 0 if struct ifr_ifru has no ifr_dstaddr] )],
                [
#include <net/if.h>
])

########################################################################
### Checks for libraries.

# The following test taken from the cvs sources via Samba:
# If we can't find connect, try looking in -lsocket, -lnsl, and -linet.
# The Irix 5 libc.so has connect and gethostbyname, but Irix 5 also has
# libsocket.so which has a bad implementation of gethostbyname (it
# only looks in /etc/hosts), so we only look for -lsocket if we need
# it.
AC_CHECK_FUNCS(connect)
if test x"$ac_cv_func_connect" = x"no"; then
    case "$LIBS" in
    *-lnsl*) ;;
    *) AC_CHECK_LIB(nsl_s, printf) ;;
    esac
    case "$LIBS" in
    *-lnsl*) ;;
    *) AC_CHECK_LIB(nsl, printf) ;;
    esac
    case "$LIBS" in
    *-lsocket*) ;;
    *) AC_CHECK_LIB(socket, connect) ;;
    esac
    case "$LIBS" in
    *-linet*) ;;
    *) AC_CHECK_LIB(inet, connect) ;;
    esac
    dnl We can't just call AC_CHECK_FUNCS(connect) here, because the value
    dnl has been cached.
    if test x"$ac_cv_lib_socket_connect" = x"yes" || 
       test x"$ac_cv_lib_inet_connect" = x"yes"; then
        # ac_cv_func_connect=yes
        # don't!  it would cause AC_CHECK_FUNC to succeed next time configure is run
        AC_DEFINE(HAVE_CONNECT,1,[Whether the system has connect()])
    fi
fi

AC_CHECK_LIB(resolv, hstrerror, , , [-lnsl -lsocket])
AC_CHECK_LIB(resolv, inet_aton, , , [-lnsl -lsocket])

AC_CHECK_FUNCS([setsid flock lockf hstrerror strerror setuid setreuid])
AC_CHECK_FUNCS([getuid geteuid mcheck wait4 wait3 waitpid setgroups getcwd])
AC_CHECK_FUNCS([snprintf vsnprintf vasprintf asprintf getcwd getwd])
AC_CHECK_FUNCS([getrusage strsignal gettimeofday])
AC_CHECK_FUNCS([getaddrinfo getnameinfo inet_ntop inet_ntoa])
AC_CHECK_FUNCS([strndup mmap strlcpy])
AC_CHECK_FUNCS([getloadavg])

AC_CHECK_DECLS([snprintf, vsnprintf, vasprintf, asprintf, strndup])

AC_CHECK_LIB(lzo2, lzo1x_1_compress, LZO_LDADD=-llzo2,
	AC_MSG_ERROR([Could not find lzo2 library - please install lzo-devel]))
AC_SUBST(LZO_LDADD)

# In DragonFlyBSD daemon needs to be linked against libkinfo.
case $host_os in
  dragonfly*) LIB_KINFO="-lkinfo" ;;
  *) LIB_KINFO="" ;;
esac
AC_SUBST(LIB_KINFO)

AC_ARG_ENABLE(clang-rewrite-includes,
    AS_HELP_STRING([--enable-clang-rewrite-includes],
        [Use by default Clang's -frewrite-includes option.]))

if test "$enable_clang_rewrite_includes" = "yes"; then
    AC_DEFINE(HAVE_CLANG_REWRITE_INCLUDES, 1, [Define to 1 if clang supports -frewrite-includes])
elif test "$enable_clang_rewrite_includes" = "no"; then
    true  # do not enable
else
    AC_CHECK_PROG(CLANG,clang,clang)
    if test -n "$CLANG"; then
        AC_MSG_CHECKING([whether clang -Werror works for unknown options])
        $CLANG -Werror -totallybogusoption -E - >/dev/null 2>/dev/null
        if test $? -eq 0; then
            AC_MSG_RESULT(no)
            # can't detect if the option is supported, but that's too old clang anyway
        else
            AC_MSG_RESULT(yes)
            AC_MSG_CHECKING([for clang -E -frewrite-includes])
            $CLANG -Werror -E -frewrite-includes - >/dev/null 2>/dev/null
            if test $? -eq 0; then
                AC_MSG_RESULT(yes)
                AC_DEFINE(HAVE_CLANG_REWRITE_INCLUDES, 1, [Define to 1 if clang supports -frewrite-includes])
            else
                AC_MSG_RESULT(no)
            fi
        fi
    fi
fi

HAVE_CLANG_DEVEL_DEP=
AC_CHECK_PROG(CLANG,clang,clang)
AC_LANG_PUSH([C++])
save_CPPFLAGS=$CPPFLAGS
CPPFLAGS="$CPPFLAGS -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS"
AC_CHECK_HEADER(clang/AST/RecursiveASTVisitor.h,
    [ HAVE_CLANG_DEVEL_DEP=clangplugin ] )
CPPFLAGS="$save_CPPFLAGS"
AC_LANG_POP([C++])
AC_SUBST(HAVE_CLANG_DEVEL_DEP)

AC_ARG_ENABLE(clang-wrappers,
    AS_HELP_STRING([--enable-clang-wrappers],
        [Use symlink wrappers for clang/clang++.]))

CLANG_SYMLINK_WRAPPERS=
if test "$enable_clang_wrappers" = "yes"; then
    CLANG_SYMLINK_WRAPPERS='clang clang++'
elif test "$enable_clang_wrappers" = "no"; then
    true # do not enable
else
    AC_CHECK_PROG(CLANG,clang,clang)
    if test -n "$CLANG"; then
        CLANG_SYMLINK_WRAPPERS='clang clang++'
    fi
fi
AC_SUBST(CLANG_SYMLINK_WRAPPERS)

AC_ARG_ENABLE(gcc-color-diagnostics,
    AS_HELP_STRING([--enable-gcc-color-diagnostics],
        [Assume by default GCC has -fdiagnostics-color=auto option.]))

if test "$enable_gcc_color_diagnostics" = "yes"; then
    AC_DEFINE(HAVE_GCC_COLOR_DIAGNOSTICS, 1, [Define to 1 if gcc supports -fdiagnostics-color=auto])
else
    true  # do not enable
fi

AC_ARG_ENABLE(gcc-show-caret,
    AS_HELP_STRING([--enable-gcc-show-caret],
        [Assume by default GCC has -fdiagnostics-show-caret option.]))

if test "$enable_gcc_show_caret" = "yes"; then
    AC_DEFINE(HAVE_GCC_SHOW_CARET, 1, [Define to 1 if gcc supports -fdiagnostics-show-caret])
elif test "$enable_gcc_show_caret" = "no"; then
    true  # do not enable
else
    AC_CHECK_PROG(GCC_BIN,gcc,gcc)
    if test -n "$GCC_BIN"; then
        AC_MSG_CHECKING([for gcc -fdiagnostics-show-caret])
        $GCC_BIN -Werror -E -fdiagnostics-show-caret - >/dev/null 2>/dev/null
        if test $? -eq 0; then
            AC_MSG_RESULT(yes)
            AC_DEFINE(HAVE_GCC_SHOW_CARET, 1, [Define to 1 if gcc supports -fdiagnostics-show-caret])
        else
            AC_MSG_RESULT(no)
        fi
    fi
fi

AC_CONFIG_FILES([ Makefile ])
AC_CONFIG_FILES([ client/Makefile ])
AC_CONFIG_FILES([ daemon/Makefile ])
AC_CONFIG_FILES([ doc/Makefile ])
AC_CONFIG_FILES([ services/Makefile ])
AC_CONFIG_FILES([ services/icecc.pc ])
AC_CONFIG_FILES([ suse/Makefile ])
AC_CONFIG_FILES([ compilerwrapper/Makefile ])
AC_CONFIG_FILES([ scheduler/Makefile ])
AC_CONFIG_FILES([ tests/Makefile ])
AC_CONFIG_FILES([ client/icecc-create-env ])
AC_OUTPUT([ suse/icecream.spec ])
if test "$prefix" = NONE; then
  prefix=$ac_default_prefix
fi

AC_DEFUN([KDE_EXPAND_MAKEVAR], [
savex=$exec_prefix
test "x$exec_prefix" = xNONE && exec_prefix=$prefix
tmp=$$2
while $1=`eval echo "$tmp"`; test "x$$1" != "x$tmp"; do tmp=$$1; done
exec_prefix=$savex
])

KDE_EXPAND_MAKEVAR(mybindir, bindir)
AC_DEFINE_UNQUOTED(BINDIR, "$mybindir", [Where to look for icecc])

myorundir='${localstatedir}/run'
KDE_EXPAND_MAKEVAR(myrundir, myorundir)
AC_DEFINE_UNQUOTED(RUNDIR, "$myrundir", [Where to place pid files])

KDE_EXPAND_MAKEVAR(mylibexecdir, libexecdir)
PKGLIBEXECDIR="$mylibexecdir/$PACKAGE"
AC_SUBST(PKGLIBEXECDIR)

AC_OUTPUT

