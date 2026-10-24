#include "vinefb_sink.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace vine::wl {

namespace {

// Byte order of a pixel in memory
enum Order { RGBA, BGRA };

Order shm_order(uint32_t f) { return f == kShmAbgr8888 || f == kShmXbgr8888 ? RGBA : BGRA; }
bool shm_has_alpha(uint32_t f) { return f == kShmAbgr8888 || f == kShmArgb8888; }

} // namespace

void VineFbSink::frame(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t shm_format) {
    if (!writer_.is_open() || w != writer_.width() || h != writer_.height()) return;
    const uint32_t fb = writer_.format();
    if (fb == vinefb::RGB_565) return;
    const Order dst = fb == vinefb::BGRA_8888 ? BGRA : RGBA;
    const bool dst_alpha = fb != vinefb::RGBX_8888;
    const Order src = shm_order(shm_format);
    if (src == dst && (shm_has_alpha(shm_format) || !dst_alpha)) {
        writer_.present(pixels, stride);
        return;
    }
    // Swizzle and/or make the padding byte opaque
    const uint32_t row = w * 4;
    scratch_.resize((size_t)row * h);
    const bool swap = src != dst, opaque = !shm_has_alpha(shm_format);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = pixels + (size_t)y * stride;
        uint8_t* d = scratch_.data() + (size_t)y * row;
        for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) {
            d[0] = swap ? s[2] : s[0];
            d[1] = s[1];
            d[2] = swap ? s[0] : s[2];
            d[3] = opaque ? 0xff : s[3];
        }
    }
    writer_.present(scratch_.data(), row);
}

bool create_vinefb(const std::string& path, uint32_t w, uint32_t h, std::string* error) {
    vinefb::Header hd{};
    hd.magic = vinefb::kMagic;
    hd.version = vinefb::kVersion;
    hd.width = w;
    hd.height = h;
    hd.stride = w * 4;
    hd.format = vinefb::RGBA_8888;
    hd.data_offset = vinefb::kHeaderSize;
    const uint32_t side = std::max(w, h);
    hd.capacity = side * side * 4;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        if (error) *error = "cannot create " + path + ": " + strerror(errno);
        return false;
    }
    const bool ok = pwrite(fd, &hd, sizeof hd, 0) == (ssize_t)sizeof hd &&
                    ftruncate(fd, (off_t)vinefb::kHeaderSize + hd.capacity) == 0;
    if (!ok && error) *error = "cannot size " + path + ": " + strerror(errno);
    ::close(fd);
    return ok;
}

} // namespace vine::wl
