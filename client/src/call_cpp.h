#ifndef _CALL_CPP_H_
#define _CALL_CPP_H_

#include "comm.h"

pid_t
call_cpp(CompileJob & job, int fdwrite, int fdread = -1);

#endif // _CALL_CPP_H_
