#include "display_bridge.h"
#include "../utils/vine_log.h"

#include <android/choreographer.h>
#include <ctime>
#include <android/looper.h>

namespace vine::display {

namespace {
constexpr int kWindowFormatRgba8888 = 1;
constexpr int kIdleAfterFrames = 45;   // about 0.75 s of unchanged screen at 60 Hz
constexpr int kIdlePollMs = 40;
constexpr int64_t kReplaceCheckMs = 500;

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
}

DisplayBridge::~DisplayBridge() { close(); }

bool DisplayBridge::open(const std::string& vinefb_path, std::string* error) {
    return reader_.open(vinefb_path, error);
}

void DisplayBridge::close() {
    stop();
    set_surface(nullptr);
    reader_.close();
}

void DisplayBridge::set_surface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    window_w_ = window_h_ = 0; // geometry is applied again on the next frame
    force_redraw_.store(true);
    if (window_) ANativeWindow_acquire(window_);
}

DisplayBridge::Info DisplayBridge::info() const {
    std::lock_guard<std::mutex> lock(window_mutex_);
    Info i;
    i.open = reader_.is_open();
    i.rendering = running_.load();
    i.frames_drawn = frames_drawn_.load();
    FrameInfo fi;
    if (reader_.header(&fi)) {
        i.width = fi.width;
        i.height = fi.height;
        i.guest_frames = fi.frame_count;
    }
    return i;
}

bool DisplayBridge::start() {
    if (running_.load()) return true;
    if (!reader_.is_open()) return false;
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void DisplayBridge::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void DisplayBridge::loop() {
    // Choreographer callbacks are delivered through this thread's looper
    ALooper_prepare(0);
    armed_ = false;
    idle_frames_ = 0;
    int64_t checked = now_ms();
    while (running_.load()) {
        if (now_ms() - checked >= kReplaceCheckMs) {
            // The guest restarted and its display server made a new file
            checked = now_ms();
            std::lock_guard<std::mutex> lock(window_mutex_);
            if (reader_.replaced()) {
                const std::string path = reader_.path();
                if (reader_.open(path, nullptr)) {
                    last_seq_ = 0;
                    force_redraw_.store(true);
                }
            }
        }
        if (!armed_) {
            if (idle_frames_ < kIdleAfterFrames || reader_.sequence() != last_seq_ || force_redraw_.load()) {
                idle_frames_ = 0;
                armed_ = true;
                AChoreographer_postFrameCallback(AChoreographer_getInstance(), &DisplayBridge::on_vsync, this);
            }
        }
        ALooper_pollOnce(armed_ ? 16 : kIdlePollMs, nullptr, nullptr, nullptr);
    }
}

void DisplayBridge::on_vsync(long, void* data) {
    auto* self = static_cast<DisplayBridge*>(data);
    self->armed_ = false;
    if (!self->running_.load()) return;
    if (self->draw_if_changed()) self->idle_frames_ = 0;
    else ++self->idle_frames_;
    if (self->idle_frames_ < kIdleAfterFrames) {
        self->armed_ = true;
        AChoreographer_postFrameCallback(AChoreographer_getInstance(), &DisplayBridge::on_vsync, data);
    }
}

bool DisplayBridge::draw_if_changed() {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (!window_) return false;

    const uint32_t seq = reader_.sequence();
    if (!force_redraw_.load() && seq == last_seq_) return false;
    if ((seq & 1u) != 0) return false; // mid-frame, the next vsync will see it finished

    FrameInfo hdr;
    if (!reader_.header(&hdr)) return false;
    if ((int)hdr.width != window_w_ || (int)hdr.height != window_h_) {
        if (ANativeWindow_setBuffersGeometry(window_, (int32_t)hdr.width, (int32_t)hdr.height, kWindowFormatRgba8888) != 0) {
            VINE_LOGW("display: cannot set %ux%u on the surface", hdr.width, hdr.height);
            return false;
        }
        window_w_ = (int)hdr.width;
        window_h_ = (int)hdr.height;
    }

    ANativeWindow_Buffer buf{};
    if (ANativeWindow_lock(window_, &buf, nullptr) != 0) return false;
    FrameInfo got;
    const bool ok = reader_.copy_frame(static_cast<uint8_t*>(buf.bits), (size_t)buf.stride * 4, (uint32_t)buf.height, &got);
    ANativeWindow_unlockAndPost(window_);

    if (ok) {
        last_seq_ = got.frame_seq;
        force_redraw_.store(false);
        frames_drawn_.fetch_add(1);
    } else {
        force_redraw_.store(true); // try again next vsync
    }
    return ok;
}

} // namespace vine::display
