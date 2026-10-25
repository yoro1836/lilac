#include "vinefb_reader.h"
#include "pixel_convert.h"
#include "../utils/vine_log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vine::display {

bool VineFbReader::open(const std::string& path, std::string* error) {
    close();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "cannot open " + path + ": " + strerror(errno);
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < kVineFbHeaderSize) {
        ::close(fd);
        if (error) *error = "display buffer is missing or too small";
        return false;
    }
    void* map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        if (error) *error = std::string("mmap: ") + strerror(errno);
        return false;
    }
    base_ = map;
    size_ = (size_t)st.st_size;
    path_ = path;
    dev_ = (uint64_t)st.st_dev;
    ino_ = (uint64_t)st.st_ino;
    return true;
}

bool VineFbReader::replaced() const {
    struct stat st{};
    if (path_.empty() || stat(path_.c_str(), &st) != 0) return false;
    return !base_ || (uint64_t)st.st_dev != dev_ || (uint64_t)st.st_ino != ino_;
}

void VineFbReader::close() {
    if (base_) munmap(base_, size_);
    base_ = nullptr;
    size_ = 0;
}

uint32_t VineFbReader::sequence() const {
    if (!base_) return 0;
    return __atomic_load_n(&hdr()->frame_seq, __ATOMIC_ACQUIRE);
}

bool VineFbReader::valid_geometry(const VineFbHeader& h) const {
    if (h.magic != kVineFbMagic || h.version != kVineFbVersion) return false;
    if (h.width == 0 || h.height == 0 || h.width > 16384 || h.height > 16384) return false;
    if (h.format > (uint32_t)VineFbFormat::RGBX_8888) return false;
    const uint32_t bpp = bytes_per_pixel((VineFbFormat)h.format);
    if (h.stride < h.width * bpp) return false;
    if (h.data_offset < kVineFbHeaderSize) return false;
    const uint64_t needed = (uint64_t)h.stride * h.height;
    return needed <= h.capacity && (uint64_t)h.data_offset + needed <= size_;
}

bool VineFbReader::header(FrameInfo* info) const {
    if (!base_) return false;
    VineFbHeader h;
    memcpy(&h, base_, sizeof(h));
    if (!valid_geometry(h)) return false;
    if (info) {
        info->width = h.width;
        info->height = h.height;
        info->stride = h.stride;
        info->format = (VineFbFormat)h.format;
        info->frame_seq = h.frame_seq;
        info->frame_count = h.frame_count;
    }
    return true;
}

bool VineFbReader::copy_frame(uint8_t* dst, size_t dst_stride, uint32_t dst_rows, FrameInfo* info) {
    if (!base_ || !dst) return false;
    for (int attempt = 0; attempt < 6; ++attempt) {
        const uint32_t before = sequence();
        if (before == 0) return false;
        if (before & 1u) { // the guest is mid-frame
            sched_yield();
            continue;
        }
        VineFbHeader h;
        memcpy(&h, base_, sizeof(h));
        if (!valid_geometry(h)) return false;

        const uint32_t rows = std::min(h.height, dst_rows);
        const size_t row_bytes = (size_t)h.width * 4;
        if (dst_stride < row_bytes) return false;
        const auto* pixels = static_cast<const uint8_t*>(base_) + h.data_offset;
        convert_rows((VineFbFormat)h.format, pixels, h.stride, dst, dst_stride, h.width, rows);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&hdr()->frame_seq, __ATOMIC_RELAXED) != before) continue; // overwritten while copying
        if (info) {
            info->width = h.width;
            info->height = h.height;
            info->stride = h.stride;
            info->format = (VineFbFormat)h.format;
            info->frame_seq = before;
            info->frame_count = h.frame_count;
        }
        return true;
    }
    return false;
}

} // namespace vine::display
