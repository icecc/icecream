#ifndef _WORKIT_H
#define _WORKIT_H

#include <job.h>
#include <sys/types.h>
#include <string>

int work_it( CompileJob &j, const std::string &infilename,
             std::string &str_out, std::string &str_err,
             int &status, std::string &outfilename );

#endif
