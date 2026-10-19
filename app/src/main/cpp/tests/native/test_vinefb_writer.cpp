// Checks the guest side writer from rom/hwc against the host reader, including a writer racing the reader.
#include "../../display/vinefb_reader.h"
#include "vinefb_writer.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vine::display;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        ++g_checks;                                          \
        if (!(cond)) {                                       \
            ++g_failed;                                      \
            printf("  FAIL line %d: %s  ", __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

static void make_host_file(const char* path, uint32_t w, uint32_t h, uint32_t format, uint32_t stride) {
    const uint32_t cap = stride * h;
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, kVineFbHeaderSize + cap) != 0) return;
    VineFbHeader hd{};
    hd.magic = kVineFbMagic;
    hd.version = kVineFbVersion;
    hd.width = w;
    hd.height = h;
    hd.stride = stride;
    hd.format = format;
    hd.data_offset = kVineFbHeaderSize;
    hd.capacity = cap;
    if (pwrite(fd, &hd, sizeof(hd), 0) != (ssize_t)sizeof(hd)) return;
    close(fd);
}

int main() {
    char path[] = "/tmp/vinefb_writer.XXXXXX";
    close(mkstemp(path));
    const uint32_t w = 64, h = 48;
    make_host_file(path, w, h, (uint32_t)VineFbFormat::RGBA_8888, w * 4);

    printf("[vinefb writer]\n");
    vinefb::Writer wr;
    CHECK(wr.open(path), "open");
    CHECK(wr.width() == w && wr.height() == h, "size");

    VineFbReader r;
    std::string err;
    CHECK(r.open(path, &err), "reader open: %s", err.c_str());
    std::vector<uint8_t> dst(w * 4 * h);
    FrameInfo info;
    CHECK(!r.copy_frame(dst.data(), w * 4, h, &info), "no frame before the first present");

    std::vector<uint8_t> frame(w * 4 * h, 0x7f);
    CHECK(wr.present(frame.data(), w * 4), "present");
    CHECK(r.copy_frame(dst.data(), w * 4, h, &info) && dst[0] == 0x7f && dst.back() == 0x7f, "frame arrives");
    CHECK((info.frame_seq & 1) == 0 && info.frame_seq == 2, "sequence %u", info.frame_seq);

    // A source with padding at the end of every row
    std::vector<uint8_t> padded((w * 4 + 16) * h, 0);
    for (uint32_t y = 0; y < h; ++y) memset(&padded[y * (w * 4 + 16)], (int)(y + 1), w * 4);
    CHECK(wr.present(padded.data(), w * 4 + 16), "present padded");
    CHECK(r.copy_frame(dst.data(), w * 4, h, &info) && dst[0] == 1 && dst[(h - 1) * w * 4] == h, "rows are copied one by one");
    CHECK(!wr.present(frame.data(), w * 4 - 1), "a source narrower than a row is refused");
    CHECK(!wr.present(nullptr, w * 4), "null pixels are refused");

    // An interrupted write leaves the sequence odd, the next present must repair it
    {
        int fd = open(path, O_RDWR);
        uint32_t odd = 7;
        if (pwrite(fd, &odd, 4, offsetof(VineFbHeader, frame_seq)) != 4) return 2;
        close(fd);
        CHECK(!r.copy_frame(dst.data(), w * 4, h, &info), "odd sequence reads as no frame");
        CHECK(wr.present(frame.data(), w * 4), "present after a cut short write");
        CHECK(r.copy_frame(dst.data(), w * 4, h, &info) && (info.frame_seq & 1) == 0, "recovered");
    }

    // Race: solid frames from the writer thread, any mixed copy is a tear
    std::atomic<bool> stop{false};
    std::thread t([&] {
        std::vector<uint8_t> f(w * 4 * h);
        uint8_t v = 0;
        while (!stop.load()) {
            memset(f.data(), ++v, f.size());
            wr.present(f.data(), w * 4);
            usleep(120);
        }
    });
    int copied = 0, torn = 0;
    for (int i = 0; i < 20000; ++i) {
        if (!r.copy_frame(dst.data(), w * 4, h, &info)) continue;
        ++copied;
        for (size_t j = 1; j < dst.size(); ++j) {
            if (dst[j] != dst[0]) {
                ++torn;
                break;
            }
        }
    }
    stop.store(true);
    t.join();
    CHECK(copied > 100, "only %d frames copied", copied);
    CHECK(torn == 0, "%d torn frames out of %d", torn, copied);

    // A file the host did not shape properly is refused
    make_host_file(path, w, h, (uint32_t)VineFbFormat::RGBA_8888, w * 2);
    vinefb::Writer bad;
    CHECK(!bad.open(path), "stride smaller than a row accepted");
    vinefb::Writer missing;
    CHECK(!missing.open("/nonexistent/vinefb"), "missing file accepted");

    unlink(path);
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
