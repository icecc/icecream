#ifndef _ENVIRONMENT_H
#define _ENVIRONMENT_H

#include <list>
#include <string>

class MsgChannel;
std::list<std::string> available_environmnents(const std::string &basename);
bool install_environment( const std::string &basename, const std::string &_name, MsgChannel *c );

#endif
