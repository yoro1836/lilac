#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ASurfaceControl;

namespace vine::display {

// Shows guest frames that stay on the host GPU (vinefb gpu_resource)
// without copying them: the GPU server hands over each resource's
// AHardwareBuffer once, and frames go to a child SurfaceControl of the
// window. Needs Android 10's SurfaceControl NDK; supported() is false
// before that and the caller keeps to CPU frames.
class GpuPresenter {
public:
    GpuPresenter() = default;
    ~GpuPresenter();
    GpuPresenter(const GpuPresenter&) = delete;
    GpuPresenter& operator=(const GpuPresenter&) = delete;

    static bool supported();

    // The vinefb file names the GPU server's socket (gpu_ahb_socket)
    void set_vinefb(const std::string& path) { vinefb_path_ = path; }
    // A new window (or none): the surface control goes with the old one
    void set_window(ANativeWindow* window);
    // Shows the resource over the window's buffer, which must be
    // width x height (the view scales both alike); false if it cannot
    bool present(uint32_t resource, uint32_t width, uint32_t height);
    // Back to the window's own buffers
    void hide();
    // The guest restarted: its GPU server and resource ids are new
    void reset();

private:
    AHardwareBuffer* buffer(uint32_t resource);
    bool connect();
    void disconnect();
    void drop_buffers();

    std::string vinefb_path_;
    ANativeWindow* window_ = nullptr;
    ASurfaceControl* control_ = nullptr;
    bool shown_ = false;
    int sock_ = -1;
    int64_t retry_after_ms_ = 0;
    // Newest last; resource ids are never reused, so entries stay valid
    std::vector<std::pair<uint32_t, AHardwareBuffer*>> buffers_;
};

} // namespace vine::display
