#pragma once

#include <string>

namespace vine::diagnostics {

// What the phone offers the engine, read without side effects apart from a disposable namespace probe child
struct HostCapabilities {
    int uid = -1;
    bool unshare_ok = false;
    bool mount_ok = false;
    int unshare_errno = 0;
    int mount_errno = 0;
    bool loop_control = false;     // /dev/loop-control exists
    bool loop_supported = false;   // "loop" shows in /proc/devices or loop-control exists
    bool binderfs = false;         // the kernel can create private binder devices
    bool ashmem = false;
    bool cgroup_v1 = false;
    bool cgroup_v2 = false;
    bool freezer = false;          // pausing a guest is possible
    bool mke2fs = false;
    bool resize2fs = false;
    bool ip_tool = false;
    bool iptables = false;
    bool aarch32 = false;
    std::string selinux;           // "enforcing", "permissive", "disabled" or "unknown"
    std::string kernel;
};

HostCapabilities probe_host();

// "key=value" lines, one per capability, for the helper protocol and the diagnostics screen.
std::string capabilities_to_text(const HostCapabilities& c);

} // namespace vine::diagnostics
