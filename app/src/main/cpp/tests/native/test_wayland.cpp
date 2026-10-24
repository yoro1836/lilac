// Drives the vine Wayland server with the real libwayland-client, the way
// Waydroid's hwcomposer does in single-window mode: bind the globals,
// open a maximized xdg_toplevel, wait for its configure, calibrate with a
// 1x1 buffer, then post full-size shm frames with frame callbacks.
// Also checks the vinefb sink's pixel formats.
#include "../../display/wayland/vinefb_sink.h"
#include "../../display/wayland/wl_server.h"

#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
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
    uint32_t last_w = 0, last_h = 0, last_format = 0, count = 0;
    std::vector<uint8_t> first, last_px;
};

struct Client {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_output* output = nullptr;
    wl_seat* seat = nullptr;
    bool has_abgr = false;
    int32_t mode_w = 0, mode_h = 0;
    int32_t conf_w = 0, conf_h = 0;
    bool configured = false;
    uint32_t seat_caps = 99;
    int releases = 0, frames_done = 0;
};

void global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<Client*>(data);
    const std::string i = iface;
    if (i == "wl_compositor") c->compositor = (wl_compositor*)wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 5u));
    else if (i == "wl_shm") c->shm = (wl_shm*)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (i == "xdg_wm_base") c->wm_base = (xdg_wm_base*)wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (i == "wl_output") c->output = (wl_output*)wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 3u));
    else if (i == "wl_seat") c->seat = (wl_seat*)wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u));
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
    CHECK(c.seat_caps == 0, "seat caps %u", c.seat_caps);

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
    test_sink(dir);
    rmdir(dir.c_str());
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
