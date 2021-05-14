#pragma once
#include <map>
#include <string>
#include "clients.hpp"
#include <csignal>

struct NativeEnvironment {
    std::string name; // the hash
    // Timestamps for files including compiler binaries, if they have changed since the time
    // the native env was built, it needs to be rebuilt.
    std::map<std::string, time_t> filetimes;
    time_t last_use;
    size_t size; // tarball size
    int create_env_pipe; // if in progress of creating the environment
    NativeEnvironment() : last_use( 0 ), size( 0 ), create_env_pipe( 0 ) {}
};

struct ReceivedEnvironment {
    ReceivedEnvironment() : last_use( 0 ), size( 0 ) {}
    time_t last_use;
    size_t size; // directory size
};

class Daemon {
public:
    Daemon(int& mem_limit, unsigned int& max_kids, volatile const sig_atomic_t& exit_main_loop);
    ~Daemon();

private:
    int& mem_limit; // TODO: remove asap
    unsigned int& max_kids; // TODO: remove asap

public: // for now, a lot of usage of this class memebers...
    Clients clients;
    // Installed environments received from other nodes. The key is
    // (job->targetPlatform() + "/" job->environmentVersion()).
    std::map<std::string, ReceivedEnvironment> received_environments;
    // Map of native environments, the basic one(s) containing just the compiler
    // and possibly more containing additional files (such as compiler plugins).
    // The key is the compiler name and a concatenated list of the additional files
    // (or just the compiler name for the basic ones).
    std::map<std::string, NativeEnvironment> native_environments;
    std::string envbasedir;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno;
    int tcp_listen_fd;
    int tcp_listen_local_fd; // if tcp_listen is bound to a specific network interface, this one is bound to lo interface
    int unix_listen_fd;
    std::string machine_name;
    std::string nodename;
    bool noremote;
    bool custom_nodename;
    size_t cache_size;
    std::map<int, MsgChannel *> fd2chan;
    int new_client_id;
    std::string remote_name;
    time_t next_scheduler_connect;
    unsigned long icecream_load;
    struct timeval icecream_usage;
    int current_load;
    int num_cpus;
    MsgChannel *scheduler;
    DiscoverSched *discover;
    std::string netname;
    std::string schedname;
    int scheduler_port;
    std::string daemon_interface;
    int daemon_port;
    unsigned int supported_features;
    size_t cache_size_limit = 256 * 1024 * 1024;

    int max_scheduler_pong;
    int max_scheduler_ping;
    unsigned int current_kids;
    struct timeval last_stat;

    bool reannounce_environments() __attribute_warn_unused_result__;
    void answer_client_requests();
    bool handle_transfer_env(Client *client, EnvTransferMsg *msg) __attribute_warn_unused_result__;
    bool handle_env_install_child_done(Client *client);
    bool finish_transfer_env(Client *client, bool cancel = false);
    bool handle_get_native_env(Client *client, GetNativeEnvMsg *msg) __attribute_warn_unused_result__;
    bool finish_get_native_env(Client *client, std::string env_key);
    void handle_old_request();
    bool handle_compile_file(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_activity(Client *client) __attribute_warn_unused_result__;
    bool handle_file_chunk_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    void handle_end(Client *client, int exitcode);
    int scheduler_get_internals() __attribute_warn_unused_result__;
    void clear_children();
    int scheduler_use_cs(UseCSMsg *msg) __attribute_warn_unused_result__;
    int scheduler_no_cs(NoCSMsg *msg) __attribute_warn_unused_result__;
    bool handle_get_cs(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_local_job(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_job_done(Client *cl, JobDoneMsg *m) __attribute_warn_unused_result__;
    bool handle_compile_done(Client *client) __attribute_warn_unused_result__;
    bool handle_verify_env(Client *client, VerifyEnvMsg *msg) __attribute_warn_unused_result__;
    bool handle_blacklist_host_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    int handle_cs_conf(ConfCSMsg *msg);
    std::string dump_internals() const;
    std::string determine_nodename();
    void determine_system();
    void determine_supported_features();
    bool maybe_stats(bool force_check = false);
    bool send_scheduler(const Msg &msg) __attribute_warn_unused_result__;
    void close_scheduler();
    bool reconnect();
    int working_loop();
    bool setup_listen_fds();
    bool setup_listen_tcp_fd( int& fd, const std::string& interface );
    bool setup_listen_unix_fd();
    void check_cache_size(const std::string &new_env);
    void remove_native_environment(const std::string& env_key);
    void remove_environment(const std::string& env_key);
    bool create_env_finished(std::string env_key);

private:
    volatile const sig_atomic_t& exit_main_loop;
};