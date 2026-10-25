#include "rootless_instance.h"
#include "../utils/vine_utils.h"
#include "../display/vinefb.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vine::rootless {

namespace {

const char* const kStateNames[] = {"stopped", "booting", "running", "error", "paused"};
const int64_t kBinderdStartMs = 10000;

// Android's default PATH (init sets it, init.environ.rc does not)
const char* const kGuestPath =
    "/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:"
    "/odm/bin:/vendor/bin:/vendor/xbin";

std::vector<char*> cstrings(const std::vector<std::string>& v) {
    std::vector<char*> out;
    for (const auto& s : v) out.push_back(const_cast<char*>(s.c_str()));
    out.push_back(nullptr);
    return out;
}

// Every fd but 0-2 and `keep`, for a forked child that is not about to exec
// (and before exec too: the helper's pipes must not leak into guests)
void close_other_fds(int keep = -1) {
    std::vector<int> fds;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (dirent* e = readdir(d)) {
            const int fd = atoi(e->d_name);
            if (fd > 2 && fd != keep && fd != dirfd(d)) fds.push_back(fd);
        }
        closedir(d);
    }
    for (int fd : fds) close(fd);
}

// stdin from /dev/null, stdout and stderr to `out_fd`
void redirect_stdio(int out_fd) {
    const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null >= 0) dup2(null, 0);
    if (out_fd >= 0) {
        dup2(out_fd, 1);
        dup2(out_fd, 2);
    }
}

int open_log(const std::string& path, bool truncate) {
    return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | (truncate ? O_TRUNC : 0), 0644);
}

// A host process of the instance: dies with the helper
pid_t spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log_path) {
    const int log = open_log(log_path, true);
    const pid_t pid = fork();
    if (pid != 0) {
        if (log >= 0) close(log);
        return pid;
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    redirect_stdio(log);
    close_other_fds();
    if (chdir(cwd.c_str()) != 0) _exit(126);
    auto args = cstrings(argv);
    execve(args[0], args.data(), environ);
    fprintf(stderr, "exec %s: %s\n", args[0], strerror(errno));
    _exit(127);
}

pid_t parent_of(pid_t pid) {
    const auto stat = read_file_prefix("/proc/" + std::to_string(pid) + "/stat", 1024);
    if (!stat) return -1;
    const size_t close_paren = stat->rfind(')');
    if (close_paren == std::string::npos) return -1;
    int ppid = -1;
    char state = 0;
    return sscanf(stat->c_str() + close_paren + 1, " %c %d", &state, &ppid) == 2 ? ppid : -1;
}

std::vector<pid_t> all_pids() {
    std::vector<pid_t> pids;
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) {
            const pid_t p = atoi(e->d_name);
            if (p > 0) pids.push_back(p);
        }
        closedir(d);
    }
    return pids;
}

std::vector<pid_t> children_of(pid_t parent) {
    std::vector<pid_t> out;
    for (pid_t p : all_pids())
        if (parent_of(p) == parent) out.push_back(p);
    return out;
}

// Every process below `root` right now
std::vector<pid_t> descendants_of(pid_t root) {
    std::vector<std::pair<pid_t, pid_t>> links;
    for (pid_t p : all_pids()) links.emplace_back(p, parent_of(p));
    std::vector<pid_t> out, frontier{root};
    while (!frontier.empty()) {
        const pid_t parent = frontier.back();
        frontier.pop_back();
        for (const auto& [child, ppid] : links) {
            if (ppid == parent) {
                out.push_back(child);
                frontier.push_back(child);
            }
        }
    }
    return out;
}

// As a subreaper: kill every child until none is left (orphaned
// grandchildren become children as their parents die)
void kill_own_tree() {
    const int64_t deadline = monotonic_ms() + 5000;
    while (monotonic_ms() < deadline) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }
        const auto kids = children_of(getpid());
        if (kids.empty()) {
            if (waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD) return;
        }
        for (pid_t k : kids) kill(k, SIGKILL);
        usleep(10000);
    }
}

// The keeper: subreaper for init and everything it starts. Exits with
// init's status (128 + signal when killed) after killing the rest, or
// with 0 after a SIGTERM.
[[noreturn]] void keeper_main(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                              const std::string& cwd, int log, int report_fd) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    redirect_stdio(log);
    close_other_fds(report_fd);
    sigset_t set;
    sigemptyset(&set);
    for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&set, s);
    sigprocmask(SIG_BLOCK, &set, nullptr);

    const pid_t init = fork();
    if (init == 0) {
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
        close(report_fd);
        if (chdir(cwd.c_str()) != 0) _exit(126);
        auto args = cstrings(argv);
        auto envp = cstrings(env);
        execve(args[0], args.data(), envp.data());
        fprintf(stderr, "exec %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    (void)!write(report_fd, &init, sizeof init);
    close(report_fd);
    if (init < 0) _exit(125);
    for (;;) {
        siginfo_t info{};
        if (sigwaitinfo(&set, &info) < 0) continue;
        if (info.si_signo != SIGCHLD) {
            kill_own_tree();
            _exit(0);
        }
        int st = 0;
        for (pid_t w; (w = waitpid(-1, &st, WNOHANG)) > 0;) {
            if (w != init) continue;
            const int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            fprintf(stderr, "--- init exited: %d\n", code);
            kill_own_tree();
            _exit(code);
        }
    }
}

// Waits up to timeout_ms for pid, SIGKILLs it after that
void reap(pid_t& pid, int signal_first, int timeout_ms) {
    if (pid <= 0) return;
    if (signal_first) kill(pid, signal_first);
    const int64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        const pid_t w = waitpid(pid, nullptr, WNOHANG);
        if (w == pid || (w < 0 && errno == ECHILD)) break;
        if (monotonic_ms() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            break;
        }
        usleep(20000);
    }
    pid = -1;
}

// True once pid has exited (reaped), with its exit code
bool exited(pid_t pid, int* code) {
    if (pid <= 0) return false;
    int st = 0;
    const pid_t w = waitpid(pid, &st, WNOHANG);
    if (w != pid) return false;
    if (code) *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return true;
}

// The last `lines` lines of a file, NULs (kmsg records) as line breaks
std::string tail_file(const std::string& path, size_t lines, size_t max_bytes = 65536) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    struct stat st{};
    fstat(fd, &st);
    const off_t from = st.st_size > (off_t)max_bytes ? st.st_size - (off_t)max_bytes : 0;
    std::string text(st.st_size - from, '\0');
    const ssize_t n = pread(fd, text.data(), text.size(), from);
    close(fd);
    text.resize(n > 0 ? (size_t)n : 0);
    std::replace(text.begin(), text.end(), '\0', '\n');
    auto all = split_lines(text);
    all.erase(std::remove(all.begin(), all.end(), std::string()), all.end());
    const size_t first = all.size() > lines ? all.size() - lines : 0;
    std::string out;
    for (size_t i = first; i < all.size(); ++i) out += all[i] + "\n";
    return out;
}

} // namespace

const char* state_name(Instance::State s) { return kStateNames[(int)s]; }

Instance::Instance(InstanceConfig cfg) : cfg_(std::move(cfg)), layout_(layout_for(cfg_.path)) {}

Instance::~Instance() { teardown(); }

bool Instance::start(std::string* error) {
    if (active()) {
        if (error) *error = "instance is already running";
        return false;
    }
    for (const char* tool : {"libtawcroot.so", "libbinderd.so", "libvine_wl.so", "libvine_pulse.so", "libvine_lmkd.so"}) {
        if (!is_regular_file(cfg_.lib_dir + "/" + tool)) {
            if (error) *error = std::string("this build has no ") + tool;
            return false;
        }
    }
    for (const char* tool : {"libvine_kumquat.so", "libgfxstream_backend.so", "libvine_vk_gfxstream.so"}) {
        if (cfg_.gpu && !is_regular_file(cfg_.lib_dir + "/" + tool)) {
            if (error) *error = "this build has no GPU acceleration";
            return false;
        }
    }
    if (!mkdirs(layout_.logs)) {
        if (error) *error = "could not create " + layout_.logs;
        return false;
    }
    error_.clear();
    booted_ = false;
    const std::string log_path = layout_.logs + "/prepare.log";
    const int log = open_log(log_path, true);
    prep_pid_ = fork();
    if (prep_pid_ == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        redirect_stdio(log);
        close_other_fds();
        PrepOptions o;
        o.lib_dir = cfg_.lib_dir;
        o.dpi = cfg_.dpi;
        o.host_has_32bit = cfg_.host_has_32bit;
        o.gpu = cfg_.gpu;
        std::string why;
        const bool ok = extract_rootfs(layout_, &why) && prepare_rootfs(layout_, o, &why);
        if (!ok) fprintf(stderr, "%s\n", why.c_str());
        _exit(ok ? 0 : 1);
    }
    if (log >= 0) close(log);
    if (prep_pid_ < 0) {
        if (error) *error = std::string("fork: ") + strerror(errno);
        return false;
    }
    state_ = State::Booting;
    phase_ = Phase::Preparing;
    phase_since_ms_ = monotonic_ms();
    return true;
}

bool Instance::launch_services(std::string* error) {
    const std::string lib = cfg_.lib_dir + "/";
    const std::string& W = layout_.work;
    // Paths relative to W: the app data dir leaves no room in sun_path
    binderd_pid_ = spawn({lib + "libbinderd.so", "binderfs"}, W, layout_.logs + "/binderd.log");
    wl_pid_ = spawn({lib + "libvine_wl.so", "--socket", "run/wayland-0", "--vinefb", layout_.vinefb, "--create",
                     std::to_string(cfg_.width) + "x" + std::to_string(cfg_.height), "--input", "input"},
                    W, layout_.logs + "/vine_wl.log");
    pulse_pid_ = spawn({lib + "libvine_pulse.so", "--socket", "run/pulse/native"}, W, layout_.logs + "/vine_pulse.log");
    // The guest's GPU and gralloc service, its sockets in /run/user/1000
    if (cfg_.gpu) {
        kumquat_pid_ = spawn({lib + "libvine_kumquat.so", "kumquat-gpu-0", "vine-gralloc",
                              vine::display::gpu_ahb_socket(layout_.vinefb)}, layout_.run,
                             layout_.logs + "/vine_kumquat.log");
    }
    if (binderd_pid_ < 0 || wl_pid_ < 0 || pulse_pid_ < 0 || (cfg_.gpu && kumquat_pid_ < 0)) {
        if (error) *error = std::string("fork: ") + strerror(errno);
        return false;
    }
    return true;
}

static std::vector<std::string> binds(const Layout& l) {
    std::vector<std::string> out;
    for (const auto& b : std::vector<std::string>{
             l.binderfs + ":/dev/binderfs", l.data + ":/data", "/proc:/proc", "/sys:/sys", "/dev/null:/dev/null",
             "/dev/zero:/dev/zero", "/dev/random:/dev/random", "/dev/urandom:/dev/urandom", "/dev/ptmx:/dev/ptmx",
             "/dev/pts:/dev/pts", l.run + ":/run/user/1000"}) {
        out.push_back("-b");
        out.push_back(b);
    }
    return out;
}

std::vector<std::string> Instance::guest_env() const {
    std::vector<std::string> env;
    bool has_path = false;
    for (const auto& [k, v] : parse_environ_rc(read_file(layout_.rootfs + "/init.environ.rc").value_or(""))) {
        env.push_back(k + "=" + v);
        has_path = has_path || k == "PATH";
    }
    if (!has_path) env.push_back(std::string("PATH=") + kGuestPath);
    return env;
}

bool Instance::launch_init(std::string* error) {
    std::vector<std::string> argv = {cfg_.lib_dir + "/libtawcroot.so", "-r", layout_.rootfs, "--init", "--all-caps", "--fake-net", "--cgroup", "--owners"};
    for (const auto& b : binds(layout_)) argv.push_back(b);
    for (const char* a : {"--", "/system/bin/init", "second_stage"}) argv.push_back(a);
    const auto env = guest_env();

    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0) {
        if (error) *error = std::string("pipe: ") + strerror(errno);
        return false;
    }
    const int log = open_log(layout_.logs + "/init.log", true);
    keeper_pid_ = fork();
    if (keeper_pid_ == 0) {
        close(report[0]);
        keeper_main(argv, env, layout_.rootfs, log, report[1]); // init (and every service it forks) starts at the guest's /
    }
    close(report[1]);
    if (log >= 0) close(log);
    if (keeper_pid_ < 0) {
        close(report[0]);
        if (error) *error = std::string("fork: ") + strerror(errno);
        return false;
    }
    pid_t init = -1;
    const bool got = read(report[0], &init, sizeof init) == (ssize_t)sizeof init;
    close(report[0]);
    init_pid_ = got ? init : -1;
    return true;
}

void Instance::poll() {
    for (auto it = detached_.begin(); it != detached_.end();) {
        if (exited(*it, nullptr)) it = detached_.erase(it);
        else ++it;
    }
    if (!active()) return;
    int code = 0;
    std::string why;
    switch (phase_) {
    case Phase::Preparing:
        if (!exited(prep_pid_, &code)) return;
        prep_pid_ = -1;
        if (code != 0) {
            const std::string msg = tail_file(layout_.logs + "/prepare.log", 1);
            return fail("Preparing the rootfs failed: " + (msg.empty() ? "exit " + std::to_string(code) : msg.substr(0, msg.size() - 1)));
        }
        if (!launch_services(&why)) return fail(why);
        phase_ = Phase::WaitBinderd;
        phase_since_ms_ = monotonic_ms();
        return;
    case Phase::WaitBinderd:
        if (exited(binderd_pid_, &code)) {
            binderd_pid_ = -1;
            return fail("binderd exited (" + std::to_string(code) + ")");
        }
        if (!path_exists(layout_.binderfs + "/ready")) {
            if (monotonic_ms() - phase_since_ms_ > kBinderdStartMs) fail("binderd did not start");
            return;
        }
        if (!launch_init(&why)) return fail(why);
        phase_ = Phase::Booting;
        phase_since_ms_ = monotonic_ms();
        return;
    case Phase::Booting:
    case Phase::Up:
        break;
    case Phase::Idle:
        return;
    }
    if (exited(keeper_pid_, &code)) {
        keeper_pid_ = -1;
        return fail("Android stopped (init exited with " + std::to_string(code) + ")");
    }
    if (exited(binderd_pid_, &code)) {
        binderd_pid_ = -1;
        return fail("binderd exited (" + std::to_string(code) + ")");
    }
    if (exited(wl_pid_, &code)) {
        wl_pid_ = -1;
        return fail("The display server exited (" + std::to_string(code) + ")");
    }
    if (exited(kumquat_pid_, &code)) {
        kumquat_pid_ = -1;
        return fail("The GPU server exited (" + std::to_string(code) + ")");
    }
    if (exited(pulse_pid_, nullptr)) pulse_pid_ = -1; // the guest goes on without sound
    if (phase_ == Phase::Booting) {
        if (path_exists(layout_.run + "/boot_completed")) {
            booted_ = true;
            phase_ = Phase::Up;
            if (state_ == State::Booting) state_ = State::Running;
        } else if (monotonic_ms() - phase_since_ms_ > (int64_t)cfg_.boot_timeout_s * 1000) {
            fail("Android did not finish booting in " + std::to_string(cfg_.boot_timeout_s) + " s");
        }
    }
}

void Instance::fail(const std::string& why) {
    teardown();
    error_ = why;
    state_ = State::Error;
}

void Instance::teardown() {
    if (state_ == State::Paused) resume();
    reap(prep_pid_, SIGKILL, 1000);
    reap(keeper_pid_, SIGTERM, 8000);
    reap(binderd_pid_, SIGTERM, 2000);
    reap(kumquat_pid_, SIGTERM, 2000);
    reap(wl_pid_, SIGTERM, 2000);
    reap(pulse_pid_, SIGTERM, 2000);
    for (pid_t& p : detached_) reap(p, SIGKILL, 1000);
    detached_.clear();
    if (input_fd_ >= 0) close(input_fd_);
    input_fd_ = -1;
    init_pid_ = -1;
    phase_ = Phase::Idle;
}

void Instance::stop() {
    teardown();
    error_.clear();
    state_ = State::Stopped;
}

bool Instance::pause() {
    if (state_ != State::Running || keeper_pid_ <= 0) return false;
    // Twice: a process forked during the first pass is caught by the second
    for (int pass = 0; pass < 2; ++pass)
        for (pid_t p : descendants_of(keeper_pid_)) kill(p, SIGSTOP);
    state_ = State::Paused;
    return true;
}

bool Instance::resume() {
    if (state_ != State::Paused) return false;
    if (keeper_pid_ > 0)
        for (pid_t p : descendants_of(keeper_pid_)) kill(p, SIGCONT);
    state_ = State::Running;
    return true;
}

std::vector<std::string> Instance::exec_argv(const std::vector<std::string>& argv) const {
    std::vector<std::string> out = {cfg_.lib_dir + "/libtawcroot.so", "-r", layout_.rootfs, "--owners"};
    for (const auto& b : binds(layout_)) out.push_back(b);
    for (const char* a : {"--", "/system/bin/sh", "-c", "exec \"$@\"", "sh"}) out.push_back(a);
    out.insert(out.end(), argv.begin(), argv.end());
    return out;
}

ExecResult Instance::exec(const std::vector<std::string>& argv, int timeout_ms) const {
    ExecResult r;
    if (phase_ != Phase::Booting && phase_ != Phase::Up) {
        r.error = "instance is not running";
        return r;
    }
    const auto args = exec_argv(argv);
    const auto env = guest_env();
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
        r.error = std::string("pipe: ") + strerror(errno);
        return r;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        redirect_stdio(out[1]);
        close_other_fds();
        if (chdir(layout_.rootfs.c_str()) != 0) _exit(126);
        auto a = cstrings(args);
        auto e = cstrings(env);
        execve(a[0], a.data(), e.data());
        _exit(127);
    }
    close(out[1]);
    if (pid < 0) {
        close(out[0]);
        r.error = std::string("fork: ") + strerror(errno);
        return r;
    }
    const int64_t deadline = monotonic_ms() + timeout_ms;
    char buf[4096];
    for (;;) {
        const int64_t left = deadline - monotonic_ms();
        if (left <= 0) {
            r.timed_out = true;
            break;
        }
        pollfd p{out[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, (int)std::min<int64_t>(left, 1000));
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        const ssize_t n = read(out[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (r.output.size() < (1u << 20)) r.output.append(buf, (size_t)n);
    }
    close(out[0]);
    if (r.timed_out) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    r.exit_code = r.timed_out ? -1 : (WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return r;
}

void Instance::exec_detached(const std::vector<std::string>& argv) {
    if (phase_ != Phase::Booting && phase_ != Phase::Up) return;
    const auto args = exec_argv(argv);
    const auto env = guest_env();
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        redirect_stdio(-1);
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 1);
            dup2(null, 2);
        }
        close_other_fds();
        if (chdir(layout_.rootfs.c_str()) != 0) _exit(126);
        auto a = cstrings(args);
        auto e = cstrings(env);
        execve(a[0], a.data(), e.data());
        _exit(127);
    }
    if (pid > 0) detached_.push_back(pid);
}

bool Instance::send_input(const std::string& line) {
    if (phase_ != Phase::Booting && phase_ != Phase::Up) return false;
    const std::string msg = line + "\n";
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (input_fd_ < 0) {
            // Through a dir fd: the socket's full path does not fit in sun_path
            const int dir = open(layout_.work.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
            if (dir < 0) return false;
            sockaddr_un sa{};
            sa.sun_family = AF_UNIX;
            snprintf(sa.sun_path, sizeof sa.sun_path, "/proc/self/fd/%d/input", dir);
            const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            const bool ok = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
            close(dir);
            if (!ok) {
                if (fd >= 0) close(fd);
                return false;
            }
            timeval tv{0, 200000};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            input_fd_ = fd;
        }
        if (send(input_fd_, msg.data(), msg.size(), MSG_NOSIGNAL) == (ssize_t)msg.size()) return true;
        close(input_fd_);
        input_fd_ = -1;
    }
    return false;
}

std::string Instance::diagnostics() const {
    std::string out = std::string("Engine: rootless (tawcroot)\nState: ") + state_name(state_) + (booted_ ? ", booted" : "") + "\n";
    if (!error_.empty()) out += "Error: " + error_ + "\n";
    out += "Instance: " + layout_.work + "\n";
    const std::pair<const char*, size_t> logs[] = {
        {"prepare.log", 5}, {"init.log", 30}, {"binderd.log", 10}, {"vine_wl.log", 10}, {"vine_pulse.log", 5}, {"vine_kumquat.log", 10}};
    for (const auto& [name, lines] : logs) {
        const std::string t = tail_file(layout_.logs + "/" + name, lines);
        if (!t.empty()) out += std::string("\n--- ") + name + "\n" + t;
    }
    const std::string kmsg = tail_file(layout_.rootfs + "/dev/kmsg", 40);
    if (!kmsg.empty()) out += "\n--- kmsg\n" + kmsg;
    if (booted_ && state_ == State::Running) {
        const ExecResult r = exec({"logcat", "-d", "-t", "80", "-v", "brief"}, 8000);
        if (!r.output.empty()) out += "\n--- logcat\n" + r.output;
    }
    return out;
}

} // namespace vine::rootless
