#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "input_lines.h"

// A small Wayland server for one kind of client: Waydroid's hwcomposer
// (hwcomposer.waydroid) in single-window mode. SurfaceFlinger composes
// everything, the client posts one wl_shm buffer per frame on one
// xdg_toplevel, and the server hands those pixels to a FrameSink.
//
// Globals: wl_compositor 4, wl_shm 1, xdg_wm_base 1, wl_output 3,
// wl_seat 5 (touch and keyboard), android_wlegl 1 (GPU gralloc buffers,
// passed on as resource ids). No subcompositor or viewporter, so the
// client keeps to the single-surface path.
//
// Input comes in as the guest agent's lines (input_lines.h) on a second
// socket and goes to the client that owns the toplevel.
namespace vine::wl {

// wl_shm formats (fourcc, except the two legacy codes)
constexpr uint32_t kShmArgb8888 = 0;           // bytes B G R A
constexpr uint32_t kShmXrgb8888 = 1;           // bytes B G R X
constexpr uint32_t kShmAbgr8888 = 0x34324241;  // bytes R G B A
constexpr uint32_t kShmXbgr8888 = 0x34324258;  // bytes R G B X

class FrameSink {
public:
    virtual ~FrameSink() = default;
    // Output size in pixels, what the toplevel is configured to
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    // One committed buffer, valid only during the call
    virtual void frame(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride, uint32_t shm_format) = 0;
    // One committed buffer that is a host GPU resource (Android HAL format)
    virtual void gpu_frame(uint32_t /*resource*/, uint32_t /*width*/, uint32_t /*height*/, uint32_t /*format*/) {}
    // The host shows GPU buffers as they are, so one goes back to the
    // client only once the host shows a later frame. gpu_seq() names the
    // last gpu_frame; a sink without a host frees replaced buffers at once.
    virtual uint32_t gpu_seq() const { return 0; }
    virtual bool gpu_shown_after(uint32_t /*seq*/) const { return true; }
};

class Client;

struct InputConn {
    int fd = -1;
    std::string buf;
};

class Server {
public:
    using Log = std::function<void(const std::string&)>;

    Server(FrameSink* sink, Log log);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Listens on a filesystem socket (an old one at the path is replaced)
    bool listen(const std::string& path, std::string* error);
    // Input lines from the app, one connection or more
    bool listen_input(const std::string& path, std::string* error);
    // To the toplevel's client; false if there is none
    bool inject(const InputEvent& e);

    // Serves until stop(); returns false if polling failed
    bool run();
    // From any thread
    void stop();

    // The output's refresh rate in mHz, which paces the client's vsync
    void set_refresh_mhz(uint32_t mhz) { refresh_mhz_ = mhz; }
    uint32_t refresh_mhz() const { return refresh_mhz_; }

    // Frames handed to the sink so far
    uint64_t frames() const { return frames_; }
    FrameSink* sink() const { return sink_; }

private:
    friend class Client;
    void accept_client();
    void accept_input();
    // false: drop the connection
    bool read_input(InputConn& c);
    Client* focus() const;
    void log(const std::string& msg) const { if (log_) log_(msg); }

    FrameSink* sink_;
    Log log_;
    int listen_fd_ = -1;
    int wake_[2] = {-1, -1};
    std::string path_;
    std::vector<std::unique_ptr<Client>> clients_;
    int input_fd_ = -1;
    std::string input_path_;
    std::vector<InputConn> inputs_;
    uint32_t serial_ = 0;
    uint64_t frames_ = 0;
    uint32_t refresh_mhz_ = 60000;
};

} // namespace vine::wl
