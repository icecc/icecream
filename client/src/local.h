#ifndef _LOCAL_H_
#define _LOCAL_H_

#include "comm.h"

int
build_local(CompileJob & job, MsgChannel * daemon, struct rusage * usage = 0);

std::string
find_compiler(const CompileJob & job);

bool
compiler_is_clang(const CompileJob & job);

bool
compiler_only_rewrite_includes(const CompileJob & job);

std::string
compiler_path_lookup(const std::string & compiler);

std::string
clang_get_default_target(const CompileJob & job);

bool
compiler_get_arch_flags(const CompileJob &       job,
                        bool                     march,
                        bool                     mcpu,
                        bool                     mtune,
                        std::list<std::string> & args);

#endif // _LOCAL_H_
