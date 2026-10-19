#include "namespace_manager.h"
#include "boot_props.h"
#include "loop_mount.h"
#include "net_setup.h"
#include "ramdisk.h"
#include "sparse_image.h"
#include "../display/vinefb.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <poll.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vine {

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr size_t kInitScanBytes = 16 * 1024 * 1024;
constexpr int kSetupTimeoutMs = 30000;
// Exit codes the supervisor reports for a guest killed by a signal: 128 + signal number
constexpr int kExitSighup = 129;  // guest asked for a reboot
constexpr int kExitSigint = 130;  // guest asked for a power off
constexpr int kExitSigkill = 137;
constexpr int kExitSigterm = 143;

std::string safe_tag(const std::string& id) {
    std::string t;
    for (char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) t += c;
        if (t.size() == 8) break;
    }
    return t.empty() ? "unnamed" : t;
}

bool write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

std::string tail_file(const std::string& path, size_t max_bytes) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    off_t size = lseek(fd, 0, SEEK_END);
    off_t start = size > (off_t)max_bytes ? size - (off_t)max_bytes : 0;
    std::string out((size_t)(size - start), '\0');
    ssize_t n = pread(fd, &out[0], out.size(), start);
    close(fd);
    out.resize(n > 0 ? (size_t)n : 0);
    return out;
}

void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

// Waits for fd to become readable, returns false on timeout
bool wait_readable(int fd, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        return r > 0;
    }
}

bool run_tool(const std::vector<std::string>& args, std::string* out, int timeout_ms = 600000) {
    std::string buf;
    const int rc = exec_capture(args, &buf, timeout_ms, 8192);
    if (out) *out = buf;
    return rc == 0;
}

struct CgroupMount {
    std::string path;
    bool v2 = false;
};

// Finds a cgroup controller mount in /proc/self/mounts, v1 first then cgroup2
bool find_cgroup_mount(const std::string& controller, CgroupMount* out) {
    auto mounts = read_file("/proc/self/mounts");
    if (!mounts) return false;
    const auto lines = split_lines(*mounts);
    for (const auto& line : lines) {
        const auto f = split_fields(line, ' ');
        if (f.size() < 4 || f[2] != "cgroup") continue;
        for (const auto& opt : split_fields(f[3], ',')) {
            if (opt == controller) {
                out->path = f[1];
                out->v2 = false;
                return true;
            }
        }
    }
    for (const auto& line : lines) {
        const auto f = split_fields(line, ' ');
        if (f.size() < 4 || f[2] != "cgroup2") continue;
        const std::string ctl = read_file(path_join(f[1], "cgroup.controllers")).value_or("");
        if (controller == "freezer" ? path_exists(path_join(f[1], "cgroup.procs")) : ctl.find(controller) != std::string::npos) {
            out->path = f[1];
            out->v2 = true;
            return true;
        }
    }
    return false;
}

bool format_ext4(const std::string& tool, const std::string& image, uint64_t size_mb, std::string* error) {
    const std::string ratio = size_mb >= 32768 ? "65536" : "16384";
    std::string out;
    if (!run_tool({tool, "-t", "ext4", "-F", "-q", "-m", "0", "-b", "4096", "-L", "vine-data", "-i", ratio, "-E",
                   "lazy_itable_init=1,nodiscard", image}, &out)) {
        if (error) *error = "mke2fs failed: " + out.substr(0, 300);
        return false;
    }
    return true;
}

bool grow_ext4(const std::string& image, uint64_t want_bytes, std::string* error) {
    const int64_t current = file_size(image);
    if (current < 0 || (uint64_t)current >= want_bytes) return true;
    const std::string e2fsck = find_executable("e2fsck");
    const std::string resize2fs = find_executable("resize2fs");
    if (e2fsck.empty() || resize2fs.empty()) {
        if (error) *error = "e2fsck/resize2fs not found, keeping the current size";
        return false;
    }
    if (!create_sparse_file(image, want_bytes)) {
        if (error) *error = "could not extend " + image;
        return false;
    }
    std::string out;
    const int fsck = exec_capture({e2fsck, "-f", "-y", image}, &out, 600000, 4096);
    std::string out2;
    if ((fsck != 0 && fsck != 1) || !run_tool({resize2fs, image}, &out2)) {
        if (truncate(image.c_str(), current) != 0) VINE_LOGW("could not shrink %s back", image.c_str());
        if (error) *error = "resize2fs failed: " + (out2.empty() ? out : out2).substr(0, 300);
        return false;
    }
    return true;
}

struct LaunchSpec {
    const ContainerConfig* cfg = nullptr;
    bool data_is_image = true;
    bool has_vendor = false;
    bool isolate_net = true;
    NetPlan net;
    pid_t host_pid = 0;
};

struct SpawnFds {
    int err_r = -1, err_w = -1;
    int pid_r = -1, pid_w = -1;
    int go_r = -1, go_w = -1;
    int log = -1;
};

[[noreturn]] void child_die(int err_fd, const std::string& msg) {
    const std::string line = msg + "\n";
    if (err_fd >= 0) write_all(err_fd, line.data(), line.size());
    _exit(1);
}

std::string errno_text(const std::string& what) { return what + ": " + strerror(errno); }

bool touch_file(const std::string& path) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    close(fd);
    return true;
}

bool bind_mount(const std::string& src, const std::string& dst, bool read_only, std::string* error) {
    if (mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        if (error) *error = errno_text("bind " + src + " -> " + dst);
        return false;
    }
    if (read_only &&
        mount(nullptr, dst.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) {
        if (error) *error = errno_text("remount " + dst + " read-only");
        return false;
    }
    return true;
}

#ifndef BINDER_CTL_ADD
struct vine_binderfs_device {
    char name[256];
    uint32_t major;
    uint32_t minor;
};
#define VINE_BINDER_CTL_ADD _IOWR('b', 1, struct vine_binderfs_device)
#endif

// Gives the guest its own binder context. Returns a human readable note when there is none, "" when it worked.
std::string setup_guest_binder(const std::string& dev) {
    if (kernel_has_filesystem("binder")) {
        const std::string fs = dev + "/binderfs";
        mkdirs(fs);
        if (mount("binder", fs.c_str(), "binder", 0, nullptr) != 0) return errno_text("mount binderfs");
        int ctl = open((fs + "/binder-control").c_str(), O_RDWR | O_CLOEXEC);
        if (ctl < 0) return errno_text("open binder-control");
        for (const char* name : {"binder", "hwbinder", "vndbinder"}) {
            vine_binderfs_device d{};
            strncpy(d.name, name, sizeof(d.name) - 1);
            if (ioctl(ctl, VINE_BINDER_CTL_ADD, &d) != 0 && errno != EEXIST) {
                std::string note = errno_text(std::string("create ") + name);
                close(ctl);
                return note;
            }
            symlink((std::string("binderfs/") + name).c_str(), (dev + "/" + name).c_str());
        }
        close(ctl);
        return "";
    }
    // Kernels patched for Anbox expose extra binder devices that are not shared with the phone's own binder
    bool any = false;
    const char* pairs[][2] = {{"/dev/anbox-binder", "binder"}, {"/dev/anbox-hwbinder", "hwbinder"}, {"/dev/anbox-vndbinder", "vndbinder"}};
    for (const auto& p : pairs) {
        if (!path_exists(p[0])) continue;
        const std::string target = dev + "/" + p[1];
        if (touch_file(target) && mount(p[0], target.c_str(), nullptr, MS_BIND, nullptr) == 0) any = true;
    }
    return any ? "" : "the kernel has no binderfs and no extra binder devices, the guest has no private binder";
}

bool build_guest_root(const LaunchSpec& sp, const std::string& console_path, std::string* error) {
    const ContainerConfig& c = *sp.cfg;
    const std::string root = c.rootfs_dir;
    std::string e;

    if (mount(root.c_str(), root.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        *error = errno_text("bind rootfs");
        return false;
    }

    if (!mount_image(c.system_image, root + "/system", true, &e)) { *error = e; return false; }
    if (sp.has_vendor && !mount_image(c.vendor_image, root + "/vendor", true, &e)) { *error = e; return false; }
    if (sp.data_is_image) {
        if (!mount_image(c.data_image, root + "/data", false, &e)) { *error = e; return false; }
    } else if (!bind_mount(c.data_dir, root + "/data", false, &e)) {
        *error = e;
        return false;
    }
    if (!bind_mount(c.shared_dir, root + "/vine", false, &e)) { *error = e; return false; }
    if (mount("tmpfs", (root + "/cache").c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "size=64m,mode=0770") != 0) {
        *error = errno_text("mount /cache");
        return false;
    }

    // /proc comes from this PID namespace, so it only lists the guest. The sysctls and sysrq are the phone's own.
    if (mount("proc", (root + "/proc").c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) {
        *error = errno_text("mount /proc");
        return false;
    }
    for (const char* dir : {"/proc/sys", "/proc/irq", "/proc/bus"}) {
        if (path_exists(root + dir)) bind_mount(root + dir, root + dir, true, nullptr);
    }
    if (path_exists(root + "/proc/sysrq-trigger")) bind_mount(root + "/proc/sysrq-trigger", root + "/proc/sysrq-trigger", true, nullptr);
    for (const char* f : {"/proc/kcore", "/proc/kmsg"}) {
        if (path_exists(root + f)) mount("/dev/null", (root + f).c_str(), nullptr, MS_BIND, nullptr);
    }
    if (path_exists(c.shared_dir + "/cmdline") && path_exists(root + "/proc/cmdline")) {
        mount((c.shared_dir + "/cmdline").c_str(), (root + "/proc/cmdline").c_str(), nullptr, MS_BIND, nullptr);
    }

    if (mount("sysfs", (root + "/sys").c_str(), "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, nullptr) != 0) {
        *error = errno_text("mount /sys");
        return false;
    }

    const std::string dev = root + "/dev";
    if (mount("tmpfs", dev.c_str(), "tmpfs", MS_NOSUID | MS_NOATIME, "mode=0755,size=4m") != 0) {
        *error = errno_text("mount /dev");
        return false;
    }
    struct Node { const char* name; mode_t mode; unsigned maj, min; };
    const Node nodes[] = {
        {"null", 0666, 1, 3}, {"zero", 0666, 1, 5}, {"full", 0666, 1, 7}, {"random", 0666, 1, 8},
        {"urandom", 0666, 1, 9}, {"tty", 0666, 5, 0}, {"fuse", 0666, 10, 229},
    };
    for (const auto& n : nodes) {
        const std::string path = dev + "/" + n.name;
        if (mknod(path.c_str(), S_IFCHR | n.mode, makedev(n.maj, n.min)) != 0) {
            const std::string host = std::string("/dev/") + n.name;
            if (path_exists(host) && touch_file(path)) mount(host.c_str(), path.c_str(), nullptr, MS_BIND, nullptr);
        }
        chmod(path.c_str(), n.mode);
    }
    mkdirs(dev + "/pts");
    if (mount("devpts", (dev + "/pts").c_str(), "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620") == 0) {
        symlink("pts/ptmx", (dev + "/ptmx").c_str());
    }
    const std::string console = dev + "/console";
    touch_file(console);
    mount(console_path.empty() ? "/dev/null" : console_path.c_str(), console.c_str(), nullptr, MS_BIND, nullptr);
    if (path_exists("/dev/ashmem")) {
        touch_file(dev + "/ashmem");
        mount("/dev/ashmem", (dev + "/ashmem").c_str(), nullptr, MS_BIND, nullptr);
    }
    const std::string binder_note = setup_guest_binder(dev);
    if (!binder_note.empty()) dprintf(2, "vine: warning: %s\n", binder_note.c_str());
    return true;
}

bool enter_guest_root(const std::string& root) {
    const std::string old = root + "/.old_root";
    mkdir(old.c_str(), 0700);
    if (syscall(SYS_pivot_root, root.c_str(), old.c_str()) == 0) {
        if (chdir("/") != 0) return false;
        umount2("/.old_root", MNT_DETACH);
        rmdir("/.old_root");
        return true;
    }
    // Hosts whose own root is an initramfs can't pivot, move the new root over it instead
    if (chdir(root.c_str()) != 0) return false;
    if (mount(".", "/", nullptr, MS_MOVE, nullptr) != 0) return false;
    if (chroot(".") != 0) return false;
    return chdir("/") == 0;
}

[[noreturn]] void run_guest_init(const LaunchSpec& sp, const SpawnFds& fds, int console_fd, const std::string& console_path,
                                 pid_t supervisor) {
    const ContainerConfig& c = *sp.cfg;
    // getppid() is 0 here (the supervisor lives outside this PID namespace), so there is no parent check
    (void)supervisor;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    setsid();

    char go = 0;
    if (read(fds.go_r, &go, 1) != 1) child_die(fds.err_w, "the host went away before the guest could start");
    close(fds.go_r);
    close(fds.go_w);

    if (console_fd >= 0) {
        dup2(console_fd, 0);
        dup2(console_fd, 1);
        dup2(console_fd, 2);
    } else {
        const int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(fds.log, 1);
        dup2(fds.log, 2);
    }

    struct rlimit nice_limit = {20, 20};
    setrlimit(RLIMIT_NICE, &nice_limit);
    const std::string hostname = "vine-" + safe_tag(c.instance_id);
    sethostname(hostname.c_str(), hostname.size());

    if (sp.isolate_net) {
        std::string net_error;
        if (!configure_guest_network(sp.net, &net_error)) dprintf(2, "vine: warning: guest network: %s\n", net_error.c_str());
    }

    if (unshare(CLONE_NEWNS) != 0) child_die(fds.err_w, errno_text("unshare mount namespace"));
    if (mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) child_die(fds.err_w, errno_text("make mounts private"));

    std::string error;
    if (!build_guest_root(sp, console_path, &error)) child_die(fds.err_w, error);
    if (!enter_guest_root(c.rootfs_dir)) child_die(fds.err_w, errno_text("enter guest root"));

    // Everything inherited from the host (sockets, logs) must not leak into init
    DIR* d = opendir("/proc/self/fd");
    if (d) {
        const int self = dirfd(d);
        std::vector<int> close_list;
        while (dirent* e = readdir(d)) {
            const int fd = atoi(e->d_name);
            if (fd > 2 && fd != self && fd != fds.err_w) close_list.push_back(fd);
        }
        closedir(d);
        for (int fd : close_list) close(fd);
    }

    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    umask(0);

    const char* argv[] = {"/init", nullptr};
    const char* envp[] = {
        "PATH=/sbin:/system/sbin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin",
        "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "TERM=linux", nullptr,
    };
    execve("/init", const_cast<char* const*>(argv), const_cast<char* const*>(envp));
    child_die(fds.err_w, errno_text("exec /init"));
}

[[noreturn]] void run_supervisor(const LaunchSpec& sp, const SpawnFds& fds) {
    const ContainerConfig& c = *sp.cfg;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != sp.host_pid) _exit(1);
    setsid();

    // The guest's console is a pty, so init's output and the console service land in boot.log
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    int slave = -1;
    std::string slave_path;
    if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0) {
        char name[128];
        if (ptsname_r(master, name, sizeof(name)) == 0) {
            slave_path = name;
            slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        }
    }
    if (slave < 0) {
        if (master >= 0) close(master);
        master = -1;
        slave_path.clear();
    }

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC;
    if (sp.isolate_net) flags |= CLONE_NEWNET;
    if (unshare(flags) != 0) child_die(fds.err_w, errno_text("unshare"));

    const pid_t me = getpid();
    const pid_t guest = fork();
    if (guest < 0) child_die(fds.err_w, errno_text("fork"));
    if (guest == 0) run_guest_init(sp, fds, slave, slave_path, me);

    close(fds.err_w);
    close(fds.go_r);
    close(fds.go_w);
    const std::string pid_line = std::to_string(guest) + "\n";
    write_all(fds.pid_w, pid_line.data(), pid_line.size());
    close(fds.pid_w);
    (void)c;

    int status = 0;
    char buf[4096];
    for (;;) {
        if (master >= 0) {
            pollfd p{master, POLLIN, 0};
            if (poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) {
                ssize_t n = read(master, buf, sizeof(buf));
                if (n > 0) write_all(fds.log, buf, (size_t)n);
            }
        } else {
            sleep_ms(200);
        }
        const pid_t w = waitpid(guest, &status, WNOHANG);
        if (w == guest) break;
        if (w < 0 && errno != EINTR) break;
    }
    if (master >= 0) { // whatever init printed last
        int fl = fcntl(master, F_GETFL);
        fcntl(master, F_SETFL, fl | O_NONBLOCK);
        ssize_t n;
        while ((n = read(master, buf, sizeof(buf))) > 0) write_all(fds.log, buf, (size_t)n);
    }
    if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) _exit(128 + WTERMSIG(status));
    _exit(1);
}

int g_binfmt_refs = 0;
bool g_binfmt_ours = false;
const char* const kBinfmtDir = "/proc/sys/fs/binfmt_misc";
const char* const kBinfmtName = "vine-arm";

} // namespace

bool binfmt_arm_acquire(const std::string& qemu_path, std::string* error) {
    if (g_binfmt_refs > 0) {
        ++g_binfmt_refs;
        return true;
    }
    if (qemu_path.empty() || !path_exists(qemu_path)) {
        if (error) *error = "qemu-arm not found at '" + qemu_path + "'";
        return false;
    }
    const std::string dir = kBinfmtDir;
    if (!path_exists(dir + "/register") &&
        mount("binfmt_misc", dir.c_str(), "binfmt_misc", MS_NODEV | MS_NOEXEC | MS_NOSUID, nullptr) != 0) {
        if (error) *error = errno_text("mount binfmt_misc");
        return false;
    }
    const std::string entry = dir + "/" + kBinfmtName;
    if (path_exists(entry)) write_file(entry, "-1"); // stale rule from a crashed run

    // 'F' makes the kernel open the interpreter now, so it keeps working inside the guest's mount namespace
    const std::string rule =
        std::string(":") + kBinfmtName + ":M::"
        "\\x7fELF\\x01\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\x28\\x00"
        ":"
        "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff"
        ":" + qemu_path + ":F";
    if (!write_file(dir + "/register", rule, 0200)) {
        if (error) *error = errno_text("register binfmt rule");
        return false;
    }
    g_binfmt_refs = 1;
    g_binfmt_ours = true;
    VINE_LOGI("binfmt: 32-bit ARM now runs through %s", qemu_path.c_str());
    return true;
}

void binfmt_arm_release() {
    if (g_binfmt_refs == 0) return;
    if (--g_binfmt_refs == 0 && g_binfmt_ours) {
        write_file(std::string(kBinfmtDir) + "/" + kBinfmtName, "-1");
        g_binfmt_ours = false;
    }
}

ContainerConfig make_container_config(const std::string& instance_id, const std::string& instance_path) {
    ContainerConfig c;
    c.instance_id = instance_id;
    c.instance_path = instance_path;
    c.rootfs_dir = instance_path + "/rootfs";
    c.ramdisk_image = instance_path + "/ramdisk.img";
    c.system_image = instance_path + "/system.img";
    c.vendor_image = instance_path + "/vendor.img";
    c.data_image = instance_path + "/data.img";
    c.data_dir = instance_path + "/data";
    c.shared_dir = instance_path + "/shared";
    c.boot_log_path = instance_path + "/boot.log";
    return c;
}

Container::Container(ContainerConfig config) : config_(std::move(config)) {}

Container::~Container() {
    if (status_ == ContainerStatus::RUNNING || status_ == ContainerStatus::BOOTING || status_ == ContainerStatus::PAUSED) {
        VINE_LOGW("Container %s destroyed while running, killing it", config_.instance_id.c_str());
        kill_now();
    }
}

bool Container::fail(const std::string& message) {
    last_error_ = message;
    VINE_LOGE("container %s: %s", config_.instance_id.c_str(), message.c_str());
    return false;
}

bool Container::start() {
    if (status_ == ContainerStatus::BOOTING || status_ == ContainerStatus::RUNNING || status_ == ContainerStatus::PAUSED) {
        return fail("already running");
    }
    last_error_.clear();
    boot_signal_seen_ = false;
    stop_requested_ = false;
    exit_code_ = 0;
    status_ = ContainerStatus::BOOTING;

    if (!prepare() || !spawn()) {
        teardown_cgroups();
        nat_.reset();
        if (qemu_registered_) { binfmt_arm_release(); qemu_registered_ = false; }
        status_ = ContainerStatus::ERROR;
        return false;
    }
    start_ms_ = monotonic_ms();
    VINE_LOGI("container %s started, init pid %d", config_.instance_id.c_str(), (int)init_pid_);
    return true;
}

bool Container::prepare() {
    if (config_.instance_id.empty() || config_.instance_path.empty()) return fail("instance id and path are required");
    if (!mkdirs(config_.instance_path)) return fail("cannot create " + config_.instance_path);
    if (!is_regular_file(config_.system_image)) return fail("system.img is missing from the instance");
    if (!is_regular_file(config_.ramdisk_image)) {
        return fail("ramdisk.img is missing (ROMs without a ramdisk, system-as-root, are not supported yet)");
    }
    return prepare_images() && prepare_rootfs() && prepare_data() && prepare_shared() && write_boot_files();
}

bool Container::prepare_images() {
    has_vendor_ = is_regular_file(config_.vendor_image);
    for (const std::string& image : {config_.system_image, config_.vendor_image}) {
        if (!is_regular_file(image) || !is_android_sparse_image(image)) continue;
        VINE_LOGI("expanding sparse image %s", image.c_str());
        std::string error;
        const std::string raw = image + ".raw";
        if (!sparse_image_to_raw(image, raw, &error)) return fail("cannot expand " + image + ": " + error);
        if (rename(raw.c_str(), image.c_str()) != 0) return fail(errno_text("replace " + image));
    }
    return true;
}

bool Container::prepare_rootfs() {
    const std::string& root = config_.rootfs_dir;
    if (path_exists(root) && !remove_recursive(root)) return fail("cannot clear " + root);
    if (!mkdirs(root)) return fail("cannot create " + root);
    std::string error;
    if (!extract_ramdisk(config_.ramdisk_image, root, &error)) return fail(error);

    for (const char* dir : {"proc", "sys", "dev", "system", "vendor", "data", "cache", "vine", "mnt"}) {
        const std::string path = root + "/" + dir;
        if (!is_directory(path) && !mkdirs(path)) return fail("cannot create " + path);
    }
    const std::string init = root + "/init";
    if (!is_regular_file(init)) return fail("the ramdisk has no /init");
    if (!config_.allow_unpatched_init && !file_contains(init, kInitMarker, kInitScanBytes)) {
        return fail(std::string("this ROM's init is not container aware (marker ") + kInitMarker + " not found). An unpatched Android "
                    "init would load its SELinux policy into the phone's kernel, so the start was refused. See ROM_INTEGRATION.md");
    }
    hardware_ = config_.hardware.empty() ? detect_hardware(root) : config_.hardware;
    return true;
}

bool Container::prepare_data() {
    data_is_image_ = false;
    if (config_.data_size_mb <= 0) return mkdirs(config_.data_dir) || fail("cannot create " + config_.data_dir);

    const std::string tool = !config_.mke2fs_path.empty() ? config_.mke2fs_path : find_executable("mke2fs");
    const uint64_t want = (uint64_t)config_.data_size_mb * kMiB;
    std::string error;

    if (is_regular_file(config_.data_image)) {
        data_is_image_ = true;
        if (!grow_ext4(config_.data_image, want, &error)) VINE_LOGW("storage not grown: %s", error.c_str());
        return true;
    }
    if (tool.empty()) {
        VINE_LOGW("no mke2fs on this device, /data is a plain directory without a size limit");
        return mkdirs(config_.data_dir) || fail("cannot create " + config_.data_dir);
    }
    if (!create_sparse_file(config_.data_image, want)) return fail("cannot create " + config_.data_image + " (not enough space?)");
    if (!format_ext4(tool, config_.data_image, (uint64_t)config_.data_size_mb, &error)) {
        unlink(config_.data_image.c_str());
        return fail(error);
    }
    data_is_image_ = true;
    return true;
}

bool Container::prepare_shared() {
    const std::string& dir = config_.shared_dir;
    if (!mkdirs(dir, 0777)) return fail("cannot create " + dir);
    chmod(dir.c_str(), 0777);
    for (const char* stale : {"boot_completed", "agent.sock", "vinefb"}) unlink((dir + "/" + stale).c_str());

    using namespace display;
    const uint32_t side = (uint32_t)std::max(config_.display_width, config_.display_height);
    VineFbHeader h{};
    h.magic = kVineFbMagic;
    h.version = kVineFbVersion;
    h.width = (uint32_t)config_.display_width;
    h.height = (uint32_t)config_.display_height;
    h.stride = h.width * 4;
    h.format = (uint32_t)VineFbFormat::RGBA_8888;
    h.data_offset = kVineFbHeaderSize;
    h.capacity = side * side * 4; // room for either orientation
    std::string header(reinterpret_cast<const char*>(&h), sizeof(h));
    header.resize(kVineFbHeaderSize, '\0');
    const std::string fb = dir + "/vinefb";
    if (!write_file(fb, header, 0666)) return fail("cannot create " + fb);
    if (truncate(fb.c_str(), (off_t)(kVineFbHeaderSize + h.capacity)) != 0) return fail(errno_text("size " + fb));
    chmod(fb.c_str(), 0666);
    if (config_.owner_uid >= 0) {
        chown(dir.c_str(), (uid_t)config_.owner_uid, (gid_t)config_.owner_gid);
        chown(fb.c_str(), (uid_t)config_.owner_uid, (gid_t)config_.owner_gid);
    }
    return true;
}

bool Container::write_boot_files() {
    net_plan_ = NetPlan{};
    if (config_.network == "nat" && config_.net_slot > 0) net_plan_ = make_net_plan(config_.net_slot);

    BootParams bp;
    bp.instance_id = config_.instance_id;
    bp.hardware = hardware_;
    bp.display_width = config_.display_width;
    bp.display_height = config_.display_height;
    bp.display_dpi = config_.display_dpi;
    bp.ram_mb = config_.ram_mb;
    bp.rooted = config_.rooted;
    bp.qemu_32bit = config_.needs_qemu_32bit;
    if (!net_plan_.guest_ip.empty()) {
        bp.net_ip = net_plan_.guest_ip;
        bp.net_gateway = net_plan_.host_ip;
        bp.net_dns = net_plan_.dns;
    }

    const std::string props = build_boot_prop(bp);
    const std::string& dir = config_.shared_dir;
    if (!write_file_atomic(dir + "/boot.prop", props, 0644) || !write_file_atomic(dir + "/cmdline", build_cmdline(bp), 0644)) {
        return fail("cannot write boot files into " + dir);
    }
    if (!write_managed_block(config_.rootfs_dir + "/default.prop", split_lines(props))) return fail("cannot update default.prop");
    if (config_.owner_uid >= 0) {
        for (const char* f : {"/boot.prop", "/cmdline"}) chown((dir + f).c_str(), (uid_t)config_.owner_uid, (gid_t)config_.owner_gid);
    }
    return true;
}

bool Container::spawn() {
    SpawnFds fds;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return fail(errno_text("pipe"));
    fds.err_r = p[0]; fds.err_w = p[1];
    if (pipe2(p, O_CLOEXEC) != 0) return fail(errno_text("pipe"));
    fds.pid_r = p[0]; fds.pid_w = p[1];
    if (pipe2(p, O_CLOEXEC) != 0) return fail(errno_text("pipe"));
    fds.go_r = p[0]; fds.go_w = p[1];
    fds.log = open(config_.boot_log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fds.log >= 0) {
        const std::string banner = "=== VineOS start " + std::to_string((long)time(nullptr)) + " instance " + config_.instance_id + " ===\n";
        write_all(fds.log, banner.data(), banner.size());
        if (config_.owner_uid >= 0) fchown(fds.log, (uid_t)config_.owner_uid, (gid_t)config_.owner_gid);
    }

    auto close_all = [&]() {
        for (int* fd : {&fds.err_r, &fds.err_w, &fds.pid_r, &fds.pid_w, &fds.go_r, &fds.go_w, &fds.log}) {
            if (*fd >= 0) { close(*fd); *fd = -1; }
        }
    };

    LaunchSpec spec;
    spec.cfg = &config_;
    spec.data_is_image = data_is_image_;
    spec.has_vendor = has_vendor_;
    spec.isolate_net = config_.network != "host";
    spec.net = net_plan_;
    spec.host_pid = getpid();

    if (config_.needs_qemu_32bit) {
        std::string error;
        if (binfmt_arm_acquire(config_.qemu_arm_path, &error)) qemu_registered_ = true;
        else VINE_LOGW("32-bit ARM support unavailable: %s", error.c_str());
    }

    const pid_t monitor = fork();
    if (monitor < 0) {
        close_all();
        return fail(errno_text("fork"));
    }
    if (monitor == 0) {
        close(fds.err_r);
        close(fds.pid_r);
        run_supervisor(spec, fds);
    }
    monitor_pid_ = monitor;
    close(fds.err_w); fds.err_w = -1;
    close(fds.pid_w); fds.pid_w = -1;
    close(fds.go_r); fds.go_r = -1;
    if (fds.log >= 0) { close(fds.log); fds.log = -1; }

    auto abort_start = [&](const std::string& why) {
        if (init_pid_ > 1) kill(init_pid_, SIGKILL);
        kill(monitor_pid_, SIGKILL);
        int st = 0;
        waitpid(monitor_pid_, &st, 0);
        monitor_pid_ = -1;
        init_pid_ = -1;
        close_all();
        return fail(why);
    };
    auto read_error = [&]() {
        char buf[1024];
        ssize_t n = read(fds.err_r, buf, sizeof(buf) - 1);
        return n > 0 ? std::string(buf, (size_t)n) : std::string();
    };

    // The supervisor either reports the guest's host-side pid or an error
    std::string pid_text;
    const int64_t deadline = monotonic_ms() + kSetupTimeoutMs;
    while (pid_text.find('\n') == std::string::npos) {
        pollfd pf[2] = {{fds.pid_r, POLLIN, 0}, {fds.err_r, POLLIN, 0}};
        const int left = (int)(deadline - monotonic_ms());
        if (left <= 0) return abort_start("timed out waiting for the guest supervisor");
        if (::poll(pf, 2, left) < 0 && errno == EINTR) continue;
        if (pf[1].revents & (POLLIN | POLLHUP)) {
            std::string msg = read_error();
            return abort_start(msg.empty() ? "guest supervisor died during startup" : msg);
        }
        if (pf[0].revents & POLLIN) {
            char buf[64];
            ssize_t n = read(fds.pid_r, buf, sizeof(buf));
            if (n <= 0) return abort_start("guest supervisor closed its pipe");
            pid_text.append(buf, (size_t)n);
        }
    }
    init_pid_ = (pid_t)atoi(pid_text.c_str());
    if (init_pid_ <= 1) return abort_start("guest supervisor reported a bad pid");

    if (!setup_cgroups()) VINE_LOGW("resource limits not applied for %s", config_.instance_id.c_str());

    if (!net_plan_.guest_ip.empty()) {
        auto nat = std::make_unique<HostNat>();
        std::string error;
        if (nat->setup(init_pid_, net_plan_, &error)) nat_ = std::move(nat);
        else VINE_LOGW("guest has no internet: %s", error.c_str());
    }

    if (write_all(fds.go_w, "g", 1)) {
        close(fds.go_w);
        fds.go_w = -1;
    }

    // EOF on the error pipe means init was exec'd, a message means setup failed
    if (!wait_readable(fds.err_r, kSetupTimeoutMs)) return abort_start("timed out while setting up the guest");
    std::string msg = read_error();
    if (!msg.empty()) {
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        return abort_start(msg);
    }
    int st = 0;
    if (waitpid(monitor_pid_, &st, WNOHANG) == monitor_pid_) {
        if (init_pid_ > 1) kill(init_pid_, SIGKILL);
        monitor_pid_ = -1;
        init_pid_ = -1;
        close_all();
        return fail("the guest exited while starting, see boot.log");
    }
    close_all();
    return true;
}

bool Container::setup_cgroups() {
    if (init_pid_ <= 1) return false;
    const std::string tag = "vineos-" + safe_tag(config_.instance_id);
    const std::string pid = std::to_string(init_pid_);
    const long period_us = 100000;
    bool ok = false;

    CgroupMount v2;
    if (find_cgroup_mount("memory", &v2) && v2.v2) {
        // Unified hierarchy: one directory holds every limit and the freezer
        write_file(path_join(v2.path, "cgroup.subtree_control"), "+memory +cpu");
        const std::string cg = path_join(v2.path, tag);
        if (mkdirs(cg)) {
            if (config_.ram_mb > 0) write_file(cg + "/memory.max", std::to_string((long long)config_.ram_mb * (long long)kMiB));
            if (config_.cpu_cores > 0) {
                write_file(cg + "/cpu.max", std::to_string(period_us * config_.cpu_cores) + " " + std::to_string(period_us));
            }
            if (write_file(cg + "/cgroup.procs", pid)) {
                cgroup_paths_.push_back(cg);
                if (path_exists(cg + "/cgroup.freeze")) freeze_file_ = cg + "/cgroup.freeze";
                ok = true;
            } else {
                rmdir(cg.c_str());
            }
        }
        return ok;
    }

    CgroupMount mem, cpu, frz;
    if (config_.ram_mb > 0 && find_cgroup_mount("memory", &mem)) {
        const std::string cg = path_join(mem.path, tag);
        if (mkdirs(cg) && write_file(cg + "/memory.limit_in_bytes", std::to_string((long long)config_.ram_mb * (long long)kMiB)) &&
            write_file(cg + "/tasks", pid)) {
            cgroup_paths_.push_back(cg);
            ok = true;
        } else {
            rmdir(cg.c_str());
        }
    }
    if (config_.cpu_cores > 0 && find_cgroup_mount("cpu", &cpu)) {
        const std::string cg = path_join(cpu.path, tag);
        if (mkdirs(cg) && write_file(cg + "/cpu.cfs_period_us", std::to_string(period_us)) &&
            write_file(cg + "/cpu.cfs_quota_us", std::to_string(period_us * config_.cpu_cores)) && write_file(cg + "/tasks", pid)) {
            cgroup_paths_.push_back(cg);
            ok = true;
        } else {
            rmdir(cg.c_str());
        }
    }
    if (find_cgroup_mount("freezer", &frz)) {
        const std::string cg = path_join(frz.path, tag);
        if (mkdirs(cg) && write_file(cg + (frz.v2 ? "/cgroup.procs" : "/tasks"), pid)) {
            cgroup_paths_.push_back(cg);
            freeze_file_ = cg + (frz.v2 ? "/cgroup.freeze" : "/freezer.state");
            ok = true;
        } else {
            rmdir(cg.c_str());
        }
    }
    return ok;
}

void Container::teardown_cgroups() {
    for (const auto& cg : cgroup_paths_) {
        for (int i = 0; i < 20; ++i) {
            if (rmdir(cg.c_str()) == 0 || errno == ENOENT) break;
            sleep_ms(25);
        }
    }
    cgroup_paths_.clear();
    freeze_file_.clear();
}

bool Container::pause() {
    if (status_ != ContainerStatus::RUNNING && status_ != ContainerStatus::BOOTING) return false;
    if (freeze_file_.empty()) return false;
    const bool v2 = freeze_file_.size() >= 13 && freeze_file_.compare(freeze_file_.size() - 13, 13, "cgroup.freeze") == 0;
    if (!write_file(freeze_file_, v2 ? "1" : "FROZEN")) return false;
    status_ = ContainerStatus::PAUSED;
    return true;
}

bool Container::resume() {
    if (status_ != ContainerStatus::PAUSED) return false;
    const bool v2 = freeze_file_.size() >= 13 && freeze_file_.compare(freeze_file_.size() - 13, 13, "cgroup.freeze") == 0;
    if (!write_file(freeze_file_, v2 ? "0" : "THAWED")) return false;
    status_ = boot_signal_seen_ ? ContainerStatus::RUNNING : ContainerStatus::BOOTING;
    return true;
}

void Container::finish(int wait_status) {
    int code = 0;
    if (wait_status >= 0) {
        if (WIFEXITED(wait_status)) code = WEXITSTATUS(wait_status);
        else if (WIFSIGNALED(wait_status)) code = 128 + WTERMSIG(wait_status);
    } else {
        code = -1;
    }
    exit_code_ = code;
    monitor_pid_ = -1;
    init_pid_ = -1;
    teardown_cgroups();
    nat_.reset();
    if (qemu_registered_) { binfmt_arm_release(); qemu_registered_ = false; }

    const bool clean = code == 0 || code == kExitSighup || code == kExitSigint ||
                       (stop_requested_ && (code == kExitSigkill || code == kExitSigterm));
    if (clean) {
        status_ = ContainerStatus::STOPPED;
    } else {
        status_ = ContainerStatus::ERROR;
        last_error_ = "the guest exited with code " + std::to_string(code) + ", see boot.log";
        VINE_LOGW("container %s: %s", config_.instance_id.c_str(), last_error_.c_str());
    }
}

ContainerStatus Container::poll() {
    if (status_ != ContainerStatus::BOOTING && status_ != ContainerStatus::RUNNING && status_ != ContainerStatus::PAUSED) {
        return status_;
    }
    int st = 0;
    const pid_t w = waitpid(monitor_pid_, &st, WNOHANG);
    if (w == monitor_pid_) {
        finish(st);
        return status_;
    }
    if (w < 0 && errno == ECHILD) {
        finish(-1);
        return status_;
    }
    if (status_ == ContainerStatus::BOOTING) {
        bool booted = path_exists(config_.shared_dir + "/boot_completed");
        if (!booted) {
            if (auto head = read_file_prefix(config_.shared_dir + "/vinefb", sizeof(display::VineFbHeader))) {
                if (head->size() == sizeof(display::VineFbHeader)) {
                    display::VineFbHeader h;
                    memcpy(&h, head->data(), sizeof(h));
                    booted = h.magic == display::kVineFbMagic && h.frame_count > 0;
                }
            }
        }
        if (booted) {
            boot_signal_seen_ = true;
            status_ = ContainerStatus::RUNNING;
        } else if (monotonic_ms() - start_ms_ > (int64_t)config_.boot_timeout_s * 1000) {
            VINE_LOGW("container %s sent no boot signal after %d s, treating it as running", config_.instance_id.c_str(),
                      config_.boot_timeout_s);
            status_ = ContainerStatus::RUNNING;
        }
    }
    return status_;
}

void Container::stop(int timeout_ms) {
    if (status_ != ContainerStatus::BOOTING && status_ != ContainerStatus::RUNNING && status_ != ContainerStatus::PAUSED) return;
    stop_requested_ = true;
    if (status_ == ContainerStatus::PAUSED) resume();
    if (init_pid_ > 1) kill(init_pid_, SIGTERM);
    const int64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        poll();
        if (status_ != ContainerStatus::BOOTING && status_ != ContainerStatus::RUNNING && status_ != ContainerStatus::PAUSED) return;
        sleep_ms(50);
    }
    kill_now();
}

void Container::kill_now() {
    if (status_ != ContainerStatus::BOOTING && status_ != ContainerStatus::RUNNING && status_ != ContainerStatus::PAUSED) return;
    stop_requested_ = true;
    if (status_ == ContainerStatus::PAUSED) resume();
    if (init_pid_ > 1) kill(init_pid_, SIGKILL);
    if (monitor_pid_ > 1) kill(monitor_pid_, SIGKILL);
    int st = 0;
    for (int i = 0; i < 200; ++i) {
        const pid_t w = waitpid(monitor_pid_, &st, WNOHANG);
        if (w == monitor_pid_ || (w < 0 && errno == ECHILD)) break;
        sleep_ms(50);
    }
    finish(st);
}

std::string Container::diagnostics() const {
    static const char* kNames[] = {"stopped", "booting", "running", "error", "paused"};
    std::string out = "instance      : " + config_.instance_id + "\n";
    out += std::string("state         : ") + kNames[(int)status_] + "\n";
    out += "init pid      : " + std::to_string(init_pid_) + "\n";
    out += "boot signal   : " + std::string(boot_signal_seen_ ? "seen" : "not yet") + "\n";
    out += "hardware      : " + (hardware_.empty() ? std::string("(none detected)") : hardware_) + "\n";
    out += "/data         : " + std::string(data_is_image_ ? "ext4 image " + config_.data_image : "host directory " + config_.data_dir) + "\n";
    out += "ram cap       : " + (config_.ram_mb > 0 ? std::to_string(config_.ram_mb) + " MB" : std::string("none")) + "\n";
    out += "cpu cap       : " + (config_.cpu_cores > 0 ? std::to_string(config_.cpu_cores) + " cores" : std::string("none")) + "\n";
    out += "cgroups       : " + std::to_string(cgroup_paths_.size()) + " applied, freezer " + (freeze_file_.empty() ? "no" : "yes") + "\n";
    out += "network       : " + config_.network + (nat_ && nat_->active() ? " (" + net_plan_.guest_ip + ")" : "") + "\n";
    out += "binderfs      : " + std::string(kernel_has_filesystem("binder") ? "supported" : "not supported by this kernel") + "\n";
    out += "32-bit (qemu) : " + std::string(config_.needs_qemu_32bit ? (qemu_registered_ ? "active" : "needed, not active") : "not needed") + "\n";
    if (!last_error_.empty()) out += "last error    : " + last_error_ + "\n";
    const std::string log = tail_file(config_.boot_log_path, 4096);
    if (!log.empty()) out += "--- boot.log (tail) ---\n" + log;
    return out;
}

NamespaceManager& NamespaceManager::instance() {
    static NamespaceManager mgr;
    return mgr;
}

bool NamespaceManager::init(const std::string& data_dir, const std::string& native_lib_dir) {
    data_dir_ = data_dir;
    native_lib_dir_ = native_lib_dir;
    signal(SIGPIPE, SIG_IGN); // a guest dying mid-handshake must not kill the host process
    initialized_ = true;
    VINE_LOGI("NamespaceManager ready, data=%s libs=%s", data_dir.c_str(), native_lib_dir.c_str());
    return true;
}

void NamespaceManager::shutdown() {
    for (auto& entry : containers_) entry.second->stop(5000);
    containers_.clear();
    initialized_ = false;
}

int64_t NamespaceManager::start_container(const ContainerConfig& config) {
    last_start_error_.clear();
    if (!initialized_) {
        last_start_error_ = "runtime not initialised";
        return 0;
    }
    for (const auto& entry : containers_) {
        const Container& other = *entry.second;
        const bool active = other.status() == ContainerStatus::BOOTING || other.status() == ContainerStatus::RUNNING ||
                            other.status() == ContainerStatus::PAUSED;
        if (active && other.instance_id() == config.instance_id) {
            last_start_error_ = "instance " + config.instance_id + " is already running";
            return 0;
        }
    }
    ContainerConfig cfg = config;
    if (cfg.network == "nat" && cfg.net_slot <= 0) {
        for (int slot = 1; slot <= 250; ++slot) {
            bool taken = false;
            for (const auto& entry : containers_) {
                if (entry.second->config().net_slot == slot && entry.second->status() != ContainerStatus::STOPPED &&
                    entry.second->status() != ContainerStatus::ERROR) {
                    taken = true;
                    break;
                }
            }
            if (!taken) { cfg.net_slot = slot; break; }
        }
    }
    auto container = std::make_unique<Container>(cfg);
    if (!container->start()) {
        last_start_error_ = container->last_error();
        return 0;
    }
    const int64_t handle = next_handle_++;
    containers_[handle] = std::move(container);
    return handle;
}

void NamespaceManager::stop_container(int64_t handle) {
    if (Container* c = get_container(handle)) c->stop();
}

void NamespaceManager::kill_container(int64_t handle) {
    if (Container* c = get_container(handle)) c->kill_now();
}

Container* NamespaceManager::get_container(int64_t handle) {
    auto it = containers_.find(handle);
    return it != containers_.end() ? it->second.get() : nullptr;
}

void NamespaceManager::poll_all() {
    for (auto& entry : containers_) entry.second->poll();
}

void NamespaceManager::remove_container(int64_t handle) {
    containers_.erase(handle);
}

std::vector<int64_t> NamespaceManager::handles() const {
    std::vector<int64_t> out;
    for (const auto& entry : containers_) out.push_back(entry.first);
    std::sort(out.begin(), out.end());
    return out;
}

size_t NamespaceManager::running_count() const {
    size_t n = 0;
    for (const auto& entry : containers_) {
        const ContainerStatus s = entry.second->status();
        if (s == ContainerStatus::BOOTING || s == ContainerStatus::RUNNING || s == ContainerStatus::PAUSED) ++n;
    }
    return n;
}

} // namespace vine
