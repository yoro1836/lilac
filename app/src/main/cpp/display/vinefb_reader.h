#pragma once

#include "vinefb.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace vine::display {

struct FrameInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    VineFbFormat format = VineFbFormat::RGBA_8888;
    uint32_t frame_seq = 0;
    uint32_t frame_count = 0;
    // Nonzero: the frame is this host GPU resource (vinefb.h)
    uint32_t gpu_resource = 0;
    uint32_t gpu_width = 0;
    uint32_t gpu_height = 0;
};

// Maps the vinefb file read-only and returns tear-free copies of the newest frame
class VineFbReader {
public:
    VineFbReader() = default;
    ~VineFbReader() { close(); }

    VineFbReader(const VineFbReader&) = delete;
    VineFbReader& operator=(const VineFbReader&) = delete;

    bool open(const std::string& path, std::string* error);
    void close();
    bool is_open() const { return base_ != nullptr; }

    // True when the path now names another file than the one mapped (the
    // writer replaced it, e.g. a guest restart); open() it again then
    bool replaced() const;
    const std::string& path() const { return path_; }

    // Current frame sequence number: 0 before the first frame, odd while the guest is writing, even when stable.
    uint32_t sequence() const;

    // Header as it is right now. False when the file does not hold a valid vinefb header yet.
    bool header(FrameInfo* info) const;

    // Copies the newest finished frame as RGBA_8888, false if none yet or the writer kept racing
    bool copy_frame(uint8_t* dst, size_t dst_stride, uint32_t dst_rows, FrameInfo* info);

private:
    const VineFbHeader* hdr() const { return static_cast<const VineFbHeader*>(base_); }
    bool valid_geometry(const VineFbHeader& h) const;

    void* base_ = nullptr;
    size_t size_ = 0;
    std::string path_;
    uint64_t dev_ = 0, ino_ = 0;
};

} // namespace vine::display
