#include "vine_utils.h"
#include "vine_log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/system_properties.h>

namespace vine {

bool mkdirs(const std::string& path, mode_t mode) {
    if (path.empty()) return false;
    if (path_exists(path)) return is_directory(path);

    std::string current;
    for (char ch : path) {
        current += ch;
        if (ch == '/' && current.size() > 1) {
            if (mkdir(current.c_str(), mode) != 0 && errno != EEXIST) {
                VINE_LOGE_ERRNO(("mkdirs: mkdir(" + current + ")").c_str());
                return false;
            }
        }
    }
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        VINE_LOGE_ERRNO(("mkdirs: final mkdir(" + path + ")").c_str());
        return false;
    }
    return true;
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_regular_file(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::optional<std::string> read_file_prefix(const std::string& path, size_t max_bytes) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string out;
    out.resize(max_bytes);
    size_t total = 0;
    while (total < max_bytes) {
        ssize_t n = read(fd, &out[total], max_bytes - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    out.resize(total);
    return out;
}

static bool write_all(int fd, const char* data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

bool write_file(const std::string& path, const std::string& contents, mode_t mode) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        VINE_LOGE_ERRNO(("write_file: open(" + path + ")").c_str());
        return false;
    }
    bool ok = write_all(fd, contents.data(), contents.size());
    close(fd);
    return ok;
}

bool write_file_atomic(const std::string& path, const std::string& contents, mode_t mode) {
    const std::string tmp = path + ".tmp." + std::to_string((long)getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        VINE_LOGE_ERRNO(("write_file_atomic: open(" + tmp + ")").c_str());
        return false;
    }
    bool ok = write_all(fd, contents.data(), contents.size());
    if (ok) fchmod(fd, mode);
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool append_file(const std::string& path, const std::string& contents) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = write_all(fd, contents.data(), contents.size());
    close(fd);
    return ok;
}

int64_t file_size(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
}

bool create_sparse_file(const std::string& path, uint64_t size_bytes) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        VINE_LOGE_ERRNO(("create_sparse_file: open(" + path + ")").c_str());
        return false;
    }
    bool ok = ftruncate(fd, (off_t)size_bytes) == 0;
    if (!ok) VINE_LOGE_ERRNO("create_sparse_file: ftruncate");
    close(fd);
    return ok;
}

bool copy_file_sparse(const std::string& src, const std::string& dst,
                      const std::function<void(uint64_t)>& progress) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) { VINE_LOGE_ERRNO(("copy: open(" + src + ")").c_str()); return false; }
    struct stat st{};
    if (fstat(in, &st) != 0) { close(in); return false; }

    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { VINE_LOGE_ERRNO(("copy: open(" + dst + ")").c_str()); close(in); return false; }

    const off_t total = st.st_size;
    std::vector<char> buf(1 << 20);
    off_t pos = 0;
    uint64_t copied = 0;
    bool ok = true;

    while (pos < total && ok) {
        off_t data = lseek(in, pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) break;                  // only a hole remains
            if (errno == EINVAL || errno == EOPNOTSUPP) { // filesystem can't report holes, copy everything
                data = pos;
                lseek(in, pos, SEEK_SET);
            } else { ok = false; break; }
        }
        off_t hole = lseek(in, data, SEEK_HOLE);
        if (hole < 0) hole = total;
        if (lseek(in, data, SEEK_SET) < 0 || lseek(out, data, SEEK_SET) < 0) { ok = false; break; }

        off_t remaining = hole - data;
        while (remaining > 0) {
            ssize_t want = (ssize_t)std::min<off_t>(remaining, (off_t)buf.size());
            ssize_t n = read(in, buf.data(), (size_t)want);
            if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
            if (n == 0) { remaining = 0; break; }
            if (!write_all(out, buf.data(), (size_t)n)) { ok = false; break; }
            remaining -= n;
            copied += (uint64_t)n;
            if (progress) progress(copied);
        }
        pos = hole;
    }

    if (ok && ftruncate(out, total) != 0) ok = false; // restores trailing hole and the exact apparent size
    close(in);
    if (close(out) != 0) ok = false;
    if (!ok) unlink(dst.c_str());
    return ok;
}

static bool remove_tree(const std::string& path, dev_t root_dev, bool keep_root) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;

    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0 || errno == ENOENT;
    if (st.st_dev != root_dev) return true; // never descend into another filesystem (e.g. a stale mount)

    DIR* d = opendir(path.c_str());
    if (!d) return false;
    bool ok = true;
    while (dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!remove_tree(path + "/" + e->d_name, root_dev, false)) ok = false;
    }
    closedir(d);
    if (!keep_root && ok) ok = rmdir(path.c_str()) == 0 || errno == ENOENT;
    return ok;
}

bool remove_recursive(const std::string& path) {
    if (path.empty() || path == "/") return false;
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    return remove_tree(path, st.st_dev, false);
}

bool clear_directory(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    return remove_tree(path, st.st_dev, true);
}

static pid_t do_fork_exec(const std::vector<std::string>& args, int stdout_fd = -1) {
    if (args.empty()) return -1;
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { VINE_LOGE_ERRNO("fork"); return -1; }
    if (pid == 0) {
        if (stdout_fd >= 0) {
            dup2(stdout_fd, STDOUT_FILENO);
            dup2(stdout_fd, STDERR_FILENO);
            close(stdout_fd);
        }
        execv(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    return pid;
}

int exec_wait(const std::vector<std::string>& args) {
    pid_t pid = do_fork_exec(args);
    if (pid < 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

pid_t exec_async(const std::vector<std::string>& args) {
    return do_fork_exec(args);
}

int exec_capture(const std::vector<std::string>& args, std::string* output, int timeout_ms, size_t max_bytes) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    pid_t pid = do_fork_exec(args, fds[1]);
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }

    const int64_t deadline = monotonic_ms() + timeout_ms;
    std::string out;
    bool timed_out = false;
    char buf[4096];
    for (;;) {
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) { timed_out = true; break; }
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        int r = poll(&pfd, 1, (int)std::min<int64_t>(left, 1000));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        if (out.size() < max_bytes) out.append(buf, (size_t)std::min<size_t>((size_t)n, max_bytes - out.size()));
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (output) *output = std::move(out);
    if (timed_out) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool terminate_gracefully(pid_t pid, int timeout_ms) {
    if (pid <= 0) return false;
    if (kill(pid, 0) != 0) return true;
    if (kill(pid, SIGTERM) != 0) { VINE_LOGE_ERRNO("SIGTERM"); return false; }

    const int poll_ms = 100;
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += poll_ms) {
        usleep(poll_ms * 1000);
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) return true;
    }
    VINE_LOGW("pid %d didn't exit in %dms, sending SIGKILL", pid, timeout_ms);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

// AArch32 detection, most to least reliable: abilist32 empty on arm64-only SoCs, abilist containing "armeabi", /proc/sys/abi/ compat knobs (CONFIG_COMPAT=y), cpuinfo aarch32_el0 flag (Linux 4.7+); all four negative means arm64-only and QEMU mode is required
bool host_supports_aarch32() {
    char buf[PROP_VALUE_MAX] = {};

    __system_property_get("ro.product.cpu.abilist32", buf);
    if (buf[0] != '\0') {
        VINE_LOGI("AArch32: L1 abilist32='%s' → supported", buf);
        return true;
    }

    __system_property_get("ro.product.cpu.abilist", buf);
    if (strstr(buf, "armeabi") != nullptr) {
        VINE_LOGI("AArch32: L2 abilist='%s' → supported", buf);
        return true;
    }

    static const char* kCompatKnobs[] = {
        "/proc/sys/abi/cp15_barrier_emulation",
        "/proc/sys/abi/setend_emulation",
        "/proc/sys/abi/swp_emulation",
    };
    for (const char* f : kCompatKnobs) {
        if (path_exists(f)) {
            VINE_LOGI("AArch32: L3 found %s → supported", f);
            return true;
        }
    }

    auto cpuinfo = read_file("/proc/cpuinfo");
    if (cpuinfo.has_value() && cpuinfo->find("aarch32_el0") != std::string::npos) {
        VINE_LOGI("AArch32: L4 aarch32_el0 in cpuinfo → supported");
        return true;
    }

    VINE_LOGI("AArch32: all layers negative → arm64-only SoC, QEMU required");
    return false;
}

std::vector<std::string> host_abi_list() {
    char abi_list[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.cpu.abilist", abi_list);
    std::vector<std::string> result;
    std::istringstream ss(abi_list);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

long available_ram_mb() {
    struct sysinfo info{};
    if (sysinfo(&info) != 0) { VINE_LOGE_ERRNO("sysinfo"); return -1; }
    return (long)(info.freeram * (unsigned long)info.mem_unit) / (1024 * 1024);
}

bool kernel_has_filesystem(const std::string& name) {
    auto fs = read_file("/proc/filesystems");
    if (!fs) return false;
    for (const auto& line : split_lines(*fs)) {
        // lines look like "nodev\tbinder" or "\text4"
        size_t tab = line.rfind('\t');
        std::string fsname = tab == std::string::npos ? line : line.substr(tab + 1);
        if (fsname == name) return true;
    }
    return false;
}

std::string find_executable(const std::string& name) {
    std::vector<std::string> dirs;
    if (const char* env = getenv("PATH")) {
        for (auto& d : split_fields(env, ':')) if (!d.empty()) dirs.push_back(d);
    }
    for (const char* d : {"/system/bin", "/system/xbin", "/vendor/bin", "/sbin", "/usr/sbin", "/usr/bin", "/bin"}) {
        dirs.push_back(d);
    }
    for (const auto& d : dirs) {
        const std::string candidate = path_join(d, name);
        if (access(candidate.c_str(), X_OK) == 0 && is_regular_file(candidate)) return candidate;
    }
    return "";
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::string escape_line(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape_line(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char n = text[++i];
            out += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::vector<std::string> split_fields(const std::string& text, char sep) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        size_t p = text.find(sep, start);
        if (p == std::string::npos) { fields.push_back(text.substr(start)); break; }
        fields.push_back(text.substr(start, p - start));
        start = p + 1;
    }
    return fields;
}

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const bool a_slash = a.back() == '/';
    const bool b_slash = b.front() == '/';
    if (a_slash && b_slash) return a + b.substr(1);
    if (a_slash || b_slash) return a + b;
    return a + "/" + b;
}

static std::vector<std::string> normalise_components(const std::string& path) {
    std::vector<std::string> parts;
    for (const auto& c : split_fields(path, '/')) {
        if (c.empty() || c == ".") continue;
        if (c == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(c);
    }
    return parts;
}

bool path_is_within(const std::string& path, const std::string& root) {
    if (path.empty() || path[0] != '/' || root.empty() || root[0] != '/') return false;
    const auto p = normalise_components(path);
    const auto r = normalise_components(root);
    if (p.size() < r.size()) return false;
    return std::equal(r.begin(), r.end(), p.begin());
}

int64_t monotonic_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

} // namespace vine
