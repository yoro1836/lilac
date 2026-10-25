#include "rootless_prep.h"
#include "ext4_extract.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vine::rootless {

namespace {

const char* const kStampVersion = "vine-rootfs 1";

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

bool fail_errno(std::string* error, const std::string& what) {
    return fail(error, what + ": " + strerror(errno));
}

// What the stamp records about the images a rootfs came from
std::string image_stamp(const Layout& l) {
    std::string s = std::string(kStampVersion) + "\n";
    for (const char* name : {"system.img", "vendor.img"}) {
        struct stat st{};
        if (stat((l.instance + "/" + name).c_str(), &st) != 0) continue;
        s += std::string(name) + " " + std::to_string((long long)st.st_size) + " " + std::to_string((long long)st.st_mtime) + "\n";
    }
    return s;
}

bool remove_if_exists(const std::string& path, std::string* error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return true;
    return remove_recursive(path) || fail(error, "could not remove " + path);
}

bool write_mode(const std::string& path, const std::string& text, mode_t mode, std::string* error) {
    if (!write_file_atomic(path, text, mode)) return fail(error, "could not write " + path);
    // init skips group/world-writable rc files whatever the umask did
    return chmod(path.c_str(), mode) == 0 || fail_errno(error, "chmod " + path);
}

bool copy_mode(const std::string& src, const std::string& dst, mode_t mode, std::string* error) {
    const std::string tmp = dst + ".tmp";
    if (!copy_file_sparse(src, tmp)) return fail(error, "could not copy " + src);
    if (chmod(tmp.c_str(), mode) != 0 || rename(tmp.c_str(), dst.c_str()) != 0) {
        unlink(tmp.c_str());
        return fail_errno(error, "install " + dst);
    }
    return true;
}

bool symlink_over(const std::string& target, const std::string& path, std::string* error) {
    unlink(path.c_str());
    return symlink(target.c_str(), path.c_str()) == 0 || fail_errno(error, "symlink " + path);
}

bool mkdirs_or_fail(const std::string& path, std::string* error) {
    return mkdirs(path) || fail(error, "could not create " + path);
}

// The rc that adapts init's services to an app sandbox
std::string rootless_rc(bool own_audio_hal) {
    std::string rc =
        // lmkd needs PSI or memcg, both closed to apps (the host's lmkd
        // covers us): a stand-in answers system_server on its socket
        "service lmkd /system/bin/vine_lmkd\n"
        "    class core\n"
        "    user lmkd\n"
        "    group lmkd system readproc\n"
        "    capabilities DAC_OVERRIDE KILL IPC_LOCK SYS_NICE SYS_RESOURCE\n"
        "    socket lmkd seqpacket+passcred 0660 system system\n"
        "    task_profiles ServiceCapacityLow\n"
        "    override\n"
        "\n"
        // No keychords: init would open the input FIFOs and block until
        // the hwcomposer writes one
        "service bugreport /system/bin/dumpstate -d -p -z\n"
        "    oneshot\n"
        "    disabled\n"
        "    override\n"
        "\n"
        // The engine's boot signal, in its /run/user/1000
        "on property:sys.boot_completed=1\n"
        "    write /run/user/1000/boot_completed 1\n";
    if (own_audio_hal) {
        // The image's audio HAL service is 32-bit only; its 64-bit
        // implementations are served by our passthrough service
        rc += "\n"
              "service vendor.audio-hal /vendor/bin/hw/vine_hal_service "
              "android.hardware.audio@4.0::IDevicesFactory android.hardware.audio.effect@4.0::IEffectsFactory\n"
              "    class hal\n"
              "    user audioserver\n"
              "    group audio camera drmrpc inet media mediadrm net_bt net_bt_admin net_bw_acct wakelock context_hub\n"
              "    capabilities BLOCK_SUSPEND SYS_NICE\n"
              "    onrestart restart audioserver\n"
              "    override\n";
    }
    return rc;
}

// What Waydroid's host writes: no GPU device, so software GLES (ANGLE on
// lavapipe) over memfd-backed gralloc buffers
std::string waydroid_prop(int dpi) {
    return "ro.hardware.gralloc=default\n"
           "ro.hardware.egl=angle\n"
           "ro.hardware.vulkan=lvp\n"
           "sys.use_memfd=true\n"
           "ro.opengles.version=196610\n"
           "ro.sf.lcd_density=" + std::to_string(dpi) + "\n"
           "waydroid.background_start=false\n"
           "waydroid.stub_sensors_hal=1\n";
}

// 64-bit-only zygote: the 64_32 one without its 32-bit secondary
std::string zygote64_rc(const std::string& zygote64_32) {
    const size_t at = zygote64_32.find("\nservice zygote_secondary");
    return at == std::string::npos ? zygote64_32 : zygote64_32.substr(0, at + 1);
}

} // namespace

Layout layout_for(const std::string& instance_path) {
    Layout l;
    l.instance = instance_path;
    l.work = instance_path + "/rootless";
    l.rootfs = l.work + "/rootfs";
    l.store = l.work + "/tawcroot";
    l.binderfs = l.work + "/binderfs";
    l.data = l.work + "/data";
    l.run = l.work + "/run";
    l.input = l.work + "/input";
    l.logs = l.work + "/logs";
    l.vinefb = instance_path + "/shared/vinefb";
    return l;
}

std::string props_64bit_only(const std::string& build_prop) {
    std::string out;
    size_t pos = 0;
    while (pos < build_prop.size()) {
        size_t nl = build_prop.find('\n', pos);
        const bool has_nl = nl != std::string::npos;
        std::string line = build_prop.substr(pos, has_nl ? nl - pos : std::string::npos);
        pos = has_nl ? nl + 1 : build_prop.size();
        const size_t eq = line.find('=');
        const std::string key = eq == std::string::npos ? "" : line.substr(0, eq);
        // ro.<partition>.product.cpu.abilist[32], partition in [a-z]*
        size_t p = 3;
        while (key.rfind("ro.", 0) == 0 && p < key.size() && key[p] >= 'a' && key[p] <= 'z') ++p;
        const std::string rest = key.rfind("ro.", 0) == 0 ? key.substr(p) : "";
        if (rest == ".product.cpu.abilist") line = key + "=arm64-v8a";
        else if (rest == ".product.cpu.abilist32") line = key + "=";
        else if (key == "ro.zygote") line = "ro.zygote=zygote64";
        out += line;
        if (has_nl) out += "\n";
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_environ_rc(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto& raw : split_lines(text)) {
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos || raw.compare(a, 7, "export ") != 0) continue;
        a = raw.find_first_not_of(" \t", a + 7);
        if (a == std::string::npos) continue;
        const size_t b = raw.find_first_of(" \t", a);
        if (b == std::string::npos) continue;
        const size_t c = raw.find_first_not_of(" \t", b);
        if (c == std::string::npos) continue;
        const size_t d = raw.find_last_not_of(" \t");
        env.emplace_back(raw.substr(a, b - a), raw.substr(c, d + 1 - c));
    }
    return env;
}

bool extract_rootfs(const Layout& l, std::string* error) {
    const std::string stamp_path = l.work + "/rootfs.stamp";
    const std::string stamp = image_stamp(l);
    if (is_directory(l.rootfs) && read_file(stamp_path).value_or("") == stamp) return true;
    if (!is_regular_file(l.instance + "/system.img")) return fail(error, "this instance has no system.img");

    unlink(stamp_path.c_str());
    if (!remove_if_exists(l.rootfs, error) || !remove_if_exists(l.store, error) || !remove_if_exists(l.data, error)) return false;
    if (!mkdirs_or_fail(l.rootfs, error) || !mkdirs_or_fail(l.store, error)) return false;
    const std::string owners = l.store + "/owners";
    char err[512];
    if (vine_ext4_extract((l.instance + "/system.img").c_str(), l.rootfs.c_str(), owners.c_str(), err, sizeof err) != 0) {
        return fail(error, std::string("system.img: ") + err);
    }
    if (is_regular_file(l.instance + "/vendor.img")) {
        if (!mkdirs_or_fail(l.rootfs + "/vendor", error)) return false;
        if (vine_ext4_extract((l.instance + "/vendor.img").c_str(), (l.rootfs + "/vendor").c_str(), owners.c_str(), err, sizeof err) != 0) {
            return fail(error, std::string("vendor.img: ") + err);
        }
    }
    if (!is_regular_file(l.rootfs + "/system/bin/init")) return fail(error, "system.img is not a system-as-root image (no /system/bin/init)");
    return write_file_atomic(stamp_path, stamp) || fail(error, "could not write " + stamp_path);
}

bool prepare_rootfs(const Layout& l, const PrepOptions& o, std::string* error) {
    const std::string& R = l.rootfs;

    // Flattened APEXes, where /apex/<name> is expected
    if (DIR* d = opendir((R + "/system/apex").c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st{};
            const std::string link = R + "/apex/" + n;
            if (lstat(link.c_str(), &st) != 0 && symlink(("/system/apex/" + n).c_str(), link.c_str()) != 0) {
                closedir(d);
                return fail_errno(error, "symlink " + link);
            }
        }
        closedir(d);
    }

    if (!remove_if_exists(l.binderfs, error)) return false;
    for (const auto& dir : {l.binderfs, l.data, l.run, l.logs, l.instance + "/shared"}) {
        if (!mkdirs_or_fail(dir, error)) return false;
    }

    // The previous boot's runtime state: the guest's /dev, tawcroot's
    // per-session store files (the ownership table stays), the boot signal
    for (const char* n : {"null", "zero", "random", "urandom", "kmsg"}) {
        const std::string p = R + "/dev/" + n;
        if (is_regular_file(p)) unlink(p.c_str());
    }
    for (const char* n : {"__properties__", "cgroup_info", ".booting", "socket", "pts", "input"}) {
        if (!remove_if_exists(R + "/dev/" + n, error)) return false;
    }
    for (const char* n : {"mounts", "mounts.event", "mnt", "cgroup", "cgroup.registry", "uts", "ids"}) {
        if (!remove_if_exists(l.store + "/" + n, error)) return false;
    }
    unlink((l.run + "/boot_completed").c_str());
    if (!write_file(R + "/dev/kmsg", "")) return fail(error, "could not create /dev/kmsg");

    // A host without AArch32 can't run the ROM's 32-bit ABIs: present it
    // as 64-bit only, like the host
    const std::string hal_service = o.lib_dir + "/libvine_hal_service.so";
    const bool own_audio_hal = !o.host_has_32bit && is_regular_file(hal_service);
    if (!o.host_has_32bit) {
        for (const char* f : {"/system/build.prop", "/vendor/build.prop", "/vendor/odm/etc/build.prop"}) {
            const auto text = read_file(R + f);
            if (!text) continue;
            const std::string edited = props_64bit_only(*text);
            if (edited != *text && !write_mode(R + f, edited, 0644, error)) return false;
        }
        const std::string z = R + "/system/etc/init/hw";
        if (!is_regular_file(z + "/init.zygote64.rc")) {
            const auto z6432 = read_file(z + "/init.zygote64_32.rc");
            if (!z6432) return fail(error, "the image has neither init.zygote64.rc nor init.zygote64_32.rc");
            if (!write_mode(z + "/init.zygote64.rc", zygote64_rc(*z6432), 0644, error)) return false;
        }
        if (own_audio_hal && !copy_mode(hal_service, R + "/vendor/bin/hw/vine_hal_service", 0755, error)) return false;
    }
    if (!copy_mode(o.lib_dir + "/libvine_lmkd.so", R + "/system/bin/vine_lmkd", 0755, error)) return false;
    // $R/product is an absolute symlink, its files are in system/product
    const std::string product_init = R + "/system/product/etc/init";
    if (!mkdirs_or_fail(product_init, error) || !write_mode(product_init + "/vine-rootless.rc", rootless_rc(own_audio_hal), 0644, error)) return false;
    if (!write_mode(R + "/vendor/waydroid.prop", waydroid_prop(o.dpi), 0644, error)) return false;

    // A first boot has no app ops recorded, and LineageOS 20's SystemUI
    // then crashes on getPackagesForOps() returning null. Seed one that
    // persists: SYSTEM_ALERT_WINDOW allowed for the system package.
    const std::string appops = l.data + "/system/appops.xml";
    if (!path_exists(appops)) {
        if (!mkdirs_or_fail(l.data + "/system", error)) return false;
        if (!write_file(appops,
                        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<app-ops v=\"1\">\n<pkg n=\"android\">\n"
                        "<uid n=\"1000\">\n<op n=\"24\" m=\"0\" />\n</uid>\n</pkg>\n</app-ops>\n")) {
            return fail(error, "could not write " + appops);
        }
    }

    // libbinder access()es the driver before opening it
    for (const char* b : {"binder", "hwbinder", "vndbinder"}) {
        if (!symlink_over(std::string("binderfs/") + b, R + "/dev/" + b, error)) return false;
    }
    // What first-stage init would have created; /dev/input empty for the
    // hwcomposer's FIFOs
    for (const char* d : {"/dev/socket", "/dev/pts", "/dev/input", "/mnt/vendor", "/mnt/product", "/run/user/1000"}) {
        if (!mkdirs_or_fail(R + d, error)) return false;
    }
    return mkdirs_or_fail(l.run + "/pulse", error);
}

} // namespace vine::rootless
