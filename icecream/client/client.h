#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <job.h>

#include "exitcode.h"
#include "logging.h"
#include "util.h"

class MsgChannel;

/* In arg.cpp.  */
bool analyse_argv (const char * const *argv,
                   CompileJob &job);

/* In cpp.cpp.  */
pid_t call_cpp (CompileJob &job, int fd);

/* In local.cpp.  */
int build_local (CompileJob& job, MsgChannel *scheduler);

/* In remote.cpp.  */
int build_remote (CompileJob &job, MsgChannel *scheduler);

#endif
