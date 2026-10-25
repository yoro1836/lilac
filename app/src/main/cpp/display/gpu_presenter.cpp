#include "gpu_presenter.h"
#include "vinefb.h"
#include "../utils/vine_log.h"

#include <android/rect.h>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <type_traits>
#include <unistd.h>

struct ASurfaceTransaction;

namespace vine::display {

namespace {

constexpr size_t kMaxBuffers = 8;      // the guest cycles through 3 or so
constexpr int64_t kRetryMs = 1000;     // after the GPU server would not answer
constexpr int8_t kVisibilityShow = 1, kVisibilityHide = 0, kTransparencyOpaque = 2;

// Android 10 (API 29) NDK, looked up at run time: minSdk is lower
struct Api {
    ASurfaceControl* (*create_from_window)(ANativeWindow*, const char*);
    void (*release)(ASurfaceControl*);
    ASurfaceTransaction* (*tx_create)();
    void (*tx_delete)(ASurfaceTransaction*);
    void (*tx_apply)(ASurfaceTransaction*);
    void (*set_buffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int);
    void (*set_geometry)(ASurfaceTransaction*, ASurfaceControl*, const ARect*, const ARect*, int32_t);
    void (*set_visibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
    void (*set_transparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api x{};
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return x;
        auto sym = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, name));
            return f != nullptr;
        };
        x.ok = sym(x.create_from_window, "ASurfaceControl_createFromWindow") && sym(x.release, "ASurfaceControl_release") &&
               sym(x.tx_create, "ASurfaceTransaction_create") && sym(x.tx_delete, "ASurfaceTransaction_delete") &&
               sym(x.tx_apply, "ASurfaceTransaction_apply") && sym(x.set_buffer, "ASurfaceTransaction_setBuffer") &&
               sym(x.set_geometry, "ASurfaceTransaction_setGeometry") &&
               sym(x.set_visibility, "ASurfaceTransaction_setVisibility") &&
               sym(x.set_transparency, "ASurfaceTransaction_setBufferTransparency");
        return x;
    }();
    return a;
}

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

GpuPresenter::~GpuPresenter() {
    set_window(nullptr);
    disconnect();
}

bool GpuPresenter::supported() { return api().ok; }

void GpuPresenter::set_window(ANativeWindow* window) {
    if (window == window_) return;
    if (control_) api().release(control_);
    control_ = nullptr;
    shown_ = false;
    window_ = window;
    // The buffers stay cached: they belong to the guest, not the window
}

void GpuPresenter::hide() {
    if (!control_ || !shown_) return;
    const Api& a = api();
    ASurfaceTransaction* tx = a.tx_create();
    a.set_visibility(tx, control_, kVisibilityHide);
    a.tx_apply(tx);
    a.tx_delete(tx);
    shown_ = false;
}

bool GpuPresenter::present(uint32_t resource, uint32_t width, uint32_t height) {
    const Api& a = api();
    if (!a.ok || !window_ || !resource || !width || !height) return false;
    AHardwareBuffer* ahb = buffer(resource);
    if (!ahb) return false;
    if (!control_) {
        control_ = a.create_from_window(window_, "wayondroid-gpu");
        if (!control_) return false;
    }
    // Over the window's own buffer, which the caller keeps at the frame's
    // size: the child takes the parent's scaling to the view
    const ARect rect{0, 0, (int32_t)width, (int32_t)height};
    ASurfaceTransaction* tx = a.tx_create();
    a.set_buffer(tx, control_, ahb, -1);
    a.set_geometry(tx, control_, &rect, &rect, 0);
    a.set_transparency(tx, control_, kTransparencyOpaque);
    if (!shown_) a.set_visibility(tx, control_, kVisibilityShow);
    a.tx_apply(tx);
    a.tx_delete(tx);
    shown_ = true;
    return true;
}

AHardwareBuffer* GpuPresenter::buffer(uint32_t resource) {
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->first != resource) continue;
        auto hit = *it;
        buffers_.erase(it);
        buffers_.push_back(hit);
        return hit.second;
    }
    if (!connect()) return nullptr;
    uint32_t status = 1;
    AHardwareBuffer* ahb = nullptr;
    if (send(sock_, &resource, 4, MSG_NOSIGNAL) != 4 || recv(sock_, &status, 4, 0) != 4) {
        disconnect();
        return nullptr;
    }
    if (status != 0) return nullptr; // not a ColorBuffer (yet)
    if (AHardwareBuffer_recvHandleFromUnixSocket(sock_, &ahb) != 0 || !ahb) {
        disconnect();
        return nullptr;
    }
    if (buffers_.size() >= kMaxBuffers) {
        AHardwareBuffer_release(buffers_.front().second);
        buffers_.erase(buffers_.begin());
    }
    buffers_.emplace_back(resource, ahb);
    return ahb;
}

bool GpuPresenter::connect() {
    if (sock_ >= 0) return true;
    if (now_ms() < retry_after_ms_) return false;
    retry_after_ms_ = now_ms() + kRetryMs;
    const std::string name = gpu_ahb_socket(vinefb_path_);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (name.empty() || name.size() + 1 > sizeof sa.sun_path) return false;
    memcpy(sa.sun_path + 1, name.data(), name.size()); // abstract
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name.size())) != 0) {
        close(fd);
        return false;
    }
    sock_ = fd;
    return true;
}

void GpuPresenter::reset() { disconnect(); }

void GpuPresenter::disconnect() {
    if (sock_ >= 0) close(sock_);
    sock_ = -1;
    // A new GPU server (a guest restart) has other buffers
    drop_buffers();
}

void GpuPresenter::drop_buffers() {
    for (auto& b : buffers_) AHardwareBuffer_release(b.second);
    buffers_.clear();
}

} // namespace vine::display
