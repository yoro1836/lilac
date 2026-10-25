#pragma once

#include "rootless_prep.h"

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace vine::rootless {

struct InstanceConfig {
    std::string id;
    std::string path;            // the instance dir
    std::string lib_dir;         // libtawcroot.so, libbinderd.so, libvine_wl.so, ...
    int width = 720;
    int height = 1440;
    int dpi = 320;
    int refresh_hz = 60;         // the guest display's frame rate
    int ram_mb = 0;              // vine_lmkd's budget for the guest's apps, 0: none
    int boot_timeout_s = 900;    // first boots compile every app
    bool host_has_32bit = false;
    bool gpu = false;            // graphics=gfxstream: vine_kumquat on the host GPU
};

struct ExecResult {
    int exit_code = -1;          // -1 when killed on timeout or not started
    bool timed_out = false;
    std::string output;          // stdout and stderr together
    std::string error;           // why it could not start
};

// One rootless guest: rootfs preparation in a child process, then binderd,
// vine_wl and vine_pulse, then tawcroot --init under a keeper process
// that is the subreaper of every guest process (zygotes outlive init), so
// stopping the instance kills exactly its own processes.
class Instance {
public:
    enum class State { Stopped, Booting, Running, Error, Paused };

    explicit Instance(InstanceConfig cfg);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    bool start(std::string* error);
    // Advances the boot and notices exits; never blocks
    void poll();
    void stop();
    bool pause();
    bool resume();

    State state() const { return state_; }
    bool active() const { return state_ == State::Booting || state_ == State::Running || state_ == State::Paused; }
    const std::string& error() const { return error_; }
    bool booted() const { return booted_; }
    pid_t init_pid() const { return init_pid_; }
    const InstanceConfig& config() const { return cfg_; }
    const Layout& layout() const { return layout_; }

    // Runs a command in the guest as root, through the guest's sh and PATH
    ExecResult exec(const std::vector<std::string>& argv, int timeout_ms) const;
    // The same without waiting; its output is dropped
    void exec_detached(const std::vector<std::string>& argv);
    // One input line for vine_wl (input_lines.h)
    bool send_input(const std::string& line);
    std::string diagnostics() const;

private:
    enum class Phase { Idle, Preparing, WaitBinderd, Booting, Up };

    void fail(const std::string& why);
    void teardown();
    bool launch_services(std::string* error);
    bool launch_init(std::string* error);
    std::vector<std::string> exec_argv(const std::vector<std::string>& argv) const;
    std::vector<std::string> guest_env() const;

    InstanceConfig cfg_;
    Layout layout_;
    State state_ = State::Stopped;
    Phase phase_ = Phase::Idle;
    std::string error_;
    bool booted_ = false;
    int64_t phase_since_ms_ = 0;
    pid_t prep_pid_ = -1, binderd_pid_ = -1, wl_pid_ = -1, pulse_pid_ = -1, kumquat_pid_ = -1, keeper_pid_ = -1, init_pid_ = -1;
    std::vector<pid_t> detached_;
    int input_fd_ = -1;
};

const char* state_name(Instance::State s);

} // namespace vine::rootless
