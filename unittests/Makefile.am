TESTS = testargs

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services/include  -I$(top_srcdir)/  -I$(top_srcdir)/client/src
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs
testargs_SOURCES = args.cpp

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
