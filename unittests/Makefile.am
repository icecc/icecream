TESTS = testargs

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la $(LIBRSYNC)

check_PROGRAMS = testargs
testargs_SOURCES = args.cpp
