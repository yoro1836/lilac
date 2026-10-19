#pragma once

#include <cstdint>

// vinefb is the display contract between a guest and the host app, see ROM_INTEGRATION.md in the docs repo
namespace vine::display {

constexpr uint32_t kVineFbMagic = 0x31424656; // "VFB1"
constexpr uint32_t kVineFbVersion = 1;
constexpr uint32_t kVineFbHeaderSize = 4096;  // pixels start here, page aligned for mmap

enum class VineFbFormat : uint32_t {
    RGBA_8888 = 0, // bytes in memory: R G B A (Android HAL_PIXEL_FORMAT_RGBA_8888)
    BGRA_8888 = 1,
    RGB_565 = 2,
    RGBX_8888 = 3,
};

struct VineFbHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;      // bytes per row
    uint32_t format;      // VineFbFormat
    uint32_t data_offset; // always kVineFbHeaderSize in v1
    uint32_t capacity;    // bytes available at data_offset
    // frame_seq is odd while the guest writes and even once a frame is complete
    uint32_t frame_seq;
    uint32_t frame_count; // frames presented since boot
    uint32_t reserved[6];
};

static_assert(sizeof(VineFbHeader) == 64, "VineFbHeader is part of the guest ABI");

inline uint32_t bytes_per_pixel(VineFbFormat f) { return f == VineFbFormat::RGB_565 ? 2u : 4u; }

} // namespace vine::display
