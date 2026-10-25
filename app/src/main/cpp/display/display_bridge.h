#pragma once

#include "gpu_presenter.h"
#include "vinefb_reader.h"

#include <android/native_window.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace vine::display {

// Draws vinefb frames onto a Surface, follows vsync while frames arrive and idles when the screen is still
class DisplayBridge {
public:
    struct Info {
        bool open = false;
        bool rendering = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t guest_frames = 0;     // frames the guest has presented
        uint64_t frames_drawn = 0;     // frames this bridge pushed to the surface
    };

    DisplayBridge() = default;
    ~DisplayBridge();

    DisplayBridge(const DisplayBridge&) = delete;
    DisplayBridge& operator=(const DisplayBridge&) = delete;

    bool open(const std::string& vinefb_path, std::string* error);
    void close();

    // Takes its own reference on the window; pass nullptr to detach.
    void set_surface(ANativeWindow* window);

    bool start();
    void stop();

    Info info() const;

private:
    void loop();
    bool draw_if_changed();
    static void on_vsync(long frame_time_nanos, void* data);

    VineFbReader reader_;
    GpuPresenter gpu_; // under window_mutex_
    mutable std::mutex window_mutex_;
    ANativeWindow* window_ = nullptr;
    int window_w_ = 0;
    int window_h_ = 0;
    std::atomic<bool> force_redraw_{true};
    uint32_t last_seq_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_drawn_{0};
    std::thread thread_;

    // Only touched on the render thread
    bool armed_ = false;
    int idle_frames_ = 0;
};

} // namespace vine::display
