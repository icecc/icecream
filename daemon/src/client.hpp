#pragma once
#include <string>
#include <cstdint>
#include "services/comm.h"

class Client {
public:
    /*
     * UNKNOWN: Client was just created - not supposed to be long term
     * GOTNATIVE: Client asked us for the native env - this is the first step
     * PENDING_USE_CS: We have a CS from scheduler and need to tell the client
     *          as soon as there is a spot available on the local machine
     * JOBDONE: This was compiled by a local client and we got a jobdone - awaiting END
     * LINKJOB: This is a local job (aka link job) by a local client we told the scheduler about
     *          and await the finish of it
     * TOINSTALL: We're receiving an environment transfer and wait for it to complete.
     * WAITINSTALL: Client is waiting for the environment transfer unpacking child to finish.
     * TOCOMPILE: We're supposed to compile it ourselves
     * WAITFORCS: Client asked for a CS and we asked the scheduler - waiting for its answer
     * WAITCOMPILE: Client got a CS and will ask him now (it's not me)
     * CLIENTWORK: Client is busy working and we reserve the spot (job_id is set if it's a scheduler job)
     * WAITFORCHILD: Client is waiting for the compile job to finish.
     * WAITCREATEENV: We're waiting for icecc-create-env to finish.
     */
    enum Status { UNKNOWN, GOTNATIVE, PENDING_USE_CS, JOBDONE, LINKJOB, TOINSTALL, WAITINSTALL, TOCOMPILE,
        WAITFORCS, WAITCOMPILE, CLIENTWORK, WAITFORCHILD, WAITCREATEENV,
        LASTSTATE = WAITCREATEENV
    } status;
    Client();
    ~Client();

    static std::string status_str(Status status);


    uint32_t job_id;
    std::string outfile; // only useful for LINKJOB or TOINSTALL/WAITINSTALL
    MsgChannel *channel;
    UseCSMsg *usecsmsg;
    CompileJob *job;
    int client_id;
    // pipe from child process with end status, only valid if WAITFORCHILD or TOINSTALL/WAITINSTALL
    int pipe_from_child;
    // pipe to child process, only valid if TOINSTALL/WAITINSTALL
    int pipe_to_child;
    pid_t child_pid;
    std::string pending_create_env; // only for WAITCREATEENV

    std::string dump() const;
};