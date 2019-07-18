#include "client.h"
#include <list>
#include <string>
#include <iostream>

using namespace std;

void test_run(const string &prefix, const char * const *argv, bool icerun, const string& expected) {
  list<string> extrafiles;
  CompileJob job;
  bool local = analyse_argv(argv, job, icerun, &extrafiles);
  std::stringstream str;
  str << "local:" << local;
  str << " language:" << job.language();
  str << " compiler:" << job.compilerName();
  str << " local:" << concat_args(job.localFlags());
  str << " remote:" << concat_args(job.remoteFlags());
  str << " rest:" << concat_args(job.restFlags());
  if (str.str() != expected) {
    cerr << prefix << " failed\n";
    cerr << "     got: \"" << str.str() << "\"\nexpected: \"" << expected << "\"\n";
    exit(1);
  }
}

static void test_1() {
   const char * argv[] = { "gcc", "-D", "TEST=1", "-c", "main.cpp", "-o", "main.o", 0 };
   test_run("1", argv, false, "local:0 language:C++ compiler:gcc local:'-D, TEST=1' remote:'-c' rest:''");
}

static void test_2() {
   const char * argv[] = { "gcc", "-DTEST=1", "-c", "main.cpp", "-o", "main.o", 0 };
   test_run("2", argv, false, "local:0 language:C++ compiler:gcc local:'-DTEST=1' remote:'-c' rest:''");
}

static void test_3() {
   const char * argv[] = { "clang", "-D", "TEST1=1", "-I.", "-c", "make1.cpp", "-o", "make.o", "-target", "x86_64-unknown-linux-gnu", 0};
   test_run("3", argv, false, "local:0 language:C++ compiler:clang local:'-D, TEST1=1, -I.' remote:'-c' rest:'-target, x86_64-unknown-linux-gnu'");
}

int main() {
    unsetenv( "ICECC_COLOR_DIAGNOSTICS" );
    unsetenv( "ICECC" );
    unsetenv( "ICECC_VERSION" );
    unsetenv( "ICECC_DEBUG" );
    unsetenv( "ICECC_LOGFILE" );
    unsetenv( "ICECC_REPEAT_RATE" );
    unsetenv( "ICECC_PREFERRED_HOST" );
    unsetenv( "ICECC_CC" );
    unsetenv( "ICECC_CXX" );
    unsetenv( "ICECC_CLANG_REMOTE_CPP" );
    unsetenv( "ICECC_IGNORE_UNVERIFIED" );
    unsetenv( "ICECC_EXTRAFILES" );
    unsetenv( "ICECC_COLOR_DIAGNOSTICS" );
    unsetenv( "ICECC_CARET_WORKAROUND" );
    setenv( "ICECC_REMOTE_CPP", "0", 1 );
    test_1();
    test_2();
    test_3();
    return 0;
}
