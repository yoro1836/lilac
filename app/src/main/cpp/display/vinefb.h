#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>

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
    // Nonzero: the frame is this host GPU resource (a gfxstream
    // ColorBuffer, gpu_format an Android HAL format), not the pixels
    uint32_t gpu_resource;
    uint32_t gpu_width;
    uint32_t gpu_height;
    uint32_t gpu_format;
    // Written by the host: frame_seq of the newest gpu_resource frame on
    // screen. Older GPU buffers are free for the guest to draw into again.
    uint32_t host_shown_seq;
    uint32_t reserved;
};

static_assert(sizeof(VineFbHeader) == 64, "VineFbHeader is part of the guest ABI");

// The abstract socket on which the instance's GPU server hands out the
// AHardwareBuffers behind gpu_resource frames (vine_kumquat's
// AHB_SOCKET). Named after the directory holding vinefb, by inode: the
// app and the helper may spell its path differently. Empty if it is gone.
inline std::string gpu_ahb_socket(const std::string& vinefb_path) {
    const size_t slash = vinefb_path.rfind('/');
    struct stat st {};
    if (slash == std::string::npos || stat(vinefb_path.substr(0, slash).c_str(), &st) != 0) return {};
    char name[48];
    snprintf(name, sizeof name, "vine-ahb-%llx-%llx", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
    return name;
}

inline uint32_t bytes_per_pixel(VineFbFormat f) { return f == VineFbFormat::RGB_565 ? 2u : 4u; }

} // namespace vine::display
