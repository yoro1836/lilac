#include "display_bridge.h"
#include "../utils/vine_log.h"

#include <android/choreographer.h>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <android/looper.h>

namespace vine::display {

namespace {
constexpr int kWindowFormatRgba8888 = 1;
constexpr int kIdleAfterFrames = 45;   // about 0.75 s of unchanged screen at 60 Hz
constexpr int kIdlePollMs = 40;
constexpr int kFreePollMs = 2;         // vsync off: how often a new frame is looked for
constexpr int64_t kFreeIdleMs = 750;   // vsync off: unchanged for this long, poll slowly
constexpr int64_t kReplaceCheckMs = 500;

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// API 30, looked up at run time: minSdk is lower
using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
SetFrameRate window_set_frame_rate() {
    static const SetFrameRate f = [] {
        void* lib = dlopen("libnativewindow.so", RTLD_NOW);
        return lib ? reinterpret_cast<SetFrameRate>(dlsym(lib, "ANativeWindow_setFrameRate")) : nullptr;
    }();
    return f;
}
}

DisplayBridge::~DisplayBridge() { close(); }

bool DisplayBridge::open(const std::string& vinefb_path, std::string* error) {
    gpu_.set_vinefb(vinefb_path);
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
    gpu_.set_window(window_);
    apply_frame_rate();
}

void DisplayBridge::set_timing(int refresh_hz, bool vsync) {
    vsync_.store(vsync);
    std::lock_guard<std::mutex> lock(window_mutex_);
    refresh_hz_ = refresh_hz;
    apply_frame_rate();
}

void DisplayBridge::apply_frame_rate() {
    gpu_.set_frame_rate((float)refresh_hz_);
    if (window_ && window_set_frame_rate()) window_set_frame_rate()(window_, (float)refresh_hz_, 0);
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
    int64_t last_frame = 0;
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
                    gpu_.reset();
                }
            }
        }
        if (!vsync_.load()) {
            // A frame goes out as soon as it is seen
            if (draw_if_changed()) last_frame = now_ms();
            ALooper_pollOnce(now_ms() - last_frame < kFreeIdleMs ? kFreePollMs : kIdlePollMs, nullptr, nullptr, nullptr);
            continue;
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
    if (self->vsync_.load() && self->idle_frames_ < kIdleAfterFrames) {
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
    if (hdr.gpu_resource && GpuPresenter::supported()) {
        // The frame stays on the GPU and goes to a surface control over
        // the window. A black window buffer of the frame's size underneath
        // gives both the view's scaling.
        if ((int)hdr.gpu_width != window_w_ || (int)hdr.gpu_height != window_h_) {
            if (ANativeWindow_setBuffersGeometry(window_, (int32_t)hdr.gpu_width, (int32_t)hdr.gpu_height,
                                                 kWindowFormatRgba8888) != 0)
                return false;
            ANativeWindow_Buffer buf{};
            if (ANativeWindow_lock(window_, &buf, nullptr) != 0) return false;
            memset(buf.bits, 0, (size_t)buf.stride * 4 * (size_t)buf.height);
            ANativeWindow_unlockAndPost(window_);
            window_w_ = (int)hdr.gpu_width;
            window_h_ = (int)hdr.gpu_height;
        }
        if (reader_.sequence() != seq) return false; // replaced while read, the next vsync gets it
        if (!gpu_.present(hdr.gpu_resource, hdr.gpu_width, hdr.gpu_height, seq)) {
            force_redraw_.store(true);
            return false;
        }
        last_seq_ = seq;
        force_redraw_.store(false);
        frames_drawn_.fetch_add(1);
        return true;
    }
    gpu_.hide();
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
