#ifndef _ARG_H_
#define _ARG_H_

#include "comm.h"

bool
analyse_argv(const char * const *     argv,
             CompileJob &             job,
             bool                     icerun,
             std::list<std::string> * extrafiles);

#endif // _ARG_H_
