// Stand-in for a container-aware Android init, used by the host-side engine tests.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

// The runtime refuses to start an init that does not carry this marker
extern "C" const char kMarker[] = "VINE_CONTAINER_INIT_V1";

static volatile sig_atomic_t g_stop = 0;
static void on_term(int) { g_stop = 1; }

static std::string slurp(const char* path, size_t max = 4096) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return "";
    std::string s(max, '\0');
    ssize_t n = read(fd, &s[0], max);
    close(fd);
    s.resize(n > 0 ? (size_t)n : 0);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

static std::string errno_of_open(const char* path, int flags, mode_t mode = 0600) {
    int fd = open(path, flags, mode);
    if (fd >= 0) {
        close(fd);
        return "ok";
    }
    return strerror(errno);
}

int main() {
    // Keep the marker referenced so the linker can't drop it
    volatile const char* keep = kMarker;
    (void)keep;

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_DFL);

    std::string r;
    auto add = [&](const std::string& k, const std::string& v) { r += k + "=" + v + "\n"; };

    add("pid", std::to_string(getpid()));
    char host[128] = {0};
    gethostname(host, sizeof(host) - 1);
    add("hostname", host);
    add("system_marker", slurp("/system/marker.txt"));
    add("system_ro", errno_of_open("/system/should_fail", O_CREAT | O_WRONLY));
    add("vendor_marker", slurp("/vendor/marker.txt"));
    add("data_existed", access("/data/written_by_guest", F_OK) == 0 ? "1" : "0");
    add("data_write", errno_of_open("/data/written_by_guest", O_CREAT | O_WRONLY));
    struct statvfs sv{};
    if (statvfs("/data", &sv) == 0) add("data_total_mb", std::to_string((unsigned long long)sv.f_blocks * sv.f_frsize / (1024 * 1024)));
    add("proc_pid1_exe", [] { char b[256] = {0}; ssize_t n = readlink("/proc/1/exe", b, sizeof(b) - 1); return n > 0 ? std::string(b, (size_t)n) : std::string(); }());
    int procs = 0;
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) if (atoi(e->d_name) > 0) ++procs;
        closedir(d);
    }
    add("proc_count", std::to_string(procs));
    add("procsys_write", errno_of_open("/proc/sys/vm/swappiness", O_WRONLY));
    add("sys_mkdir", mkdir("/sys/vine_test", 0755) == 0 ? "ok" : strerror(errno));
    add("cmdline", slurp("/proc/cmdline"));
    add("boot_prop_instance", [] {
        std::string p = slurp("/vine/boot.prop", 8192);
        size_t at = p.find("ro.boot.vine.instance=");
        if (at == std::string::npos) return std::string();
        size_t end = p.find('\n', at);
        return p.substr(at + 22, end == std::string::npos ? std::string::npos : end - at - 22);
    }());
    add("default_prop_has_block", slurp("/default.prop", 8192).find("# vine-begin") != std::string::npos ? "1" : "0");
    add("console_tty", isatty(1) ? "1" : "0");
    add("dev_null", errno_of_open("/dev/null", O_WRONLY));
    add("dev_pts_ptmx", access("/dev/pts/ptmx", F_OK) == 0 ? "1" : "0");
    add("dev_console", access("/dev/console", F_OK) == 0 ? "1" : "0");
    add("old_root_gone", access("/.old_root", F_OK) != 0 ? "1" : "0");
    add("binder_node", access("/dev/binder", F_OK) == 0 ? "1" : "0");
    add("cache_writable", errno_of_open("/cache/x", O_CREAT | O_WRONLY));
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        ifreq ifr{};
        strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
        const bool up = s >= 0 && ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_UP);
        add("lo_up", up ? "1" : "0");
        if (s >= 0) close(s);
        add("eth0", if_nametoindex("eth0") ? "1" : "0");
        int ifaces = 0;
        if (DIR* d = opendir("/sys/class/net")) {
            while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++ifaces;
            closedir(d);
        }
        add("net_ifaces", std::to_string(ifaces));
    }
    add("fd_leak", [] {
        int n = 0;
        if (DIR* d = opendir("/proc/self/fd")) {
            while (dirent* e = readdir(d)) if (atoi(e->d_name) > 2) ++n;
            closedir(d);
        }
        return std::to_string(n);
    }());

    printf("fake-init: report ready\n");
    fflush(stdout);
    int rfd = open("/vine/report.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (rfd >= 0) {
        if (write(rfd, r.data(), r.size()) < 0) perror("report");
        close(rfd);
    }

    if (access("/vine/crash", F_OK) == 0) {
        printf("fake-init: crashing on request\n");
        fflush(stdout);
        _exit(7);
    }

    int bfd = open("/vine/boot_completed", O_CREAT | O_WRONLY, 0644);
    if (bfd >= 0) close(bfd);
    printf("fake-init: booted\n");
    fflush(stdout);

    unsigned long tick = 0;
    while (!g_stop) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
        if (access("/vine/poweroff", F_OK) == 0) {
            printf("fake-init: powering off\n");
            fflush(stdout);
            sync();
            reboot(RB_POWER_OFF);
        }
        char line[32];
        int len = snprintf(line, sizeof(line), "%lu\n", ++tick);
        int hb = open("/vine/heartbeat", O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (hb >= 0) {
            if (write(hb, line, (size_t)len) < 0) perror("heartbeat");
            close(hb);
        }
        usleep(100000);
    }
    printf("fake-init: stopping\n");
    fflush(stdout);
    return 0;
}
