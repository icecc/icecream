#ifndef _SERVE_H
#define _SERVE_H

#include <string>

class CompileJob;
class MsgChannel;

int handle_connection( const std::string &basedir, CompileJob *job, MsgChannel *serv, int & out_fd );

#endif
