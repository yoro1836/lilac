#include "rootless_prep.h"
#include "ext4_extract.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
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

// `rc` with `option` added to `service`'s definition (after its
// backslash-continued first line); unchanged if it has no such service
std::string with_service_option(const std::string& rc, const std::string& service, const std::string& option) {
    const std::string head = "service " + service + " ";
    size_t at = rc.compare(0, head.size(), head) == 0 ? 0 : rc.find("\n" + head);
    if (at == std::string::npos) return rc;
    if (at) ++at;
    size_t end = rc.find('\n', at);
    while (end != std::string::npos && end > 0 && rc[end - 1] == '\\') end = rc.find('\n', end + 1);
    if (end == std::string::npos) return rc + "\n    " + option + "\n";
    return rc.substr(0, end + 1) + "    " + option + "\n" + rc.substr(end + 1);
}

// The rc that adapts init's services to an app sandbox
std::string rootless_rc(bool own_audio_hal, bool gpu, int ram_mb) {
    std::string rc =
        // lmkd needs PSI or memcg, both closed to apps: vine_lmkd kills
        // by system_server's priorities, within the instance's RAM
        "service lmkd /system/bin/vine_lmkd" + (ram_mb > 0 ? " --budget-mb " + std::to_string(ram_mb) : std::string()) + "\n"
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
        // Waydroid's statsd can't find libstatssocket (APEX library
        // namespaces) and would restart every few seconds (init.rc
        // starts it by name, so `disabled` alone is not enough)
        "service statsd /apex/com.android.os.statsd/bin/statsd\n"
        "    class main\n"
        "    disabled\n"
        "    oneshot\n"
        "    override\n"
        "\n"
        // llvmpipe rasterizes in the drawing thread: with its own
        // rasterizer threads, now and then one runs a shader whose JIT
        // code was already released (SEGV_ACCERR in llvmpipe-N), which
        // takes SurfaceFlinger or an app down
        "on early-init\n"
        "    export LP_NUM_THREADS 0\n"
        // The audio HAL's libpulse takes PULSE_RUNTIME_PATH only when it
        // is its own and 0700, or it never connects to vine_pulse
        "    chown audioserver audio /run/user/1000/pulse\n"
        "    chmod 0700 /run/user/1000/pulse\n";
    if (gpu) {
        // gfxstream-vk talks to vine_kumquat over kumquat's socket
        rc += "    export VIRTGPU_KUMQUAT 1\n"
              "    export VIRTGPU_KUMQUAT_GPU_SOCKET /run/user/1000/kumquat-gpu-0\n";
    }
    rc += "\n"
        // Emulated storage without FUSE (libvine_vold_shim fakes vold's
        // FUSE mount): /data/media where FUSE and its pass-through would
        // be, before zygote forks apps that bind /mnt/user/0 onto /storage
        "on post-fs-data\n"
        "    mkdir /mnt/user/0 0710 shell everybody\n"
        "    mkdir /mnt/user/0/emulated 0700 root root\n"
        "    mkdir /mnt/pass_through/0 0710 root media_rw\n"
        "    mkdir /mnt/pass_through/0/emulated 0710 root media_rw\n"
        "    mount none /data/media /mnt/user/0/emulated bind rec\n"
        "    mount none /data/media /mnt/pass_through/0/emulated bind rec\n"
        "    mount none /mnt/user/0 /storage bind rec\n"
        "\n"
        // Without FUSE nothing tells MediaProvider what apps write
        "service vine_mediascan /system/bin/vine_mediascan /data/media/0 /storage/emulated/0\n"
        "    user root\n"
        "    group media_rw\n"
        "    disabled\n"
        "\n"
        // The engine's boot signal, in its /run/user/1000
        "on property:sys.boot_completed=1\n"
        "    write /run/user/1000/boot_completed 1\n"
        "    start vine_mediascan\n";
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

// What Waydroid's host writes with no GPU device: software GLES (ANGLE on
// lavapipe) over memfd-backed gralloc buffers. With the GPU, ANGLE runs
// on gfxstream-vk and gralloc hands out host ColorBuffers. gralloc "vine"
// (not "default") makes the hwcomposer pass buffers on by handle over
// android_wlegl, so frames stay on the GPU. zygote must not preload the
// driver, or its kumquat socket leaks into every fork and zygote's fd
// check aborts.
std::string waydroid_prop(int dpi, bool gpu) {
    return std::string(gpu ? "ro.hardware.gralloc=vine\n" : "ro.hardware.gralloc=default\n") +
           "ro.hardware.egl=angle\n" +
           (gpu ? "ro.hardware.vulkan=gfxstream\nro.vendor.vine.gpu=1\nro.zygote.disable_gl_preload=true\n" : "ro.hardware.vulkan=lvp\n") +
           "sys.use_memfd=true\n"
           // Codec2 takes linear buffers from DMA-BUF heaps, which apps
           // can't open: gralloc blobs instead (ION | BUFFERQUEUE | BLOB)
           "debug.stagefright.c2-poolmask=851968\n"
           // The phone's own volume sets the loudness; a guest volume
           // below max only attenuates twice
           "ro.config.media_vol_default=15\n"
           // The hwcomposer releases a frame as soon as the next one is
           // set, but the host still shows it until that one lands:
           // with SurfaceFlinger's 2 framebuffers the guest draws into the
           // frame on screen (tearing). 4 give it 2 frames to land.
           "ro.surface_flinger.max_frame_buffer_acquired_buffers=4\n"
           "ro.opengles.version=196610\n"
           "ro.sf.lcd_density=" + std::to_string(dpi) + "\n"
           "waydroid.background_start=false\n"
           "waydroid.stub_sensors_hal=1\n";
}

// True for a 32-bit ELF file
// java.io.DataOutputStream's writeUTF / writeInt
void put_utf(std::string& out, const std::string& s) {
    out += char(s.size() >> 8);
    out += char(s.size() & 0xff);
    out += s;
}

void put_int(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += char((v >> shift) & 0xff);
}

} // namespace

// The fake eth0's static configuration, in IpConfigStore's format:
// EthernetTracker brings eth0 up with it, no DHCP needed.
std::string ethernet_ipconfig() {
    std::string out;
    put_int(out, 3);
    put_utf(out, "ipAssignment");
    put_utf(out, "STATIC");
    put_utf(out, "linkAddress");
    put_utf(out, "10.0.2.15");
    put_int(out, 24);
    put_utf(out, "gateway");
    put_int(out, 0); // default route
    put_int(out, 1);
    put_utf(out, "10.0.2.2");
    for (const char* dns : {"8.8.8.8", "1.1.1.1"}) {
        put_utf(out, "dns");
        put_utf(out, dns);
    }
    put_utf(out, "proxySettings");
    put_utf(out, "NONE");
    put_utf(out, "id");
    put_utf(out, "eth0");
    put_utf(out, "eos");
    return out;
}

namespace {

bool is_elf32(const std::string& path) {
    const auto head = read_file_prefix(path, 5);
    return head && head->size() == 5 && head->compare(0, 4, "\x7f" "ELF") == 0 && (*head)[4] == 1;
}

// The HAL a vendor service binary serves, from its name:
// android.hardware.media.omx@1.0-service -> android.hardware.media.omx
std::string hal_of_binary(const std::string& path) {
    std::string base = path.substr(path.rfind('/') + 1);
    const size_t at = base.find('@');
    if (at != std::string::npos) return base.substr(0, at);
    const size_t svc = base.find("-service");
    return svc == std::string::npos ? base : base.substr(0, svc);
}

// The manifest without the <hal> entries named `hal`
std::string drop_manifest_hal(const std::string& xml, const std::string& hal) {
    std::string out;
    size_t pos = 0;
    for (;;) {
        const size_t start = xml.find("<hal", pos);
        if (start == std::string::npos) break;
        const size_t end = xml.find("</hal>", start);
        if (end == std::string::npos) break;
        const size_t stop = end + 6;
        const std::string block = xml.substr(start, stop - start);
        const bool drop = block.find("<name>" + hal + "</name>") != std::string::npos;
        size_t line_start = start;
        while (line_start > pos && (xml[line_start - 1] == ' ' || xml[line_start - 1] == '\t')) --line_start;
        out += xml.substr(pos, (drop ? line_start : stop) - pos);
        pos = stop;
        if (drop && pos < xml.size() && xml[pos] == '\n') ++pos;
    }
    return out + xml.substr(pos);
}

bool drop_vendor_hals(const std::string& R, const std::vector<std::string>& hals, std::string* error);

// Services the host can't run (32-bit only binaries, e.g. mediaserver and
// the omx HAL): their rc files go, and for vendor HALs their VINTF entries
// too, so nothing waits for them (system_server waits forever for a
// declared IOmxStore) and init does not restart them over and over.
// `keep` is served by our own service instead.
bool drop_32bit_services(const std::string& R, const std::string& keep, std::string* error) {
    std::vector<std::string> hals;
    for (const char* dir : {"/system/etc/init", "/vendor/etc/init"}) {
    const bool vendor = dir[1] == 'v';
    const std::string init_dir = R + dir;
    if (DIR* d = opendir(init_dir.c_str())) {
        std::vector<std::string> rcs;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".rc") == 0) rcs.push_back(n);
        }
        closedir(d);
        for (const auto& n : rcs) {
            const std::string rc = init_dir + "/" + n;
            bool any = false, all32 = true;
            std::vector<std::string> rc_hals;
            for (const auto& line : split_lines(read_file(rc).value_or(""))) {
                char name[128], bin[256];
                if (sscanf(line.c_str(), " service %127s %255s", name, bin) != 2) continue;
                any = true;
                if (!is_elf32(R + bin)) {
                    all32 = false;
                    continue;
                }
                const std::string hal = hal_of_binary(bin);
                if (hal == keep) all32 = false;
                else rc_hals.push_back(hal);
            }
            if (!any || !all32) continue;
            if (unlink(rc.c_str()) != 0) return fail_errno(error, "remove " + rc);
            if (vendor) hals.insert(hals.end(), rc_hals.begin(), rc_hals.end());
        }
    }
    }
    return drop_vendor_hals(R, hals, error);
}

// Removes HALs from the vendor VINTF manifests
bool drop_vendor_hals(const std::string& R, const std::vector<std::string>& hals, std::string* error) {
    if (hals.empty()) return true;
    const std::string vintf = R + "/vendor/etc/vintf";
    std::vector<std::string> manifests = {vintf + "/manifest.xml"};
    if (DIR* d = opendir((vintf + "/manifest").c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') manifests.push_back(vintf + "/manifest/" + e->d_name);
        closedir(d);
    }
    for (const auto& m : manifests) {
        const auto xml = read_file(m);
        if (!xml) continue;
        std::string edited = *xml;
        for (const auto& hal : hals) edited = drop_manifest_hal(edited, hal);
        if (edited != *xml && !write_mode(m, edited, 0644, error)) return false;
    }
    return true;
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
    // Only for images whose audio HAL service is 32-bit (64_32 builds;
    // waydroid_arm64_only ships a 64-bit one)
    const bool own_audio_hal = !o.host_has_32bit && is_regular_file(hal_service) &&
                               is_elf32(R + "/vendor/bin/hw/android.hardware.audio.service");
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
        if (!drop_32bit_services(R, own_audio_hal ? "android.hardware.audio.service" : "", error)) return false;
    }
    // waydroid_arm64_only declares the omx HAL but has no service for it:
    // mediaserver then waits for IOmx forever, and with it everything that
    // lists codecs (MediaProvider never comes up, ringtones hang Settings)
    if (!is_regular_file(R + "/vendor/bin/hw/android.hardware.media.omx@1.0-service") &&
        !drop_vendor_hals(R, {"android.hardware.media.omx"}, error))
        return false;
    if (!copy_mode(o.lib_dir + "/libvine_lmkd.so", R + "/system/bin/vine_lmkd", 0755, error) ||
        !copy_mode(o.lib_dir + "/libvine_mediascan.so", R + "/system/bin/vine_mediascan", 0755, error))
        return false;
    // vold mounts emulated storage only through FUSE: the shim fakes that
    // part over the binds vine-rootless.rc makes
    const std::string vold_rc = R + "/system/etc/init/vold.rc";
    if (const auto text = read_file(vold_rc)) {
        if (!mkdirs_or_fail(R + "/system/lib64", error) ||
            !copy_mode(o.lib_dir + "/libvine_vold_shim.so", R + "/system/lib64/libvine_vold_shim.so", 0644, error) ||
            (text->find("libvine_vold_shim") == std::string::npos &&
             !write_mode(vold_rc, with_service_option(*text, "vold", "setenv LD_PRELOAD /system/lib64/libvine_vold_shim.so"), 0644, error)))
            return false;
    }
    // The padded gralloc takes gralloc.default's place (the hwcomposer and
    // Mesa tell gralloc kinds apart by ro.hardware.gralloc) and loads it
    // under another name
    const std::string hw = R + "/vendor/lib64/hw";
    if (is_regular_file(hw + "/gralloc.default.so") && !is_regular_file(hw + "/gralloc.default_real.so") &&
        rename((hw + "/gralloc.default.so").c_str(), (hw + "/gralloc.default_real.so").c_str()) != 0)
        return fail_errno(error, "rename gralloc.default.so");
    if (is_regular_file(hw + "/gralloc.default_real.so") &&
        !copy_mode(o.lib_dir + "/libvine_gralloc.so", hw + "/gralloc.default.so", 0644, error))
        return false;
    // $R/product is an absolute symlink, its files are in system/product
    const std::string product_init = R + "/system/product/etc/init";
    if (!mkdirs_or_fail(product_init, error) || !write_mode(product_init + "/vine-rootless.rc", rootless_rc(own_audio_hal, o.gpu, o.ram_mb), 0644, error)) return false;
    if (!write_mode(R + "/vendor/waydroid.prop", waydroid_prop(o.dpi, o.gpu), 0644, error)) return false;
    if (o.gpu && (!copy_mode(o.lib_dir + "/libvine_vk_gfxstream.so", hw + "/vulkan.gfxstream.so", 0644, error) ||
                  !copy_mode(o.lib_dir + "/libvine_gralloc.so", hw + "/gralloc.vine.so", 0644, error)))
        return false;

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

    // Read from the tethering apex's dir, migrated from the legacy one
    for (const char* dir : {"/misc/ethernet", "/misc/apexdata/com.android.tethering/misc/ethernet"}) {
        const std::string d = l.data + dir;
        if (!mkdirs_or_fail(d, error) || !write_mode(d + "/ipconfig.txt", ethernet_ipconfig(), 0644, error)) return false;
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
