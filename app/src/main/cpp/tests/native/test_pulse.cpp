// Drives vine_pulse with real PulseAudio clients: libpulse's simple API
// for playback, drain, latency and capture, then ALSA's pulse plugin the
// way Waydroid's audio HAL opens it (snd_pcm_open("pulse"), writei and
// readi).
// The server's device is a clock that records what it plays.
#include "../../audio/pulse/pa_server.h"

#include <alsa/asoundlib.h>
#include <pulse/error.h>
#include <pulse/simple.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

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

namespace {

// Everything the clock device played, in order
struct Tap {
    std::mutex mu;
    std::vector<uint8_t> played;
    void add(const uint8_t* p, size_t n) {
        std::lock_guard<std::mutex> lock(mu);
        played.insert(played.end(), p, p + n);
    }
    // Non-silent bytes played, as one sequence
    std::vector<uint8_t> audible() {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<uint8_t> out;
        size_t first = played.size(), last = 0;
        for (size_t i = 0; i < played.size(); ++i)
            if (played[i]) {
                first = std::min(first, i);
                last = i + 1;
            }
        if (first < last) out.assign(played.begin() + (ptrdiff_t)first, played.begin() + (ptrdiff_t)last);
        return out;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu);
        played.clear();
    }
};

// 16-bit stereo frames whose samples count up, never zero
std::vector<int16_t> ramp(size_t frames) {
    std::vector<int16_t> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = (int16_t)(1 + i % 30000);
    return v;
}

bool same_samples(const std::vector<uint8_t>& got, const std::vector<int16_t>& want) {
    return got.size() == want.size() * 2 && memcmp(got.data(), want.data(), got.size()) == 0;
}

void test_simple(const std::string& server, Tap& tap) {
    printf("[libpulse simple API]\n");
    const pa_sample_spec ss = {PA_SAMPLE_S16LE, 48000, 2};
    int err = 0;
    pa_simple* out = pa_simple_new(server.c_str(), "test", PA_STREAM_PLAYBACK, nullptr, "playback", &ss, nullptr,
                                   nullptr, &err);
    CHECK(out, "playback stream: %s", pa_strerror(err));
    if (!out) return;
    const auto pcm = ramp(48000 * 3 / 10); // 300 ms
    CHECK(pa_simple_write(out, pcm.data(), pcm.size() * 2, &err) == 0, "write: %s", pa_strerror(err));
    const pa_usec_t latency = pa_simple_get_latency(out, &err);
    CHECK(latency != (pa_usec_t)-1, "latency: %s", pa_strerror(err));
    CHECK(pa_simple_drain(out, &err) == 0, "drain: %s", pa_strerror(err));
    pa_simple_free(out);
    const auto got = tap.audible();
    CHECK(same_samples(got, pcm), "played %zu bytes, wrote %zu, in order", got.size(), pcm.size() * 2);

    pa_simple* in = pa_simple_new(server.c_str(), "test", PA_STREAM_RECORD, nullptr, "record", &ss, nullptr, nullptr,
                                  &err);
    CHECK(in, "record stream: %s", pa_strerror(err));
    if (!in) return;
    std::vector<uint8_t> buf(48000 * 4 / 10, 0xaa); // 100 ms
    CHECK(pa_simple_read(in, buf.data(), buf.size(), &err) == 0, "read: %s", pa_strerror(err));
    bool silent = true;
    for (uint8_t b : buf) silent = silent && b == 0;
    CHECK(silent, "the clock device records silence");
    pa_simple_free(in);
}

void test_alsa(const std::string& server, Tap& tap) {
    printf("[ALSA pulse plugin, as the HAL uses it]\n");
    tap.clear();
    setenv("PULSE_SERVER", server.c_str(), 1);
    snd_pcm_t* pcm = nullptr;
    int r = snd_pcm_open(&pcm, "pulse", SND_PCM_STREAM_PLAYBACK, 0);
    CHECK(r == 0, "snd_pcm_open: %s", snd_strerror(r));
    if (r) return;
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned rate = 48000;
    r = snd_pcm_hw_params_any(pcm, hw) < 0 ? -1 : 0; // a positive count is fine
    if (!r) r = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (!r) r = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (!r) r = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
    if (!r) r = snd_pcm_hw_params_set_channels(pcm, hw, 2);
    if (!r) r = snd_pcm_hw_params_set_periods(pcm, hw, 4, 0);
    if (!r) r = snd_pcm_hw_params_set_buffer_size(pcm, hw, 4 * 1024);
    if (!r) r = snd_pcm_hw_params(pcm, hw);
    if (!r) r = snd_pcm_prepare(pcm);
    CHECK(r == 0, "hw params: %s", snd_strerror(r));
    const auto data = ramp(48000 / 2); // 500 ms in 1024-frame writes, like the HAL
    size_t done = 0;
    while (r == 0 && done < data.size() / 2) {
        const size_t n = std::min<size_t>(1024, data.size() / 2 - done);
        const snd_pcm_sframes_t w = snd_pcm_writei(pcm, data.data() + 2 * done, n);
        if (w < 0) {
            r = (int)w;
            break;
        }
        done += (size_t)w;
    }
    CHECK(r == 0 && done == data.size() / 2, "writei: %s (%zu frames)", snd_strerror(r), done);
    r = snd_pcm_drain(pcm);
    CHECK(r == 0, "drain: %s", snd_strerror(r));
    snd_pcm_close(pcm);
    const auto got = tap.audible();
    CHECK(same_samples(got, data), "played %zu bytes, wrote %zu, in order", got.size(), data.size() * 2);
}

void test_alsa_capture(const std::string& server) {
    printf("[ALSA pulse plugin capture, 16 kHz as the HAL opens it]\n");
    setenv("PULSE_SERVER", server.c_str(), 1);
    snd_pcm_t* pcm = nullptr;
    int r = snd_pcm_open(&pcm, "pulse", SND_PCM_STREAM_CAPTURE, 0);
    CHECK(r == 0, "snd_pcm_open: %s", snd_strerror(r));
    if (r) return;
    r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 16000, 1, 100000);
    CHECK(r == 0, "set_params: %s", snd_strerror(r));
    std::vector<int16_t> buf(1600 * 2, 7);
    size_t got = 0;
    for (int i = 0; i < 3 && r == 0; ++i) {
        const snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf.data(), 1600);
        if (n < 0) r = (int)n;
        else got += (size_t)n;
    }
    CHECK(r == 0 && got == 4800, "readi: %s (%zu frames)", snd_strerror(r), got);
    bool silent = true;
    for (int16_t v : buf) silent = silent && v == 0;
    CHECK(silent, "silence captured");
    snd_pcm_close(pcm);
}

} // namespace

int main() {
    char tmpl[] = "/tmp/vine_pulse_test.XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string path = dir + "/native";
    Tap tap;
    vine::pa::ClockBackend backend([&](const uint8_t* p, size_t n) { tap.add(p, n); });
    vine::pa::Server server(&backend, [](const std::string& m) { fprintf(stderr, "vine_pulse: %s\n", m.c_str()); });
    std::string err;
    CHECK(server.listen(path, &err), "listen: %s", err.c_str());
    std::thread th([&] { server.run(); });
    setenv("PULSE_CLIENTCONFIG", "/dev/null", 1); // no autospawn, no cookie lookups elsewhere
    test_simple("unix:" + path, tap);
    test_alsa("unix:" + path, tap);
    test_alsa_capture("unix:" + path);
    server.stop();
    th.join();
    unlink(path.c_str());
    rmdir(dir.c_str());
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
