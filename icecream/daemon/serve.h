#ifndef _SERVE_H
#define _SERVE_H

#include <string>

class CompileJob;
class MsgChannel;
extern MsgChannel *scheduler;

int handle_connection( CompileJob *job, MsgChannel *serv );

#endif
