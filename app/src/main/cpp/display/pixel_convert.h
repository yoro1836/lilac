#pragma once

#include "vinefb.h"

#include <cstddef>
#include <cstdint>

namespace vine::display {

// Converts rows of vinefb pixels to RGBA_8888, strides in bytes
void convert_rows(VineFbFormat format, const uint8_t* src, size_t src_stride, uint8_t* dst, size_t dst_stride,
                  uint32_t width, uint32_t rows);

} // namespace vine::display
