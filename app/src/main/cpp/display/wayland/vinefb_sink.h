#pragma once

#include "wl_server.h"
#include "vinefb_writer.h"

#include <string>
#include <vector>

namespace vine::wl {

// Publishes committed frames into a vinefb file the host created; the
// output size is the file's geometry. Frames of another size (the
// client's 1x1 calibration buffer) are dropped.
class VineFbSink : public FrameSink {
public:
    bool open(const std::string& path) { return writer_.open(path.c_str()); }
    uint32_t width() const override { return writer_.width(); }
    uint32_t height() const override { return writer_.height(); }
    void frame(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t shm_format) override;

private:
    vinefb::Writer writer_;
    std::vector<uint8_t> scratch_;
};

// Creates (or replaces) a vinefb file for a w x h RGBA display, with room
// for either orientation, like the host's own setup
bool create_vinefb(const std::string& path, uint32_t w, uint32_t h, std::string* error);

} // namespace vine::wl
