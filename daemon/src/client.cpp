#include "client.hpp"
#include <cassert>
#include <unistd.h>
#include "services/logging.h"

Client::Client()
{
    job_id = 0;
    channel = nullptr;
    job = nullptr;
    usecsmsg = nullptr;
    client_id = 0;
    status = UNKNOWN;
    pipe_from_child = -1;
    pipe_to_child = -1;
    child_pid = -1;
}

Client::~Client()
{
    status = (Status) - 1;
    delete channel;
    channel = nullptr;
    delete usecsmsg;
    usecsmsg = nullptr;
    delete job;
    job = nullptr;

    if(pipe_from_child >= 0)
    {
        if(- 1 == close(pipe_from_child) && (errno != EBADF))
        {
            log_perror("Failed to close pipe from child process");
        }
    }
    if(pipe_to_child >= 0)
    {
        if(- 1 == close(pipe_to_child) && (errno != EBADF))
        {
            log_perror("Failed to close pipe to child process");
        }
    }

}

std::string Client::status_str(Status status)
{
    switch(status)
    {
        case UNKNOWN:
            return "unknown";
        case GOTNATIVE:
            return "gotnative";
        case PENDING_USE_CS:
            return "pending_use_cs";
        case JOBDONE:
            return "jobdone";
        case LINKJOB:
            return "linkjob";
        case TOINSTALL:
            return "toinstall";
        case WAITINSTALL:
            return "waitinstall";
        case TOCOMPILE:
            return "tocompile";
        case WAITFORCS:
            return "waitforcs";
        case CLIENTWORK:
            return "clientwork";
        case WAITCOMPILE:
            return "waitcompile";
        case WAITFORCHILD:
            return "waitforchild";
        case WAITCREATEENV:
            return "waitcreateenv";
    }

    assert(false);
    return std::string(); // shutup gcc
}

std::string Client::dump() const {
    std::string ret = status_str(status) + " " + channel->dump();

    switch (status) {
        case LINKJOB:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + " PID: " + toString(child_pid);
        case TOINSTALL:
        case WAITINSTALL:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + " PID: " + toString(child_pid);
        case WAITFORCHILD:
            return ret + " ClientID: " + toString(client_id) + " PID: " + toString(child_pid) + " PFD: " + toString(pipe_from_child);
        case WAITCREATEENV:
            return ret + " " + toString(client_id) + " " + pending_create_env;
        default:

            if (job_id) {
                std::string jobs;

                if (usecsmsg) {
                    jobs = " CompileServer: " + usecsmsg->hostname;
                }

                return ret + " ClientID: " + toString(client_id) + " Job ID: " + toString(job_id) + jobs;
            } else {
                return ret + " ClientID: " + toString(client_id);
            }
    }

    return ret;
}