// Checks the vinefb reader and the pixel converters, including a guest writer racing the reader.
#include "../../display/pixel_convert.h"
#include "../../display/vinefb_reader.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vine::display;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        ++g_checks;                                               \
        if (!(cond)) {                                            \
            ++g_failed;                                           \
            printf("  FAIL line %d: %s  ", __LINE__, #cond);      \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while (0)

static void test_convert() {
    printf("[pixel conversion]\n");
    uint8_t out[16];
    const uint8_t rgba[4] = {1, 2, 3, 4};
    convert_rows(VineFbFormat::RGBA_8888, rgba, 4, out, 4, 1, 1);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4, "rgba passthrough");
    const uint8_t bgra[4] = {10, 20, 30, 40}; // B G R A
    convert_rows(VineFbFormat::BGRA_8888, bgra, 4, out, 4, 1, 1);
    CHECK(out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 40, "bgra swap %d %d %d %d", out[0], out[1], out[2], out[3]);
    const uint8_t rgbx[4] = {5, 6, 7, 0};
    convert_rows(VineFbFormat::RGBX_8888, rgbx, 4, out, 4, 1, 1);
    CHECK(out[0] == 5 && out[1] == 6 && out[2] == 7 && out[3] == 255, "rgbx alpha");
    const uint16_t px[3] = {0xF800, 0x07E0, 0x001F}; // red, green, blue
    uint8_t out3[12];
    convert_rows(VineFbFormat::RGB_565, reinterpret_cast<const uint8_t*>(px), 6, out3, 12, 3, 1);
    CHECK(out3[0] == 255 && out3[1] == 0 && out3[2] == 0 && out3[3] == 255, "565 red");
    CHECK(out3[4] == 0 && out3[5] == 255 && out3[6] == 0, "565 green");
    CHECK(out3[8] == 0 && out3[9] == 0 && out3[10] == 255, "565 blue");
    const uint8_t two_rows[16] = {1, 1, 1, 1, 9, 9, 9, 9, 2, 2, 2, 2, 9, 9, 9, 9};
    uint8_t dst[24] = {0};
    convert_rows(VineFbFormat::RGBA_8888, two_rows, 8, dst, 12, 1, 2);
    CHECK(dst[0] == 1 && dst[12] == 2, "strided rows");
}

static void test_reader() {
    printf("[vinefb reader]\n");
    char path[] = "/tmp/vinefb_test.XXXXXX";
    int fd = mkstemp(path);
    const uint32_t w = 64, h = 48, cap = w * h * 4;
    if (ftruncate(fd, kVineFbHeaderSize + cap) != 0) return;
    uint8_t* base = static_cast<uint8_t*>(mmap(nullptr, kVineFbHeaderSize + cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    auto* hd = reinterpret_cast<VineFbHeader*>(base);
    hd->magic = kVineFbMagic;
    hd->version = kVineFbVersion;
    hd->width = w;
    hd->height = h;
    hd->stride = w * 4;
    hd->format = (uint32_t)VineFbFormat::RGBA_8888;
    hd->data_offset = kVineFbHeaderSize;
    hd->capacity = cap;

    VineFbReader r;
    std::string err;
    CHECK(r.open(path, &err), "open: %s", err.c_str());
    FrameInfo info;
    CHECK(r.header(&info) && info.width == w && info.height == h, "header");
    std::vector<uint8_t> dst(w * 4 * h);
    CHECK(!r.copy_frame(dst.data(), w * 4, h, &info), "no frame yet must not copy");

    // Each frame is one solid colour, so any mix of colours in a copy is a tear
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint32_t seq = 0, frame = 0;
        while (!stop.load()) {
            __atomic_store_n(&hd->frame_seq, ++seq, __ATOMIC_RELEASE); // odd: writing
            const uint8_t v = (uint8_t)(++frame);
            memset(base + kVineFbHeaderSize, v, cap);
            hd->frame_count = frame;
            __atomic_store_n(&hd->frame_seq, ++seq, __ATOMIC_RELEASE); // even: done
            usleep(150);
        }
    });
    while (r.sequence() == 0) usleep(100); // let the writer produce its first frame
    int copied = 0, torn = 0;
    for (int i = 0; i < 20000; ++i) {
        if (!r.copy_frame(dst.data(), w * 4, h, &info)) continue;
        ++copied;
        const uint8_t first = dst[0];
        for (size_t j = 0; j < dst.size(); ++j) {
            if (dst[j] != first) {
                ++torn;
                break;
            }
        }
    }
    stop.store(true);
    writer.join();
    CHECK(copied > 100, "only %d frames copied", copied);
    CHECK(torn == 0, "%d torn frames out of %d", torn, copied);
    CHECK((info.frame_seq & 1) == 0, "returned an odd sequence");

    hd->stride = w * 4 - 1;
    CHECK(!r.header(&info), "invalid stride accepted");
    hd->stride = w * 4;
    hd->capacity = 16;
    CHECK(!r.header(&info), "capacity smaller than the frame accepted");
    hd->capacity = cap;
    hd->magic = 0;
    CHECK(!r.header(&info), "bad magic accepted");

    VineFbReader missing;
    CHECK(!missing.open("/nonexistent/vinefb", &err) && !err.empty(), "missing file");
    r.close();
    munmap(base, kVineFbHeaderSize + cap);
    unlink(path);
}

int main() {
    test_convert();
    test_reader();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
