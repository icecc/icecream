#ifndef _SERVE_H
#define _SERVE_H

#include <string>

class MsgChannel;

extern MsgChannel *scheduler;

int handle_connection( MsgChannel *serv );

#endif
