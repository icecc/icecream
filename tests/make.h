#ifndef MAKE_H
#define MAKE_H

// some includes that'll make the compile take at least a little time
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// This is to prevent scheduler from ignoring stats for the compile job,
// as jobs with too small .o result are ignored in add_job_stats().
static volatile const int largedata[16384] = {1, 2};

#endif
