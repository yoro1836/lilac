#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pa_wire.h"

// A small PulseAudio server for one kind of client: Waydroid's audio HAL
// (audio.primary.waydroid, ALSA's pulse plugin on libpulse 12), which
// connects to /run/user/1000/pulse/native. Native protocol version 32,
// no shared memory: audio travels inline in the stream. Each playback
// stream gets its own host output and each record stream its own host
// input; the host mixes. No volumes, no sinks to choose from.
namespace vine::pa {

// The host's audio for one stream, running on its own thread
class Device {
public:
    virtual ~Device() = default;
};

class Backend {
public:
    virtual ~Backend() = default;
    // Fills `dst` (`bytes` long, whole frames) from the stream; the
    // device plays what comes back (the server pads with silence)
    using Pull = std::function<void(uint8_t* dst, size_t bytes)>;
    virtual std::unique_ptr<Device> open_output(const SampleSpec& ss, Pull pull) = 0;
    // Hands captured audio to the stream
    using Push = std::function<void(const uint8_t* src, size_t bytes)>;
    virtual std::unique_ptr<Device> open_input(const SampleSpec& ss, Push push) = 0;
};

// Pulls and pushes on a clock thread: silence in, audio dropped. The
// fallback when the host has no audio for a stream, and the tests' device.
class ClockBackend : public Backend {
public:
    // Every pulled byte also goes to `tap` (tests), if set. A device pulls
    // `period_ms` of audio at a time.
    explicit ClockBackend(std::function<void(const uint8_t*, size_t)> tap = nullptr, unsigned period_ms = 10)
        : tap_(std::move(tap)), period_ms_(period_ms) {}
    std::unique_ptr<Device> open_output(const SampleSpec& ss, Pull pull) override;
    std::unique_ptr<Device> open_input(const SampleSpec& ss, Push push) override;

private:
    std::function<void(const uint8_t*, size_t)> tap_;
    unsigned period_ms_;
};

class Connection;
struct Shared;

class Server {
public:
    using Log = std::function<void(const std::string&)>;

    Server(Backend* backend, Log log);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Listens on a filesystem socket (an old one at the path is replaced)
    bool listen(const std::string& path, std::string* error);
    // Serves until stop(); false if polling failed
    bool run();
    // From any thread
    void stop();
    // From any thread: device threads call it after moving audio
    void wake();

    Backend* backend() const { return backend_; }
    void log(const std::string& msg) const { if (log_) log_(msg); }

private:
    Backend* backend_;
    Log log_;
    int listen_fd_ = -1;
    int wake_[2] = {-1, -1};
    int stop_[2] = {-1, -1};
    std::string path_;
    std::vector<std::unique_ptr<Connection>> conns_;
};

} // namespace vine::pa
