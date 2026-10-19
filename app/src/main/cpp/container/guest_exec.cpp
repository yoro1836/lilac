#include "guest_exec.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vine {

namespace {

pid_t parent_of(pid_t pid) {
    auto stat = read_file("/proc/" + std::to_string(pid) + "/stat");
    if (!stat) return -1;
    const size_t close_paren = stat->rfind(')');
    if (close_paren == std::string::npos) return -1;
    // "... ) S ppid ..."
    const auto fields = split_fields(stat->substr(close_paren + 2), ' ');
    return fields.size() > 1 ? (pid_t)atoi(fields[1].c_str()) : -1;
}

// Resolves a bare command name against the PATH of the guest environment, inside the guest's root
std::string resolve_command(const std::string& name, const std::vector<std::string>& env) {
    if (name.find('/') != std::string::npos) return name;
    std::string path = "/system/bin:/system/xbin:/vendor/bin";
    for (const auto& v : env) {
        if (v.rfind("PATH=", 0) == 0) path = v.substr(5);
    }
    for (const auto& dir : split_fields(path, ':')) {
        const std::string candidate = dir + "/" + name;
        if (!dir.empty() && access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return name;
}

std::vector<std::string> default_environment() {
    return {"PATH=/sbin:/system/sbin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin", "ANDROID_ROOT=/system",
            "ANDROID_DATA=/data", "ANDROID_STORAGE=/storage", "TERM=linux"};
}

} // namespace

std::vector<std::string> guest_environment(pid_t init_pid) {
    DIR* d = opendir("/proc");
    if (!d) return default_environment();
    std::vector<std::string> best;
    while (dirent* e = readdir(d)) {
        const pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 1 || parent_of(pid) != init_pid) continue;
        auto env = read_file("/proc/" + std::to_string(pid) + "/environ");
        if (!env || env->empty()) continue;
        std::vector<std::string> vars;
        size_t start = 0;
        while (start < env->size()) {
            const size_t end = env->find('\0', start);
            const size_t stop = end == std::string::npos ? env->size() : end;
            if (stop > start) vars.push_back(env->substr(start, stop - start));
            start = stop + 1;
        }
        bool has_classpath = false;
        for (const auto& v : vars) has_classpath = has_classpath || v.rfind("BOOTCLASSPATH=", 0) == 0;
        if (has_classpath) {
            best = vars;
            break;
        }
        if (best.empty()) best = vars;
    }
    closedir(d);
    return best.empty() ? default_environment() : best;
}

bool guest_exec(pid_t init_pid, const std::vector<std::string>& args, int timeout_ms, GuestExecResult* result,
                const std::vector<std::pair<std::string, std::string>>& extra_env, size_t max_bytes) {
    GuestExecResult local;
    GuestExecResult& r = result ? *result : local;
    r = GuestExecResult{};
    if (init_pid <= 1 || args.empty()) {
        r.error = "no guest to run in";
        return false;
    }

    const std::string base = "/proc/" + std::to_string(init_pid);
    static const char* kNamespaces[] = {"ipc", "uts", "net", "pid", "mnt"};
    int ns_fds[5];
    for (int i = 0; i < 5; ++i) {
        ns_fds[i] = open((base + "/ns/" + kNamespaces[i]).c_str(), O_RDONLY | O_CLOEXEC);
        if (ns_fds[i] < 0) {
            r.error = std::string("cannot open ") + kNamespaces[i] + " namespace: " + strerror(errno);
            for (int j = 0; j < i; ++j) close(ns_fds[j]);
            return false;
        }
    }
    const int root_fd = open((base + "/root").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    auto close_fds = [&]() {
        for (int fd : ns_fds) close(fd);
        if (root_fd >= 0) close(root_fd);
    };
    if (root_fd < 0) {
        r.error = std::string("cannot open the guest root: ") + strerror(errno);
        close_fds();
        return false;
    }

    std::vector<std::string> env = guest_environment(init_pid);
    for (const auto& kv : extra_env) env.push_back(kv.first + "=" + kv.second);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
        r.error = std::string("pipe: ") + strerror(errno);
        close_fds();
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        r.error = std::string("fork: ") + strerror(errno);
        close(out[0]);
        close(out[1]);
        close_fds();
        return false;
    }
    if (child == 0) {
        setpgid(0, 0);
        for (int fd : ns_fds) {
            if (setns(fd, 0) != 0) {
                dprintf(out[1], "setns failed: %s\n", strerror(errno));
                _exit(126);
            }
        }
        if (fchdir(root_fd) != 0 || chroot(".") != 0 || chdir("/") != 0) {
            dprintf(out[1], "cannot enter the guest root: %s\n", strerror(errno));
            _exit(126);
        }
        // setns(pid) only affects children, so the command runs in a grandchild that is inside the guest's PID namespace
        const pid_t grandchild = fork();
        if (grandchild < 0) _exit(126);
        if (grandchild == 0) {
            const int null = open("/dev/null", O_RDONLY);
            if (null >= 0) dup2(null, 0);
            dup2(out[1], 1);
            dup2(out[1], 2);
            signal(SIGPIPE, SIG_DFL);
            const std::string program = resolve_command(args[0], env);
            execve(program.c_str(), argv.data(), envp.data());
            dprintf(2, "exec %s: %s\n", args[0].c_str(), strerror(errno));
            _exit(127);
        }
        int status = 0;
        waitpid(grandchild, &status, 0);
        if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
        _exit(128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
    }

    setpgid(child, child); // also done by the child, whichever runs first wins, so killing the group can't miss it
    close(out[1]);
    close_fds();
    const int64_t deadline = monotonic_ms() + timeout_ms;
    char buf[4096];
    bool eof = false;
    while (!eof) {
        const int left = (int)(deadline - monotonic_ms());
        if (left <= 0) {
            r.timed_out = true;
            break;
        }
        pollfd p{out[0], POLLIN, 0};
        const int pr = poll(&p, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) continue;
        const ssize_t n = read(out[0], buf, sizeof(buf));
        if (n <= 0) {
            eof = true;
        } else if (r.output.size() < max_bytes) {
            r.output.append(buf, std::min((size_t)n, max_bytes - r.output.size()));
        }
    }
    close(out[0]);
    if (r.timed_out) {
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
    }

    int status = 0;
    waitpid(child, &status, 0);
    if (r.timed_out) {
        r.exit_code = -1;
    } else if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.exit_code = 128 + WTERMSIG(status);
    }
    return !r.timed_out && r.error.empty();
}

} // namespace vine
