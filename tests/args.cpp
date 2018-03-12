#include "client.h"
#include "client/util.h"
#include "services/platform.h"
#include <list>
#include <string>
#include <iostream>

using namespace std;

static const char *ICECC_COLOR_DIAGNOSTICS = NULL;
void backup_icecc_color_diagnostics() {
  ICECC_COLOR_DIAGNOSTICS = getenv("ICECC_COLOR_DIAGNOSTICS");
  setenv("ICECC_COLOR_DIAGNOSTICS", "0", 1);
}

void restore_icecc_color_diagnostics() {
  if (ICECC_COLOR_DIAGNOSTICS)
    setenv("ICECC_COLOR_DIAGNOSTICS", ICECC_COLOR_DIAGNOSTICS, 1);
  else
    unsetenv("ICECC_COLOR_DIAGNOSTICS");
}

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
  } else {
    cout << prefix << " unit test passed" << endl;
  }
}

static void test_1() {
   const char * argv[] = { "gcc", "-D", "TEST=1", "-c", "main.cpp", "-o", "main.o", 0 };
   backup_icecc_color_diagnostics();
   test_run(__FUNCTION__, argv, false, "local:0 language:C++ compiler:gcc local:'' remote:'-c, -fdirectives-only' rest:'-D, TEST=1'");
   restore_icecc_color_diagnostics();
}

static void test_2() {
   const char * argv[] = { "gcc", "-DTEST=2", "-c", "main.cpp", "-o", "main.o", 0 };
   backup_icecc_color_diagnostics();
   test_run(__FUNCTION__, argv, false, "local:0 language:C++ compiler:gcc local:'' remote:'-c, -fdirectives-only' rest:'-DTEST=2'");
   restore_icecc_color_diagnostics();
}

static void test_3() {
   const char * argv[] = { "clang", "-D", "TEST=3", "-I.", "-c", "make1.cpp", "-o", "make.o", 0};

   string target = read_command_output("clang -dumpmachine");
   string expected = "local:0 language:C++ compiler:clang local:'-I.' remote:'-c, -target, " + target + "' rest:'-D, TEST=3, -fcolor-diagnostics'";

   backup_icecc_color_diagnostics();
   test_run(__FUNCTION__, argv, false, expected);
   restore_icecc_color_diagnostics();
}

int main() {

  string machine_name = determine_platform();
  if (machine_name.compare(0, 6, "Darwin") != 0) {
    // TODO:  figure out how to make these tests pass on Mac, then re-enable.
    test_1();
    test_2();
  }
  test_3();

  cout << "All unit tests passed" << endl;
  exit(0);
}
