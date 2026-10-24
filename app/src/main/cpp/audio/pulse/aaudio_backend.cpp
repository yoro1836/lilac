#include "aaudio_backend.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace vine::pa {

namespace {

aaudio_format_t aaudio_format(uint8_t format) {
    switch (format) {
    case kS16le: return AAUDIO_FORMAT_PCM_I16;
    case kFloat32le: return AAUDIO_FORMAT_PCM_FLOAT;
    default: return AAUDIO_FORMAT_INVALID;
    }
}

class AAudioDevice : public Device {
public:
    AAudioDevice(bool input, const SampleSpec& ss, Backend::Pull pull, Backend::Push push,
                 std::function<void(const std::string&)> log)
        : input_(input), ss_(ss), pull_(std::move(pull)), push_(std::move(push)), log_(std::move(log)) {}

    ~AAudioDevice() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        if (supervisor_.joinable()) supervisor_.join();
        close_stream();
    }

    bool start() {
        if (!open_stream()) return false;
        // Reopens the stream after a disconnect; AAudio forbids doing it
        // from its own callbacks
        supervisor_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mu_);
            for (;;) {
                cv_.wait(lock, [this] { return quit_ || reopen_; });
                if (quit_) return;
                reopen_ = false;
                lock.unlock();
                close_stream();
                if (!open_stream()) log_("cannot reopen the host audio stream");
                lock.lock();
            }
        });
        return true;
    }

private:
    static aaudio_data_callback_result_t on_data(AAudioStream*, void* user, void* audio, int32_t frames) {
        auto* d = static_cast<AAudioDevice*>(user);
        const size_t bytes = (size_t)frames * frame_size(d->ss_);
        if (d->input_) d->push_(static_cast<const uint8_t*>(audio), bytes);
        else d->pull_(static_cast<uint8_t*>(audio), bytes);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void on_error(AAudioStream*, void* user, aaudio_result_t error) {
        auto* d = static_cast<AAudioDevice*>(user);
        if (error != AAUDIO_ERROR_DISCONNECTED) return;
        {
            std::lock_guard<std::mutex> lock(d->mu_);
            d->reopen_ = true;
        }
        d->cv_.notify_all();
    }

    bool open_stream() {
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setDirection(b, input_ ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)ss_.rate);
        AAudioStreamBuilder_setChannelCount(b, ss_.channels);
        AAudioStreamBuilder_setFormat(b, aaudio_format(ss_.format));
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setDataCallback(b, on_data, this);
        AAudioStreamBuilder_setErrorCallback(b, on_error, this);
        AAudioStream* s = nullptr;
        aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
        AAudioStreamBuilder_delete(b);
        if (r == AAUDIO_OK) {
            r = AAudioStream_requestStart(s);
            if (r != AAUDIO_OK) AAudioStream_close(s);
        }
        if (r != AAUDIO_OK) {
            log_(std::string("AAudio ") + (input_ ? "input" : "output") + ": " + AAudio_convertResultToText(r));
            return false;
        }
        std::lock_guard<std::mutex> lock(stream_mu_);
        stream_ = s;
        return true;
    }

    void close_stream() {
        std::lock_guard<std::mutex> lock(stream_mu_);
        if (!stream_) return;
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }

    const bool input_;
    const SampleSpec ss_;
    Backend::Pull pull_;
    Backend::Push push_;
    std::function<void(const std::string&)> log_;
    std::mutex stream_mu_;
    AAudioStream* stream_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false, reopen_ = false;
    std::thread supervisor_;
};

} // namespace

std::unique_ptr<Device> AAudioBackend::open_output(const SampleSpec& ss, Pull pull) {
    if (aaudio_format(ss.format) != AAUDIO_FORMAT_INVALID) {
        auto d = std::make_unique<AAudioDevice>(false, ss, pull, nullptr, log_);
        if (d->start()) return d;
    } else {
        log_("no AAudio format for " + std::to_string(ss.format) + "; output dropped");
    }
    return clock_.open_output(ss, std::move(pull));
}

std::unique_ptr<Device> AAudioBackend::open_input(const SampleSpec& ss, Push push) {
    if (aaudio_format(ss.format) != AAUDIO_FORMAT_INVALID) {
        auto d = std::make_unique<AAudioDevice>(true, ss, nullptr, push, log_);
        if (d->start()) return d;
    }
    log_("capturing silence");
    return clock_.open_input(ss, std::move(push));
}

} // namespace vine::pa
