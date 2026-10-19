#pragma once

#include <string>
#include <vector>

namespace vine {

// What a guest learns about its host, written to boot.prop, cmdline and default.prop
struct BootParams {
    std::string instance_id;
    std::string hardware;   // androidboot.hardware, empty if unknown
    int display_width = 1080;
    int display_height = 1920;
    int display_dpi = 420;
    int ram_mb = 1024;
    bool rooted = false;
    bool qemu_32bit = false;
    // Filled only when the guest gets a private network; empty strings mean "no eth0 configured for you".
    std::string net_ip;
    std::string net_gateway;
    std::string net_dns;
};

// Hardware name from an init.<name>.rc in root_dir (vine, ranchu, goldfish), or ""
std::string detect_hardware(const std::string& root_dir);

std::string build_cmdline(const BootParams& p);
std::string build_boot_prop(const BootParams& p);

// Replaces or appends the # vine-begin / # vine-end block of a property file
bool write_managed_block(const std::string& path, const std::vector<std::string>& lines);

// True if the file contains the byte string within its first max_bytes. Used to find the init patch marker.
bool file_contains(const std::string& path, const std::string& needle, size_t max_bytes);

// Marker a container aware init must embed, an unpatched init would load its SELinux policy into the phone
constexpr const char* kInitMarker = "VINE_CONTAINER_INIT_V1";

} // namespace vine
