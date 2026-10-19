#include "sparse_image.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace vine {

namespace {

constexpr uint32_t kSparseMagic = 0xED26FF3A;
constexpr uint16_t kChunkRaw = 0xCAC1;
constexpr uint16_t kChunkFill = 0xCAC2;
constexpr uint16_t kChunkDontCare = 0xCAC3;
constexpr uint16_t kChunkCrc32 = 0xCAC4;

struct SparseHeader {
    uint32_t magic;
    uint16_t major_version, minor_version;
    uint16_t file_hdr_sz, chunk_hdr_sz;
    uint32_t blk_sz, total_blks, total_chunks, image_checksum;
};

struct ChunkHeader {
    uint16_t chunk_type, reserved;
    uint32_t chunk_sz, total_sz;
};

bool read_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        done += (size_t)n;
    }
    return true;
}

} // namespace

bool is_android_sparse_image(const std::string& path) {
    auto head = read_file_prefix(path, 4);
    if (!head || head->size() < 4) return false;
    uint32_t magic = 0;
    memcpy(&magic, head->data(), 4);
    return magic == kSparseMagic; // the format is little-endian and so is every Android host
}

bool sparse_image_to_raw(const std::string& src, const std::string& dst, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        VINE_LOGE("sparse_image_to_raw: %s", msg.c_str());
        unlink(dst.c_str());
        return false;
    };

    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail("cannot open " + src);
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { close(in); return fail("cannot create " + dst); }

    struct Closer {
        int a, b;
        ~Closer() { close(a); close(b); }
    } closer{in, out};

    SparseHeader hdr{};
    if (!read_all(in, &hdr, sizeof(hdr)) || hdr.magic != kSparseMagic) return fail("not an Android sparse image");
    if (hdr.major_version != 1) return fail("unsupported sparse image version");
    if (hdr.file_hdr_sz < sizeof(hdr) || hdr.chunk_hdr_sz < sizeof(ChunkHeader)) return fail("bad sparse image header sizes");
    if (hdr.blk_sz == 0 || hdr.blk_sz % 4 != 0) return fail("bad sparse block size");
    if (hdr.file_hdr_sz > sizeof(hdr) && lseek(in, hdr.file_hdr_sz, SEEK_SET) < 0) return fail("seek failed");

    const uint64_t blk = hdr.blk_sz;
    uint64_t out_block = 0;
    std::vector<char> buf(1 << 20);

    for (uint32_t i = 0; i < hdr.total_chunks; ++i) {
        ChunkHeader ch{};
        if (!read_all(in, &ch, sizeof(ch))) return fail("truncated chunk header");
        if (hdr.chunk_hdr_sz > sizeof(ch) && lseek(in, hdr.chunk_hdr_sz - sizeof(ch), SEEK_CUR) < 0) return fail("seek failed");
        const uint64_t bytes = (uint64_t)ch.chunk_sz * blk;

        switch (ch.chunk_type) {
            case kChunkRaw: {
                if (lseek(out, (off_t)(out_block * blk), SEEK_SET) < 0) return fail("seek failed");
                uint64_t left = bytes;
                while (left > 0) {
                    size_t n = (size_t)std::min<uint64_t>(left, buf.size());
                    if (!read_all(in, buf.data(), n) || !write_all(out, buf.data(), n)) return fail("raw chunk copy failed");
                    left -= n;
                }
                break;
            }
            case kChunkFill: {
                uint32_t fill = 0;
                if (!read_all(in, &fill, sizeof(fill))) return fail("truncated fill chunk");
                if (fill != 0) { // an all-zero fill is just a hole
                    if (lseek(out, (off_t)(out_block * blk), SEEK_SET) < 0) return fail("seek failed");
                    const size_t words = buf.size() / sizeof(uint32_t);
                    auto* w = reinterpret_cast<uint32_t*>(buf.data());
                    for (size_t k = 0; k < words; ++k) w[k] = fill;
                    uint64_t left = bytes;
                    while (left > 0) {
                        size_t n = (size_t)std::min<uint64_t>(left, buf.size());
                        if (!write_all(out, buf.data(), n)) return fail("fill chunk write failed");
                        left -= n;
                    }
                }
                break;
            }
            case kChunkDontCare:
                break;
            case kChunkCrc32:
                if (lseek(in, 4, SEEK_CUR) < 0) return fail("seek failed");
                continue; // CRC chunks cover no output blocks
            default:
                return fail("unknown sparse chunk type");
        }
        out_block += ch.chunk_sz;
    }

    if (ftruncate(out, (off_t)((uint64_t)hdr.total_blks * blk)) != 0) return fail("cannot size output");
    return true;
}

} // namespace vine
