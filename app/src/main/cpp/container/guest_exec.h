#pragma once

#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>

namespace vine {

struct GuestExecResult {
    int exit_code = -1;     // -1 when the command could not be started or was killed on timeout
    bool timed_out = false;
    std::string output;     // stdout and stderr together, truncated to max_bytes
    std::string error;      // why the command could not be started, "" otherwise
};

// Runs a command inside a running guest by joining its namespaces and root, needs root
bool guest_exec(pid_t init_pid, const std::vector<std::string>& args, int timeout_ms, GuestExecResult* result,
                const std::vector<std::pair<std::string, std::string>>& extra_env = {}, size_t max_bytes = 1 << 20);

// "KEY=VALUE" strings for a process started by the guest's init, taken from one of its children when there is one.
std::vector<std::string> guest_environment(pid_t init_pid);

} // namespace vine
