// Boots a fake ROM through the real Container code in real namespaces. Needs root and a Linux kernel with loop devices.
#include "../../container/guest_exec.h"
#include "../../container/namespace_manager.h"
#include "../../container/sparse_image.h"
#include "../../utils/vine_utils.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace vine;

namespace {

int g_failed = 0;
int g_checks = 0;

#define CHECK(cond, ...)                                                                \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failed;                                                                 \
            printf("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);                    \
            printf(__VA_ARGS__);                                                        \
            printf("\n");                                                               \
        }                                                                               \
    } while (0)

std::string g_fixture;
std::string g_work;

struct Instance {
    std::string id;
    std::string dir;
    ContainerConfig cfg;
};

Instance make_instance(const std::string& id, bool patched = true) {
    Instance in;
    in.id = id;
    in.dir = g_work + "/" + id;
    mkdirs(in.dir);
    const std::string ramdisk = g_fixture + (patched ? "/ramdisk.img" : "/ramdisk_unpatched.img");
    copy_file_sparse(ramdisk, in.dir + "/ramdisk.img");
    copy_file_sparse(g_fixture + "/system.img", in.dir + "/system.img");
    copy_file_sparse(g_fixture + "/vendor.img", in.dir + "/vendor.img");
    in.cfg = make_container_config(id, in.dir);
    in.cfg.data_size_mb = 64;
    in.cfg.ram_mb = 256;
    in.cfg.cpu_cores = 1;
    in.cfg.network = "nat";
    in.cfg.display_width = 720;
    in.cfg.display_height = 1280;
    in.cfg.boot_timeout_s = 60;
    return in;
}

std::map<std::string, std::string> read_report(const Instance& in) {
    std::map<std::string, std::string> out;
    for (const auto& line : split_lines(read_file(in.cfg.shared_dir + "/report.txt").value_or(""))) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

bool wait_status(NamespaceManager& m, int64_t h, ContainerStatus want, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; ++i) {
        m.poll_all();
        Container* c = m.get_container(h);
        if (c && c->status() == want) return true;
        usleep(100000);
    }
    return false;
}

std::string mountinfo() { return read_file("/proc/self/mountinfo").value_or(""); }

// Loop devices that still point into the test directory
int loops_using(const std::string& needle) {
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        const std::string f = "/sys/block/loop" + std::to_string(i) + "/loop/backing_file";
        auto b = read_file(f);
        if (b && b->find(needle) != std::string::npos) ++n;
    }
    return n;
}

int count_processes_named(const std::string& name) {
    int n = 0;
    for (int pid = 2; pid < 400000 && n < 100; pid += 1) {
        if (access(("/proc/" + std::to_string(pid)).c_str(), F_OK) != 0) continue;
        auto cmd = read_file("/proc/" + std::to_string(pid) + "/cmdline");
        if (cmd && cmd->find(name) != std::string::npos) {
            ++n;
            if (getenv("VINE_TEST_VERBOSE")) printf("    still running: pid %d cmdline %s status %s\n", pid, cmd->c_str(),
                                                    read_file("/proc/" + std::to_string(pid) + "/stat").value_or("?").c_str());
        }
        if (pid > 40000) break;
    }
    return n;
}

void test_boot_isolation_and_cleanup() {
    printf("[boot, isolation, exec, pause, stop]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance in = make_instance("boot1");
    char host_before[128] = {0};
    gethostname(host_before, sizeof(host_before) - 1);
    const std::string mounts_before = mountinfo();

    CHECK(is_android_sparse_image(in.dir + "/system.img"), "fixture should start out sparse");
    const int64_t h = m.start_container(in.cfg);
    CHECK(h != 0, "start failed: %s", m.last_start_error().c_str());
    if (!h) return;
    Container* c = m.get_container(h);
    CHECK(c->status() == ContainerStatus::BOOTING || c->status() == ContainerStatus::RUNNING, "status right after start");
    CHECK(c->init_pid() > 1, "init pid");
    CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "never reached RUNNING, error: %s\n%s", c->last_error().c_str(), c->diagnostics().c_str());
    CHECK(c->boot_signal_seen(), "boot signal");
    CHECK(!is_android_sparse_image(in.dir + "/system.img"), "system.img should have been expanded");

    auto r = read_report(in);
    if (getenv("VINE_TEST_VERBOSE")) {
        for (const auto& kv : r) printf("    report %s = %s\n", kv.first.c_str(), kv.second.c_str());
    }
    CHECK(r["pid"] == "1", "guest init pid is %s", r["pid"].c_str());
    CHECK(r["hostname"] == "vine-boot1", "hostname %s", r["hostname"].c_str());
    CHECK(r["system_marker"] == "system-v1", "system marker '%s'", r["system_marker"].c_str());
    CHECK(r["vendor_marker"] == "vendor-v1", "vendor marker '%s'", r["vendor_marker"].c_str());
    CHECK(r["system_ro"] == "Read-only file system", "/system should be read-only: %s", r["system_ro"].c_str());
    CHECK(r["data_write"] == "ok", "/data write: %s", r["data_write"].c_str());
    CHECK(r["data_existed"] == "0", "fresh /data");
    const int total = atoi(r["data_total_mb"].c_str());
    CHECK(total > 40 && total <= 64, "data size %d MB", total);
    CHECK(r["proc_pid1_exe"] == "/init", "proc shows guest pid 1: %s", r["proc_pid1_exe"].c_str());
    CHECK(atoi(r["proc_count"].c_str()) < 10, "proc lists %s processes, host leaks in", r["proc_count"].c_str());
    CHECK(r["procsys_write"] == "Read-only file system", "/proc/sys must be read-only: %s", r["procsys_write"].c_str());
    CHECK(r["sys_mkdir"] == "Read-only file system", "/sys must be read-only: %s", r["sys_mkdir"].c_str());
    CHECK(r["cmdline"].find("androidboot.vine=1") != std::string::npos, "cmdline '%s'", r["cmdline"].c_str());
    CHECK(r["cmdline"].find("androidboot.vine.instance=boot1") != std::string::npos, "cmdline instance");
    CHECK(r["boot_prop_instance"] == "boot1", "boot.prop instance '%s'", r["boot_prop_instance"].c_str());
    CHECK(r["default_prop_has_block"] == "1", "default.prop managed block");
    CHECK(r["console_tty"] == "1", "console is a tty");
    CHECK(r["dev_null"] == "ok" && r["dev_pts_ptmx"] == "1" && r["dev_console"] == "1", "dev nodes");
    CHECK(r["old_root_gone"] == "1", "old root detached");
    CHECK(r["cache_writable"] == "ok", "/cache");
    CHECK(r["lo_up"] == "1", "lo must be up in the private network namespace");
    CHECK(r["fd_leak"] == "0" || r["fd_leak"] == "1", "inherited fds in the guest: %s", r["fd_leak"].c_str());
    CHECK(atoi(r["net_ifaces"].c_str()) <= 2, "guest sees %s network interfaces, it should have only its own", r["net_ifaces"].c_str());

    CHECK(mountinfo() == mounts_before, "the host mount table changed while the guest runs");
    char host_now[128] = {0};
    gethostname(host_now, sizeof(host_now) - 1);
    CHECK(std::string(host_now) == host_before, "host hostname changed");
    CHECK(c->diagnostics().find("state         : running") != std::string::npos, "diagnostics");

    // running a command inside the guest
    GuestExecResult er;
    bool ok = guest_exec(c->init_pid(), {"/system/bin/fake_tool", "hello", "world"}, 5000, &er);
    CHECK(ok && er.exit_code == 0, "guest_exec: %s / %s", er.error.c_str(), er.output.c_str());
    CHECK(er.output.find("marker=system-v1") != std::string::npos, "tool ran outside the guest: %s", er.output.c_str());
    CHECK(er.output.find("args=hello,world") != std::string::npos, "args: %s", er.output.c_str());
    CHECK(er.output.find("ANDROID_ROOT=/system") != std::string::npos, "env: %s", er.output.c_str());
    const size_t at = er.output.find("pid=");
    CHECK(at != std::string::npos && atoi(er.output.c_str() + at + 4) < 100, "tool pid should be a guest pid: %s", er.output.c_str());
    ok = guest_exec(c->init_pid(), {"fake_tool", "exit", "3"}, 5000, &er);
    CHECK(er.exit_code == 3, "exit code %d (%s)", er.exit_code, er.output.c_str());
    ok = guest_exec(c->init_pid(), {"/system/bin/fake_tool", "sleep"}, 500, &er);
    CHECK(!ok && er.timed_out, "timeout not reported");
    for (int i = 0; i < 20 && count_processes_named("fake_tool") > 0; ++i) usleep(100000);
    CHECK(count_processes_named("fake_tool") == 0, "timed-out command still running");
    ok = guest_exec(c->init_pid(), {"/nonexistent"}, 3000, &er);
    CHECK(er.exit_code == 127, "missing binary should exit 127, got %d", er.exit_code);

    // freeze and thaw
    if (c->can_pause()) {
        CHECK(c->pause(), "pause");
        CHECK(c->status() == ContainerStatus::PAUSED, "paused status");
        const std::string a = read_file(in.cfg.shared_dir + "/heartbeat").value_or("");
        usleep(600000);
        const std::string b = read_file(in.cfg.shared_dir + "/heartbeat").value_or("");
        CHECK(a == b, "heartbeat advanced while frozen (%s vs %s)", a.c_str(), b.c_str());
        CHECK(c->resume(), "resume");
        usleep(600000);
        const std::string d = read_file(in.cfg.shared_dir + "/heartbeat").value_or("");
        CHECK(d != b, "heartbeat did not resume");
        CHECK(c->status() == ContainerStatus::RUNNING, "status after resume");
    } else {
        printf("  (no freezer on this host, pause not tested)\n");
    }

    c->stop(5000);
    CHECK(c->status() == ContainerStatus::STOPPED, "stop -> %d", (int)c->status());
    CHECK(c->init_pid() == -1, "pid cleared");
    for (int i = 0; i < 20 && loops_using(in.dir) > 0; ++i) usleep(100000);
    CHECK(loops_using(in.dir) == 0, "loop devices left behind");
    CHECK(!path_exists("/sys/fs/cgroup/memory/vineos-boot1") && !path_exists("/sys/fs/cgroup/vineos-boot1"), "cgroup directory left behind");
    const std::string log = read_file(in.cfg.boot_log_path).value_or("");
    CHECK(log.find("fake-init: booted") != std::string::npos, "boot.log misses the guest console output:\n%s", log.c_str());
    CHECK(log.find("fake-init: stopping") != std::string::npos, "guest did not stop on SIGTERM");
    CHECK(mountinfo() == mounts_before, "host mounts changed after the guest stopped");
    m.remove_container(h);
}

void test_refuses_unpatched_init() {
    printf("[unpatched init is refused unless allowed]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance in = make_instance("unpatched", false);
    int64_t h = m.start_container(in.cfg);
    CHECK(h == 0, "an unpatched init must not start");
    CHECK(m.last_start_error().find("container aware") != std::string::npos, "message: %s", m.last_start_error().c_str());
    in.cfg.allow_unpatched_init = true;
    h = m.start_container(in.cfg);
    CHECK(h != 0, "allow_unpatched_init should start it: %s", m.last_start_error().c_str());
    if (h) {
        CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "unpatched init did not boot");
        m.stop_container(h);
        m.remove_container(h);
    }
}

void test_missing_pieces() {
    printf("[missing images give clear errors]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance in = make_instance("nosystem");
    unlink((in.dir + "/system.img").c_str());
    CHECK(m.start_container(in.cfg) == 0, "start without system.img");
    CHECK(m.last_start_error().find("system.img") != std::string::npos, "error: %s", m.last_start_error().c_str());
    Instance in2 = make_instance("noramdisk");
    unlink((in2.dir + "/ramdisk.img").c_str());
    CHECK(m.start_container(in2.cfg) == 0, "start without ramdisk");
    CHECK(m.last_start_error().find("ramdisk") != std::string::npos, "error: %s", m.last_start_error().c_str());
}

void test_crash_and_poweroff() {
    printf("[guest crash and guest-requested poweroff]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance in = make_instance("crash");
    mkdirs(in.cfg.shared_dir);
    write_file(in.cfg.shared_dir + "/crash", "1");
    int64_t h = m.start_container(in.cfg);
    CHECK(h != 0, "start: %s", m.last_start_error().c_str());
    if (h) {
        CHECK(wait_status(m, h, ContainerStatus::ERROR, 20), "crashing guest should end in ERROR");
        Container* c = m.get_container(h);
        CHECK(c->last_error().find("code 7") != std::string::npos, "error: %s", c->last_error().c_str());
        m.remove_container(h);
    }

    Instance in2 = make_instance("poweroff");
    h = m.start_container(in2.cfg);
    CHECK(h != 0, "start: %s", m.last_start_error().c_str());
    if (h) {
        CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "boot");
        write_file(in2.cfg.shared_dir + "/poweroff", "1");
        CHECK(wait_status(m, h, ContainerStatus::STOPPED, 15), "guest poweroff should end in STOPPED, status %d: %s",
              (int)m.get_container(h)->status(), m.get_container(h)->last_error().c_str());
        m.remove_container(h);
    }
}

void test_data_persistence_and_growth() {
    printf("[/data persists and grows]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance in = make_instance("data1");
    int64_t h = m.start_container(in.cfg);
    CHECK(h != 0, "start: %s", m.last_start_error().c_str());
    if (!h) return;
    CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "boot 1");
    auto r1 = read_report(in);
    const int small = atoi(r1["data_total_mb"].c_str());
    m.stop_container(h);
    m.remove_container(h);

    in.cfg.data_size_mb = 192;
    h = m.start_container(in.cfg);
    CHECK(h != 0, "restart: %s", m.last_start_error().c_str());
    if (!h) return;
    CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "boot 2");
    auto r2 = read_report(in);
    CHECK(r2["data_existed"] == "1", "file from the first boot is gone");
    const int big = atoi(r2["data_total_mb"].c_str());
    CHECK(big > small * 2, "storage did not grow: %d MB -> %d MB", small, big);
    // a "256 GB" disk must be a sparse file, not 256 GB of real blocks
    m.stop_container(h);
    m.remove_container(h);

    struct stat st{};
    stat((in.dir + "/data.img").c_str(), &st);
    CHECK((long long)st.st_blocks * 512 < 120LL * 1024 * 1024, "data.img uses %lld MB of real space", (long long)st.st_blocks * 512 / 1048576);
}

void test_directory_data_and_kill() {
    printf("[directory /data fallback, kill_now, restart]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance in = make_instance("dir1");
    in.cfg.data_size_mb = 0;
    int64_t h = m.start_container(in.cfg);
    CHECK(h != 0, "start: %s", m.last_start_error().c_str());
    if (!h) return;
    CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "boot");
    CHECK(path_exists(in.dir + "/data/written_by_guest"), "guest write should land in the host directory");
    m.kill_container(h);
    CHECK(m.get_container(h)->status() == ContainerStatus::STOPPED, "kill -> %d", (int)m.get_container(h)->status());
    m.remove_container(h);

    h = m.start_container(in.cfg);
    CHECK(h != 0, "restart after kill: %s", m.last_start_error().c_str());
    if (h) {
        CHECK(wait_status(m, h, ContainerStatus::RUNNING, 30), "boot after kill");
        m.stop_container(h);
        m.remove_container(h);
    }
}

void test_two_guests_side_by_side() {
    printf("[two guests at once]\n");
    NamespaceManager& m = NamespaceManager::instance();
    Instance a = make_instance("pair-a");
    Instance b = make_instance("pair-b");
    const int64_t ha = m.start_container(a.cfg);
    const int64_t hb = m.start_container(b.cfg);
    CHECK(ha != 0 && hb != 0, "start: %s", m.last_start_error().c_str());
    if (!ha || !hb) return;
    CHECK(m.get_container(ha)->config().net_slot != m.get_container(hb)->config().net_slot, "same network slot");
    CHECK(m.start_container(a.cfg) == 0, "starting a running instance twice must fail");
    CHECK(wait_status(m, ha, ContainerStatus::RUNNING, 30) && wait_status(m, hb, ContainerStatus::RUNNING, 30), "boot both");
    CHECK(read_report(a)["hostname"] == "vine-paira" && read_report(b)["hostname"] == "vine-pairb", "hostnames");
    CHECK(m.running_count() == 2, "running_count %zu", m.running_count());
    m.kill_container(ha);
    m.poll_all();
    CHECK(m.get_container(hb)->status() == ContainerStatus::RUNNING, "the other guest must keep running");
    m.stop_container(hb);
    m.remove_container(ha);
    m.remove_container(hb);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "needs root\n");
        return 2;
    }
    g_fixture = argv[1];
    char tmpl[] = "/tmp/vinetest.XXXXXX";
    g_work = mkdtemp(tmpl);
    NamespaceManager::instance().init(g_work, "");

    test_boot_isolation_and_cleanup();
    test_refuses_unpatched_init();
    test_missing_pieces();
    test_crash_and_poweroff();
    test_data_persistence_and_growth();
    test_directory_data_and_kill();
    test_two_guests_side_by_side();

    NamespaceManager::instance().shutdown();
    remove_recursive(g_work);
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
