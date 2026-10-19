#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

#include "net_setup.h"

namespace vine {

// Config of one container, everything lives under instance_path (layout in ARCHITECTURE.md)
struct ContainerConfig {
    std::string instance_id;
    std::string instance_path;
    std::string rootfs_dir;
    std::string ramdisk_image;
    std::string system_image;
    std::string vendor_image;
    std::string data_image;
    std::string data_dir;
    std::string shared_dir;
    std::string boot_log_path;
    std::string hardware;            // androidboot.hardware, empty = detect from init.*.rc
    std::string mke2fs_path;         // empty = search the usual places
    int ram_mb = 1024;
    int cpu_cores = 0;               // 0 = unlimited, otherwise a cgroup CPU quota
    int data_size_mb = 4096;         // size of a newly created data.img, 0 = use the data/ directory
    int display_width = 1080;
    int display_height = 1920;
    int display_dpi = 420;
    std::string network = "nat";     // "nat" (private network behind the host), "none" (loopback only) or "host" (shares the phone's network, risky)
    int net_slot = 0;                // 1..250 for "nat", assigned by NamespaceManager
    bool needs_qemu_32bit = false;   // True when host lacks AArch32 support
    std::string qemu_arm_path;       // Path to the static qemu-arm binary
    bool rooted = false;
    bool allow_unpatched_init = false;
    int owner_uid = -1;              // owner of shared/ files so the app process can read them, -1 = leave as root
    int owner_gid = -1;
    int boot_timeout_s = 120;        // after this long without a boot signal an alive guest is treated as running
};

// Fills in every path field from instance_path using the layout above.
ContainerConfig make_container_config(const std::string& instance_id, const std::string& instance_path);

enum class ContainerStatus {
    STOPPED = 0,
    BOOTING = 1,
    RUNNING = 2,
    ERROR   = 3,
    PAUSED  = 4,
};

// Lifecycle of one guest: start() prepares the instance and forks a supervisor that runs init as PID 1
class Container {
public:
    explicit Container(ContainerConfig config);
    ~Container();

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    // Returns true once init has been exec'd (the guest keeps booting afterwards); on failure last_error() says why.
    bool start();

    // SIGTERM to init, then SIGKILL after timeout_ms. Waits for the supervisor to exit.
    void stop(int timeout_ms = 4000);

    // SIGKILL immediately.
    void kill_now();

    // Freezes / thaws every process of the guest through its cgroup. Returns false if the host has no freezer.
    bool pause();
    bool resume();
    bool can_pause() const { return !freeze_file_.empty(); }

    // Reaps the supervisor, notices a dead guest and the boot-complete signal. Cheap, call every second or so.
    ContainerStatus poll();

    ContainerStatus status() const { return status_; }

    // PID of the guest init in the host's PID namespace, or -1 if stopped.
    pid_t init_pid() const { return init_pid_; }

    const std::string& instance_id() const { return config_.instance_id; }
    const ContainerConfig& config() const { return config_; }
    const std::string& last_error() const { return last_error_; }
    bool boot_signal_seen() const { return boot_signal_seen_; }

    // Multi-line diagnostic string: state, mounts, QEMU status, kernel support and the tail of boot.log.
    std::string diagnostics() const;

private:
    ContainerConfig config_;
    ContainerStatus status_ = ContainerStatus::STOPPED;
    pid_t init_pid_ = -1;      // guest init as seen from the host
    pid_t monitor_pid_ = -1;   // supervisor, our direct child
    int exit_code_ = 0;
    int64_t start_ms_ = 0;
    bool boot_signal_seen_ = false;
    bool stop_requested_ = false;
    bool qemu_registered_ = false;
    bool data_is_image_ = true;
    bool has_vendor_ = false;
    std::string hardware_;
    std::string last_error_;
    std::vector<std::string> cgroup_paths_;
    std::string freeze_file_;
    NetPlan net_plan_;
    std::unique_ptr<HostNat> nat_;

    bool prepare();
    bool prepare_rootfs();
    bool prepare_images();
    bool prepare_data();
    bool prepare_shared();
    bool write_boot_files();
    bool spawn();
    bool setup_cgroups();
    void teardown_cgroups();
    void finish(int wait_status);
    bool fail(const std::string& message);
};

// Host binfmt_misc rule for 32-bit ARM via qemu, reference counted because it is global
bool binfmt_arm_acquire(const std::string& qemu_path, std::string* error);
void binfmt_arm_release();

// Global registry of active Container instances, handle-based so the helper protocol never exposes raw pointers.
class NamespaceManager {
public:
    static NamespaceManager& instance();

    bool init(const std::string& data_dir, const std::string& native_lib_dir);
    void shutdown();

    // Creates and starts a new container. Returns a handle, or 0 on failure (see last_start_error()).
    int64_t start_container(const ContainerConfig& config);

    void stop_container(int64_t handle);
    void kill_container(int64_t handle);

    // Returns nullptr if the handle is invalid.
    Container* get_container(int64_t handle);

    // Polls every container once; returns handles whose status changed.
    void poll_all();

    // Removes a stopped container from the registry.
    void remove_container(int64_t handle);

    std::vector<int64_t> handles() const;
    size_t running_count() const;

    const std::string& data_dir() const { return data_dir_; }
    const std::string& native_lib_dir() const { return native_lib_dir_; }
    const std::string& last_start_error() const { return last_start_error_; }

private:
    NamespaceManager() = default;

    std::string data_dir_;
    std::string native_lib_dir_;
    std::string last_start_error_;
    std::unordered_map<int64_t, std::unique_ptr<Container>> containers_;
    int64_t next_handle_ = 1;
    bool initialized_ = false;
};

} // namespace vine
