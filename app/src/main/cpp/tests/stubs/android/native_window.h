#pragma once
// Host-build stub of <android/native_window.h>, enough for the display bridge to compile and be unit-tested.
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct ANativeWindow { int refs = 1; int width = 0; int height = 0; std::uint8_t* pixels = nullptr; int stride = 0; };
struct ARect { int32_t left, top, right, bottom; };
struct ANativeWindow_Buffer { int32_t width, height, stride, format; void* bits; uint32_t reserved[6]; };

inline void ANativeWindow_acquire(ANativeWindow* w) { if (w) w->refs++; }
inline void ANativeWindow_release(ANativeWindow* w) { if (w) w->refs--; }
inline int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* w, int32_t width, int32_t height, int32_t) {
    w->width = width; w->height = height; w->stride = width;
    std::free(w->pixels);
    w->pixels = static_cast<std::uint8_t*>(std::calloc((size_t)width * height, 4));
    return 0;
}
inline int32_t ANativeWindow_lock(ANativeWindow* w, ANativeWindow_Buffer* b, ARect*) {
    if (!w || !w->pixels) return -1;
    b->width = w->width; b->height = w->height; b->stride = w->stride; b->format = 1; b->bits = w->pixels;
    return 0;
}
inline int32_t ANativeWindow_unlockAndPost(ANativeWindow*) { return 0; }
