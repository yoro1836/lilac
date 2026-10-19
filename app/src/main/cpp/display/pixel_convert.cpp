#include "pixel_convert.h"

#include <cstring>

namespace vine::display {

namespace {

inline uint32_t load32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline void store32(uint8_t* p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

} // namespace

void convert_rows(VineFbFormat format, const uint8_t* src, size_t src_stride, uint8_t* dst, size_t dst_stride,
                  uint32_t width, uint32_t rows) {
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* s = src + y * src_stride;
        uint8_t* d = dst + y * dst_stride;
        switch (format) {
            case VineFbFormat::RGBA_8888:
                memcpy(d, s, (size_t)width * 4);
                break;
            case VineFbFormat::RGBX_8888:
                for (uint32_t x = 0; x < width; ++x) store32(d + x * 4, load32(s + x * 4) | 0xFF000000u);
                break;
            case VineFbFormat::BGRA_8888:
                // memory order B G R A, read little-endian that is A R G B in the register
                for (uint32_t x = 0; x < width; ++x) {
                    const uint32_t px = load32(s + x * 4);
                    store32(d + x * 4, (px & 0xFF00FF00u) | ((px & 0x000000FFu) << 16) | ((px >> 16) & 0xFFu));
                }
                break;
            case VineFbFormat::RGB_565:
                for (uint32_t x = 0; x < width; ++x) {
                    uint16_t px;
                    memcpy(&px, s + x * 2, sizeof(px));
                    const uint32_t r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
                    const uint32_t r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
                    store32(d + x * 4, r | (g << 8) | (b << 16) | 0xFF000000u);
                }
                break;
        }
    }
}

} // namespace vine::display
