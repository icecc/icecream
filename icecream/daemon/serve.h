#ifndef _SERVE_H
#define _SERVE_H

#include <string>

class MsgChannel;

extern std::string scheduler_host;
extern unsigned short scheduler_port;

int handle_connection( MsgChannel *serv );

#endif
