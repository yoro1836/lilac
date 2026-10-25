// Guest side of the vinefb display contract, header only
#pragma once

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vinefb {

constexpr uint32_t kMagic = 0x31424656; // "VFB1"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderSize = 4096;

enum Format : uint32_t { RGBA_8888 = 0, BGRA_8888 = 1, RGB_565 = 2, RGBX_8888 = 3 };

struct Header {
    uint32_t magic, version, width, height, stride, format, data_offset, capacity;
    uint32_t frame_seq, frame_count;
    // Nonzero: the frame is the host GPU resource with this id (a
    // gfxstream ColorBuffer, gpu_format an Android HAL format), not the
    // pixels at data_offset
    uint32_t gpu_resource, gpu_width, gpu_height, gpu_format;
    uint32_t reserved[2];
};
static_assert(sizeof(Header) == 64, "Header is part of the guest ABI");

class Writer {
public:
    Writer() = default;
    ~Writer() { close(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Maps the file the host created at <instance>/shared/vinefb, visible in the guest as /vine/vinefb
    bool open(const char* path = "/vine/vinefb") {
        const int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st {};
        if (fstat(fd, &st) != 0 || st.st_size < (off_t)kHeaderSize) {
            ::close(fd);
            return false;
        }
        void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        base_ = static_cast<uint8_t*>(p);
        size_ = (size_t)st.st_size;
        const Header* h = header();
        const bool ok = h->magic == kMagic && h->version == kVersion && h->data_offset == kHeaderSize && h->stride >= h->width * bpp(h->format) &&
                        (uint64_t)h->stride * h->height <= h->capacity && (uint64_t)kHeaderSize + h->capacity <= size_;
        if (!ok) close();
        return ok;
    }

    void close() {
        if (base_) munmap(base_, size_);
        base_ = nullptr;
        size_ = 0;
    }

    bool is_open() const { return base_ != nullptr; }
    const Header* header() const { return reinterpret_cast<const Header*>(base_); }
    uint32_t width() const { return header()->width; }
    uint32_t height() const { return header()->height; }
    uint32_t format() const { return header()->format; }

    // Publishes one full frame, src_stride in bytes, a reader never sees a half-written frame
    bool present(const void* pixels, uint32_t src_stride) {
        if (!base_ || !pixels) return false;
        Header* h = hdr();
        const uint32_t row = h->width * bpp(h->format);
        if (src_stride < row) return false;
        const uint32_t base_seq = h->frame_seq & ~1u; // an odd value means an earlier write was cut short
        __atomic_store_n(&h->frame_seq, base_seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        uint8_t* dst = base_ + h->data_offset;
        const uint8_t* src = static_cast<const uint8_t*>(pixels);
        if (src_stride == h->stride) {
            memcpy(dst, src, (size_t)h->stride * h->height);
        } else {
            for (uint32_t y = 0; y < h->height; ++y) memcpy(dst + (size_t)y * h->stride, src + (size_t)y * src_stride, row);
        }
        h->gpu_resource = 0;
        h->frame_count = h->frame_count + 1;
        __atomic_store_n(&h->frame_seq, base_seq + 2, __ATOMIC_RELEASE);
        return true;
    }

    // Publishes a frame that stays on the host GPU
    bool present_gpu(uint32_t resource, uint32_t width, uint32_t height, uint32_t format) {
        if (!base_ || !resource) return false;
        Header* h = hdr();
        const uint32_t base_seq = h->frame_seq & ~1u;
        __atomic_store_n(&h->frame_seq, base_seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        h->gpu_resource = resource;
        h->gpu_width = width;
        h->gpu_height = height;
        h->gpu_format = format;
        h->frame_count = h->frame_count + 1;
        __atomic_store_n(&h->frame_seq, base_seq + 2, __ATOMIC_RELEASE);
        return true;
    }

private:
    static uint32_t bpp(uint32_t format) { return format == RGB_565 ? 2u : 4u; }
    Header* hdr() { return reinterpret_cast<Header*>(base_); }

    uint8_t* base_ = nullptr;
    size_t size_ = 0;
};

} // namespace vinefb
