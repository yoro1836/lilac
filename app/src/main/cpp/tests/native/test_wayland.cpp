// Drives the vine Wayland server with the real libwayland-client, the way
// Waydroid's hwcomposer does in single-window mode: bind the globals,
// open a maximized xdg_toplevel, wait for its configure, calibrate with a
// 1x1 buffer, then post full-size shm frames with frame callbacks.
// Then touches and keys through the input socket, as the app sends them.
// Also checks android_wlegl GPU buffers, the input line parser and the
// vinefb sink's pixel formats.
#include "../../display/wayland/vinefb_sink.h"
#include "../../display/wayland/wl_server.h"

#include "android-wlegl-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        ++g_checks;                                               \
        if (!(cond)) {                                            \
            ++g_failed;                                           \
            printf("  FAIL line %d: %s  ", __LINE__, #cond);      \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while (0)

namespace {

constexpr uint32_t W = 64, H = 48;

struct MemSink : vine::wl::FrameSink {
    uint32_t width() const override { return W; }
    uint32_t height() const override { return H; }
    void frame(const uint8_t* px, uint32_t w, uint32_t h, uint32_t stride, uint32_t format) override {
        last_w = w;
        last_h = h;
        last_format = format;
        first.assign(px, px + 4);
        last_px.assign(px + (size_t)(h - 1) * stride + (w - 1) * 4, px + (size_t)(h - 1) * stride + w * 4);
        ++count;
    }
    void gpu_frame(uint32_t resource, uint32_t w, uint32_t h, uint32_t format) override {
        gpu_resource = resource;
        last_w = w;
        last_h = h;
        last_format = format;
        ++gpu_count;
    }
    // With acks on, a GPU buffer goes back only after shown passes its frame
    uint32_t gpu_seq() const override { return gpu_count * 2; }
    bool gpu_shown_after(uint32_t seq) const override { return !acks.load() || (int32_t)(shown.load() - seq) > 0; }
    std::atomic<bool> acks{false};
    std::atomic<uint32_t> shown{0};
    uint32_t last_w = 0, last_h = 0, last_format = 0, count = 0, gpu_resource = 0, gpu_count = 0;
    std::vector<uint8_t> first, last_px;
};

struct Client {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_output* output = nullptr;
    wl_seat* seat = nullptr;
    android_wlegl* wlegl = nullptr;
    bool has_abgr = false;
    int32_t mode_w = 0, mode_h = 0;
    int32_t conf_w = 0, conf_h = 0;
    bool configured = false;
    uint32_t seat_caps = 99;
    int releases = 0, frames_done = 0;
    // input
    int keymap_format = -1, keymap_fd_ok = 0, entered = 0;
    std::vector<std::string> touch_log, key_log;
};

void global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<Client*>(data);
    const std::string i = iface;
    if (i == "wl_compositor") c->compositor = (wl_compositor*)wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 5u));
    else if (i == "wl_shm") c->shm = (wl_shm*)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (i == "xdg_wm_base") c->wm_base = (xdg_wm_base*)wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (i == "wl_output") c->output = (wl_output*)wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 3u));
    else if (i == "wl_seat") c->seat = (wl_seat*)wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u));
    else if (i == "android_wlegl") c->wlegl = (android_wlegl*)wl_registry_bind(reg, name, &android_wlegl_interface, 1);
}
void global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {global, global_remove};

void shm_format(void* data, wl_shm*, uint32_t f) {
    if (f == WL_SHM_FORMAT_ABGR8888) static_cast<Client*>(data)->has_abgr = true;
}
const wl_shm_listener shm_listener = {shm_format};

void out_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
void out_mode(void* data, wl_output*, uint32_t, int32_t w, int32_t h, int32_t) {
    auto* c = static_cast<Client*>(data);
    c->mode_w = w;
    c->mode_h = h;
}
void out_done(void*, wl_output*) {}
void out_scale(void*, wl_output*, int32_t) {}
const wl_output_listener output_listener = {out_geometry, out_mode, out_done, out_scale, nullptr, nullptr};

void seat_caps(void* data, wl_seat*, uint32_t caps) { static_cast<Client*>(data)->seat_caps = caps; }
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = {seat_caps, seat_name};

std::string fmt_xy(wl_fixed_t x, wl_fixed_t y) {
    char b[48];
    snprintf(b, sizeof b, "%.1f,%.1f", wl_fixed_to_double(x), wl_fixed_to_double(y));
    return b;
}
void t_down(void* d, wl_touch*, uint32_t, uint32_t, wl_surface* s, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    static_cast<Client*>(d)->touch_log.push_back((s ? "down " : "down-nosurface ") + std::to_string(id) + " " + fmt_xy(x, y));
}
void t_up(void* d, wl_touch*, uint32_t, uint32_t, int32_t id) { static_cast<Client*>(d)->touch_log.push_back("up " + std::to_string(id)); }
void t_motion(void* d, wl_touch*, uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    static_cast<Client*>(d)->touch_log.push_back("motion " + std::to_string(id) + " " + fmt_xy(x, y));
}
void t_frame(void* d, wl_touch*) { static_cast<Client*>(d)->touch_log.push_back("frame"); }
void t_cancel(void* d, wl_touch*) { static_cast<Client*>(d)->touch_log.push_back("cancel"); }
void t_shape(void*, wl_touch*, int32_t, wl_fixed_t, wl_fixed_t) {}
void t_orientation(void*, wl_touch*, int32_t, wl_fixed_t) {}
const wl_touch_listener touch_listener = {t_down, t_up, t_motion, t_frame, t_cancel, t_shape, t_orientation};

void k_keymap(void* d, wl_keyboard*, uint32_t format, int fd, uint32_t) {
    auto* c = static_cast<Client*>(d);
    c->keymap_format = (int)format;
    c->keymap_fd_ok = fd >= 0;
    if (fd >= 0) close(fd);
}
void k_enter(void* d, wl_keyboard*, uint32_t, wl_surface* s, wl_array*) { static_cast<Client*>(d)->entered += s != nullptr; }
void k_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void k_key(void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    static_cast<Client*>(d)->key_log.push_back(std::to_string(key) + (state ? "+" : "-"));
}
void k_mods(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void k_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener keyboard_listener = {k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat};

// Writes lines to the server's input socket
bool send_input(const std::string& path, const std::string& lines) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path.c_str());
    const bool ok = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 &&
                    write(fd, lines.data(), lines.size()) == (ssize_t)lines.size();
    if (fd >= 0) close(fd);
    return ok;
}

// Round trips until `done` or about a second passes
template <typename F>
void pump(wl_display* d, F done) {
    for (int i = 0; i < 100 && !done(); ++i) {
        wl_display_roundtrip(d);
        if (!done()) usleep(10000);
    }
}

void wm_ping(void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
const xdg_wm_base_listener wm_listener = {wm_ping};

void xs_configure(void* data, xdg_surface* s, uint32_t serial) {
    static_cast<Client*>(data)->configured = true;
    xdg_surface_ack_configure(s, serial);
}
const xdg_surface_listener xs_listener = {xs_configure};

void tl_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* c = static_cast<Client*>(data);
    c->conf_w = w;
    c->conf_h = h;
}
void tl_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener tl_listener = {tl_configure, tl_close, nullptr, nullptr};

void buf_release(void* data, wl_buffer*) { static_cast<Client*>(data)->releases++; }
const wl_buffer_listener buf_listener = {buf_release};

void frame_done(void* data, wl_callback* cb, uint32_t) {
    static_cast<Client*>(data)->frames_done++;
    wl_callback_destroy(cb);
}
const wl_callback_listener frame_listener = {frame_done};

int memfd(size_t size) {
    const int fd = (int)syscall(SYS_memfd_create, "buffer", 0);
    if (fd >= 0 && ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void test_session(const std::string& dir) {
    printf("[hwcomposer-style session]\n");
    MemSink sink;
    vine::wl::Server server(&sink, nullptr);
    std::string err;
    CHECK(server.listen(dir + "/wayland-0", &err), "listen: %s", err.c_str());
    CHECK(server.listen_input(dir + "/input", &err), "listen_input: %s", err.c_str());
    std::thread th([&] { server.run(); });

    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    Client c;
    c.display = wl_display_connect(nullptr);
    CHECK(c.display, "connect");
    if (!c.display) {
        server.stop();
        th.join();
        return;
    }
    wl_registry* reg = wl_display_get_registry(c.display);
    wl_registry_add_listener(reg, &registry_listener, &c);
    wl_display_roundtrip(c.display);
    CHECK(c.compositor && c.shm && c.wm_base && c.output && c.seat, "globals");
    wl_shm_add_listener(c.shm, &shm_listener, &c);
    wl_output_add_listener(c.output, &output_listener, &c);
    wl_seat_add_listener(c.seat, &seat_listener, &c);
    xdg_wm_base_add_listener(c.wm_base, &wm_listener, &c);
    wl_display_roundtrip(c.display);
    CHECK(c.has_abgr, "ABGR8888 advertised");
    CHECK(c.mode_w == (int)W && c.mode_h == (int)H, "output mode %dx%d", c.mode_w, c.mode_h);
    CHECK(c.seat_caps == (WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH), "seat caps %u", c.seat_caps);
    wl_touch* touch = wl_seat_get_touch(c.seat);
    wl_touch_add_listener(touch, &touch_listener, &c);
    wl_keyboard* kbd = wl_seat_get_keyboard(c.seat);
    wl_keyboard_add_listener(kbd, &keyboard_listener, &c);
    wl_display_roundtrip(c.display);
    CHECK(c.keymap_format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP && c.keymap_fd_ok, "no_keymap with an fd");

    wl_surface* surf = wl_compositor_create_surface(c.compositor);
    xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, surf);
    xdg_surface_add_listener(xs, &xs_listener, &c);
    xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_listener, &c);
    xdg_toplevel_set_maximized(tl);
    xdg_toplevel_set_title(tl, "Waydroid");
    xdg_toplevel_set_app_id(tl, "Waydroid");
    wl_surface_commit(surf);
    for (int i = 0; i < 5 && !c.configured; ++i) wl_display_roundtrip(c.display);
    CHECK(c.configured, "configured");
    CHECK(c.conf_w == (int)W && c.conf_h == (int)H, "toplevel %dx%d", c.conf_w, c.conf_h);

    // Calibration: a 1x1 buffer from a 4-byte pool; not a frame of the output size
    int fd = memfd(4);
    wl_shm_pool* tiny = wl_shm_create_pool(c.shm, fd, 4);
    close(fd);
    wl_buffer* one = wl_shm_pool_create_buffer(tiny, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(tiny);
    wl_surface_attach(surf, one, 0, 0);
    wl_surface_commit(surf);
    xdg_surface_set_window_geometry(xs, 0, 0, W, H);
    wl_region* region = wl_compositor_create_region(c.compositor);
    wl_region_add(region, 0, 0, W, H);
    wl_surface_set_opaque_region(surf, region);
    wl_region_destroy(region);
    wl_display_roundtrip(c.display);
    CHECK(sink.count == 1 && sink.last_w == 1, "calibration buffer reached the sink as 1x1");

    // Two full frames from one pool, RGBA in memory
    const size_t size = (size_t)W * H * 4;
    fd = memfd(size * 2);
    auto* mem = static_cast<uint8_t*>(mmap(nullptr, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, (int32_t)(size * 2));
    close(fd);
    wl_buffer* bufs[2];
    for (int b = 0; b < 2; ++b) {
        bufs[b] = wl_shm_pool_create_buffer(pool, (int32_t)(b * size), W, H, W * 4, WL_SHM_FORMAT_ABGR8888);
        wl_buffer_add_listener(bufs[b], &buf_listener, &c);
        for (size_t p = 0; p < (size_t)W * H; ++p) {
            uint8_t* px = mem + b * size + p * 4;
            px[0] = (uint8_t)(10 + b);
            px[1] = 20;
            px[2] = 30;
            px[3] = 255;
        }
        mem[b * size + size - 4] = 99; // the last pixel's R
        wl_callback* cb = wl_surface_frame(surf);
        wl_callback_add_listener(cb, &frame_listener, &c);
        wl_surface_attach(surf, bufs[b], 0, 0);
        wl_surface_damage(surf, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(surf);
        wl_display_roundtrip(c.display);
        CHECK(sink.count == (uint32_t)(2 + b), "frame %d reached the sink (%u)", b, sink.count);
        CHECK(sink.last_w == W && sink.last_h == H && sink.last_format == WL_SHM_FORMAT_ABGR8888, "frame %d geometry", b);
        CHECK(sink.first.size() == 4 && sink.first[0] == 10 + b && sink.first[3] == 255, "frame %d first pixel", b);
        CHECK(sink.last_px.size() == 4 && sink.last_px[0] == 99, "frame %d last pixel", b);
    }
    CHECK(c.releases == 2, "buffer releases %d", c.releases);
    CHECK(c.frames_done == 2, "frame callbacks %d", c.frames_done);
    CHECK(server.frames() == 3, "server counted %llu", (unsigned long long)server.frames());

    // Input: a two-finger gesture, a key, typed text, an unknown line in between
    CHECK(send_input(dir + "/input",
                     "touch 0 0 1 7 0.5 0.25\n"
                     "touch 5 1 2 7 0.5 0.25 8 1.0 1.0\n"
                     "touch 2 0 2 7 0.25 0.5 8 1.0 1.0\n"
                     "bogus\n"
                     "touch 6 0 2 7 0.25 0.5 8 1.0 1.0\n"
                     "touch 1 0 1 8 1.0 1.0\n"
                     "key 4 1\nkey 4 0\ntext aB\n"),
          "input lines sent");
    pump(c.display, [&] { return c.touch_log.size() >= 12 && c.key_log.size() >= 8; });
    const std::vector<std::string> want_touch = {
        "down 7 32.0,12.0", "frame", "down 8 64.0,48.0", "frame", "motion 7 16.0,24.0", "motion 8 64.0,48.0",
        "frame", "up 7", "frame", "up 8", "frame"};
    std::string got;
    for (auto& t : c.touch_log) got += t + "; ";
    CHECK(c.touch_log == want_touch, "touch events: %s", got.c_str());
    const std::vector<std::string> want_keys = {"158+", "158-", "30+", "30-", "42+", "48+", "48-", "42-"};
    got.clear();
    for (auto& k : c.key_log) got += k + " ";
    CHECK(c.key_log == want_keys, "key events: %s", got.c_str());
    CHECK(c.entered == 1, "keyboard entered once (%d)", c.entered);
    CHECK(send_input(dir + "/input", "touch 3 0 1 7 0 0\n"), "cancel sent");
    pump(c.display, [&] { return c.touch_log.size() >= 12; });
    CHECK(c.touch_log.size() == 12 && c.touch_log.back() == "cancel", "touch cancel");
    wl_touch_release(touch);
    wl_keyboard_release(kbd);

    // Growing the pool maps it again
    wl_shm_pool_resize(pool, (int32_t)(size * 2));
    wl_display_roundtrip(c.display);
    wl_buffer_destroy(one);
    for (auto* b : bufs) wl_buffer_destroy(b);
    wl_shm_pool_destroy(pool);
    munmap(mem, size * 2);
    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_display_roundtrip(c.display);
    CHECK(wl_display_get_error(c.display) == 0, "no protocol error (%d)", wl_display_get_error(c.display));
    if (c.wlegl) android_wlegl_destroy(c.wlegl);
    wl_seat_destroy(c.seat);
    wl_output_destroy(c.output);
    xdg_wm_base_destroy(c.wm_base);
    wl_shm_destroy(c.shm);
    wl_compositor_destroy(c.compositor);
    wl_registry_destroy(reg);
    wl_display_disconnect(c.display);

    server.stop();
    th.join();
}

// A GPU gralloc buffer as the hwcomposer sends it: two fds and a
// cb_handle_t's 17 ints, magic at 7 and the resource id at 8
wl_buffer* gpu_buffer(Client& c, uint32_t resource) {
    std::vector<int32_t> ints(17, 0);
    ints[7] = (int32_t)0xABFABF05u;
    ints[8] = (int32_t)resource;
    wl_array arr;
    wl_array_init(&arr);
    memcpy(wl_array_add(&arr, ints.size() * 4), ints.data(), ints.size() * 4);
    android_wlegl_handle* h = android_wlegl_create_handle(c.wlegl, 2, &arr);
    wl_array_release(&arr);
    for (int i = 0; i < 2; ++i) {
        const int fd = memfd(4096);
        android_wlegl_handle_add_fd(h, fd);
        close(fd);
    }
    wl_buffer* b = android_wlegl_create_buffer(c.wlegl, (int32_t)W, (int32_t)H, (int32_t)W, 1, 0x300, h);
    android_wlegl_handle_destroy(h);
    return b;
}

void test_wlegl(const std::string& dir) {
    printf("[android_wlegl GPU buffers]\n");
    MemSink sink;
    vine::wl::Server server(&sink, nullptr);
    std::string err;
    CHECK(server.listen(dir + "/wayland-1", &err), "listen: %s", err.c_str());
    std::thread th([&] { server.run(); });
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    Client c;
    c.display = wl_display_connect("wayland-1");
    CHECK(c.display, "connect");
    if (!c.display) {
        server.stop();
        th.join();
        return;
    }
    wl_registry* reg = wl_display_get_registry(c.display);
    wl_registry_add_listener(reg, &registry_listener, &c);
    wl_display_roundtrip(c.display);
    CHECK(c.wlegl != nullptr, "android_wlegl advertised");
    xdg_wm_base_add_listener(c.wm_base, &wm_listener, &c);
    wl_surface* surf = wl_compositor_create_surface(c.compositor);
    xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, surf);
    xdg_surface_add_listener(xs, &xs_listener, &c);
    xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_listener, &c);
    wl_surface_commit(surf);
    pump(c.display, [&] { return c.configured; });

    wl_buffer* a = gpu_buffer(c, 42);
    wl_buffer* b = gpu_buffer(c, 43);
    wl_buffer_add_listener(a, &buf_listener, &c);
    wl_buffer_add_listener(b, &buf_listener, &c);
    wl_surface_attach(surf, a, 0, 0);
    wl_surface_commit(surf);
    wl_display_roundtrip(c.display);
    CHECK(sink.gpu_count == 1 && sink.gpu_resource == 42 && sink.last_w == W && sink.last_format == 1,
          "GPU frame passed on (%u frames, resource %u)", sink.gpu_count, sink.gpu_resource);
    CHECK(c.releases == 0, "buffer held while shown");
    wl_surface_attach(surf, b, 0, 0);
    wl_surface_commit(surf);
    wl_display_roundtrip(c.display);
    CHECK(sink.gpu_count == 2 && sink.gpu_resource == 43, "second GPU frame (resource %u)", sink.gpu_resource);
    CHECK(c.releases == 1, "previous buffer released (%d)", c.releases);

    // A host that shows the buffers: b stays until a later frame is on screen
    sink.acks = true;
    wl_surface_attach(surf, a, 0, 0);
    wl_surface_commit(surf);
    wl_display_roundtrip(c.display);
    usleep(30000);
    wl_display_roundtrip(c.display);
    CHECK(c.releases == 1, "replaced buffer kept until the host shows a later frame (%d)", c.releases);
    sink.shown = sink.gpu_seq();
    pump(c.display, [&] { return c.releases == 2; });
    CHECK(c.releases == 2, "released once the host shows a later frame (%d)", c.releases);
    // No word from the host: it still goes back, later
    wl_surface_attach(surf, b, 0, 0);
    wl_surface_commit(surf);
    wl_display_roundtrip(c.display);
    CHECK(c.releases == 2, "kept while the host is quiet (%d)", c.releases);
    pump(c.display, [&] { return c.releases == 3; });
    CHECK(c.releases == 3, "released after the host stayed quiet (%d)", c.releases);
    CHECK(sink.count == 0, "no pixel frames");
    CHECK(wl_display_get_error(c.display) == 0, "no protocol error (%d)", wl_display_get_error(c.display));

    wl_buffer_destroy(a);
    wl_buffer_destroy(b);
    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_display_roundtrip(c.display);
    android_wlegl_destroy(c.wlegl);
    wl_seat_destroy(c.seat);
    wl_output_destroy(c.output);
    xdg_wm_base_destroy(c.wm_base);
    wl_shm_destroy(c.shm);
    wl_compositor_destroy(c.compositor);
    wl_registry_destroy(reg);
    wl_display_disconnect(c.display);
    server.stop();
    th.join();
}

void test_lines() {
    printf("[input lines]\n");
    using vine::wl::InputEvent;
    std::vector<InputEvent> ev;
    std::string err;
    CHECK(vine::wl::parse_input_line("touch 0 0 1 3 0.1 0.9", &ev, &err) && ev.size() == 2 &&
              ev[0].kind == InputEvent::TouchDown && ev[0].id == 3 && ev[0].x == 0.1 && ev[1].kind == InputEvent::TouchFrame,
          "touch down");
    ev.clear();
    CHECK(!vine::wl::parse_input_line("touch 0 2 1 3 0.1 0.9", &ev, &err) && ev.empty(), "index out of range rejected");
    CHECK(!vine::wl::parse_input_line("touch 0 0 2 3 0.1 0.9", &ev, &err) && ev.empty(), "short point list rejected");
    CHECK(!vine::wl::parse_input_line("touch 9 0 1 3 0.1 0.9", &ev, &err) && ev.empty(), "unknown action rejected");
    CHECK(vine::wl::parse_input_line("key 3 1", &ev, &err) && ev.size() == 1 && ev[0].key == 172 && ev[0].down, "HOME");
    ev.clear();
    CHECK(!vine::wl::parse_input_line("key 187 1", &ev, &err) && ev.empty(), "APP_SWITCH has no small evdev code");
    CHECK(vine::wl::evdev_for_keycode(29) == 30 && vine::wl::evdev_for_keycode(54) == 44 &&
              vine::wl::evdev_for_keycode(7) == 11 && vine::wl::evdev_for_keycode(16) == 10,
          "letters and digits");
    CHECK(vine::wl::parse_input_line("text a\\nb", &ev, &err) && ev.size() == 6 && ev[0].key == 30 && ev[2].key == 28 &&
              ev[4].key == 48,
          "text with an escaped newline");
    ev.clear();
    CHECK(!vine::wl::parse_input_line("text \xc3\xa9", &ev, &err) && ev.empty(), "non-ASCII text rejected");
}

void test_sink(const std::string& dir) {
    printf("[vinefb sink]\n");
    const std::string path = dir + "/vinefb";
    std::string err;
    CHECK(vine::wl::create_vinefb(path, 2, 1, &err), "create: %s", err.c_str());
    vine::wl::VineFbSink sink;
    CHECK(sink.open(path) && sink.width() == 2 && sink.height() == 1, "open");
    auto read_back = [&](uint8_t out[8], uint32_t* frames) {
        const int fd = open(path.c_str(), O_RDONLY);
        vinefb::Header h{};
        (void)!pread(fd, &h, sizeof h, 0);
        (void)!pread(fd, out, 8, vinefb::kHeaderSize);
        close(fd);
        *frames = h.frame_count;
    };
    uint8_t out[8];
    uint32_t n = 0;
    const uint8_t bgra[8] = {1, 2, 3, 4, 5, 6, 7, 8}; // ARGB8888: B G R A
    sink.frame(bgra, 2, 1, 8, vine::wl::kShmArgb8888);
    read_back(out, &n);
    CHECK(n == 1 && out[0] == 3 && out[1] == 2 && out[2] == 1 && out[3] == 4 && out[4] == 7, "ARGB8888 swizzled to RGBA");
    const uint8_t rgbx[8] = {9, 8, 7, 0, 6, 5, 4, 0}; // XBGR8888: R G B X
    sink.frame(rgbx, 2, 1, 8, vine::wl::kShmXbgr8888);
    read_back(out, &n);
    CHECK(n == 2 && out[0] == 9 && out[3] == 255 && out[7] == 255, "XBGR8888 made opaque");
    const uint8_t rgba[8] = {1, 1, 1, 7, 2, 2, 2, 7};
    sink.frame(rgba, 2, 1, 8, vine::wl::kShmAbgr8888);
    read_back(out, &n);
    CHECK(n == 3 && out[3] == 7 && out[4] == 2, "ABGR8888 copied as is");
    sink.frame(rgba, 1, 1, 4, vine::wl::kShmAbgr8888);
    read_back(out, &n);
    CHECK(n == 3, "wrong size dropped");
    unlink(path.c_str());
}

} // namespace

int main() {
    char tmpl[] = "/tmp/vine_wl_test.XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    test_session(dir);
    test_wlegl(dir);
    test_lines();
    test_sink(dir);
    rmdir(dir.c_str());
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
