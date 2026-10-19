#include "host_probe.h"
#include "namespace_probe.h"
#include "../utils/vine_utils.h"

#include <sys/utsname.h>
#include <unistd.h>

namespace vine::diagnostics {

HostCapabilities probe_host() {
    HostCapabilities c;
    const NamespaceProbeResult ns = probe_namespace_support();
    c.uid = ns.caller_uid;
    c.unshare_ok = ns.unshare_ok;
    c.mount_ok = ns.mount_ok;
    c.unshare_errno = ns.unshare_errno;
    c.mount_errno = ns.mount_errno;

    c.loop_control = access("/dev/loop-control", F_OK) == 0;
    const std::string devices = read_file("/proc/devices").value_or("");
    c.loop_supported = c.loop_control || devices.find(" loop\n") != std::string::npos;
    c.binderfs = kernel_has_filesystem("binder");
    c.ashmem = path_exists("/dev/ashmem");

    const std::string mounts = read_file("/proc/self/mounts").value_or("");
    for (const auto& line : split_lines(mounts)) {
        const auto f = split_fields(line, ' ');
        if (f.size() < 4) continue;
        if (f[2] == "cgroup2") c.cgroup_v2 = true;
        if (f[2] == "cgroup") {
            c.cgroup_v1 = true;
            for (const auto& opt : split_fields(f[3], ',')) c.freezer = c.freezer || opt == "freezer";
        }
    }
    c.freezer = c.freezer || c.cgroup_v2; // cgroup2 has cgroup.freeze from kernel 5.2 on, checked again when used

    c.mke2fs = !find_executable("mke2fs").empty();
    c.resize2fs = !find_executable("resize2fs").empty();
    c.ip_tool = !find_executable("ip").empty();
    c.iptables = !find_executable("iptables").empty();
    c.aarch32 = host_supports_aarch32();

    const std::string enforce = read_file("/sys/fs/selinux/enforce").value_or("");
    c.selinux = enforce.empty() ? (path_exists("/sys/fs/selinux") ? "unknown" : "disabled")
                                : (enforce[0] == '1' ? "enforcing" : "permissive");
    struct utsname u{};
    if (uname(&u) == 0) c.kernel = u.release;
    return c;
}

std::string capabilities_to_text(const HostCapabilities& c) {
    std::string out;
    auto b = [&](const char* k, bool v) { out += std::string(k) + "=" + (v ? "1" : "0") + "\n"; };
    out += "uid=" + std::to_string(c.uid) + "\n";
    b("unshare", c.unshare_ok);
    b("private_mounts", c.mount_ok);
    out += "unshare_errno=" + std::to_string(c.unshare_errno) + "\n";
    out += "mount_errno=" + std::to_string(c.mount_errno) + "\n";
    b("loop", c.loop_supported);
    b("binderfs", c.binderfs);
    b("ashmem", c.ashmem);
    b("cgroup_v1", c.cgroup_v1);
    b("cgroup_v2", c.cgroup_v2);
    b("freezer", c.freezer);
    b("mke2fs", c.mke2fs);
    b("resize2fs", c.resize2fs);
    b("ip", c.ip_tool);
    b("iptables", c.iptables);
    b("aarch32", c.aarch32);
    out += "selinux=" + c.selinux + "\n";
    out += "kernel=" + c.kernel + "\n";
    return out;
}

} // namespace vine::diagnostics
