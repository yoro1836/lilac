#pragma once

#include "pa_server.h"

#include <functional>
#include <string>

namespace vine::pa {

// Streams on the host through AAudio. A stream AAudio can't open (a
// format it lacks, no microphone permission) runs on a clock instead:
// played audio is dropped, captured audio is silence. A disconnected
// stream (output device changed) is reopened.
class AAudioBackend : public Backend {
public:
    explicit AAudioBackend(std::function<void(const std::string&)> log) : log_(std::move(log)) {}
    std::unique_ptr<Device> open_output(const SampleSpec& ss, Pull pull) override;
    std::unique_ptr<Device> open_input(const SampleSpec& ss, Push push) override;

private:
    std::function<void(const std::string&)> log_;
    ClockBackend clock_;
};

} // namespace vine::pa
