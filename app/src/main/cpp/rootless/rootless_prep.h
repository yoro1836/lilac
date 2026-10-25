#pragma once

#include <string>
#include <utility>
#include <vector>

// Rootfs preparation for a rootless (Waydroid) guest: what Waydroid's
// host and first-stage init would have done, see docs/rootless-boot.md.
namespace vine::rootless {

// Where one instance keeps its rootless state, all under the instance dir
struct Layout {
    std::string instance;   // <instance>: system.img, vendor.img, shared/
    std::string work;       // <instance>/rootless
    std::string rootfs;     // work/rootfs, the extracted images
    std::string store;      // work/tawcroot, tawcroot's store (<rootfs>/../tawcroot)
    std::string binderfs;   // binderd's directory, /dev/binderfs in the guest
    std::string data;       // the guest's /data
    std::string run;        // /run/user/1000: wayland-0, pulse/native, boot_completed
    std::string input;      // vine_wl's input socket
    std::string logs;       // one log per process
    std::string vinefb;     // <instance>/shared/vinefb, what the app displays
};

Layout layout_for(const std::string& instance_path);

struct PrepOptions {
    std::string lib_dir;    // where libvine_lmkd.so and libvine_hal_service.so are
    int dpi = 320;
    bool host_has_32bit = false;
    bool gpu = false;       // render through vine_kumquat (gfxstream)
    int ram_mb = 0;         // vine_lmkd's budget for the guest's apps, 0: none
};

// Extracts system.img (and vendor.img into /vendor) unless the rootfs
// already holds these images. A fresh extraction also starts /data and
// the ownership table over: the old table's inode keys mean nothing for
// the new files.
bool extract_rootfs(const Layout& l, std::string* error);

// Readies an extracted rootfs for a boot. Run before every boot: it also
// clears the previous boot's runtime state.
bool prepare_rootfs(const Layout& l, const PrepOptions& o, std::string* error);

// `ro.<partition>.product.cpu.abilist*` set to 64-bit only and
// `ro.zygote=zygote64`; other lines unchanged.
std::string props_64bit_only(const std::string& build_prop);

// `export NAME VALUE` lines of an init.environ.rc
std::vector<std::pair<std::string, std::string>> parse_environ_rc(const std::string& text);

} // namespace vine::rootless
