#pragma once

#include <cstdint>
#include <string>
#include <vector>

// PulseAudio native protocol encoding: tagstructs (each value prefixed
// with a type tag, integers big endian) inside pstream frames (a 20 byte
// descriptor, then the payload). Only what vine_pulse speaks.
namespace vine::pa {

// pa_sample_format_t
enum SampleFormat : uint8_t {
    kU8 = 0, kAlaw, kUlaw, kS16le, kS16be, kFloat32le, kFloat32be, kS32le, kS32be,
    kS24le, kS24be, kS24_32le, kS24_32be, kSampleMax
};

struct SampleSpec {
    uint8_t format = kS16le;
    uint8_t channels = 2;
    uint32_t rate = 48000;
};

// Bytes per sample for a spec's format, 0 if the server doesn't take it
size_t sample_size(uint8_t format);
size_t frame_size(const SampleSpec& ss);
bool spec_valid(const SampleSpec& ss);
uint64_t bytes_to_usec(uint64_t bytes, const SampleSpec& ss);

class TagWriter {
public:
    TagWriter& u32(uint32_t v);
    TagWriter& u8(uint8_t v);
    TagWriter& u64(uint64_t v);
    TagWriter& s64(int64_t v);
    TagWriter& usec(uint64_t v);
    TagWriter& boolean(bool v);
    TagWriter& str(const char* s); // nullptr: the null string
    TagWriter& spec(const SampleSpec& ss);
    TagWriter& channel_map(uint8_t channels, const uint8_t* positions);
    TagWriter& timeval(uint32_t sec, uint32_t usec);
    TagWriter& empty_proplist();
    TagWriter& pcm_format_info(); // encoding PCM, no properties
    const std::vector<uint8_t>& data() const { return d_; }

private:
    void raw32(uint32_t v);
    std::vector<uint8_t> d_;
};

class TagReader {
public:
    TagReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool u32(uint32_t* v);
    bool u8(uint8_t* v);
    bool u64(uint64_t* v);
    bool s64(int64_t* v);
    bool usec(uint64_t* v);
    bool boolean(bool* v);
    bool str(std::string* s, bool* is_null = nullptr);
    bool spec(SampleSpec* ss);
    bool channel_map(uint8_t* channels, uint8_t positions[32]);
    bool cvolume(uint8_t* channels);
    bool timeval(uint32_t* sec, uint32_t* usec);
    bool proplist(); // skipped
    bool format_info(uint8_t* encoding);
    bool eof() const { return p_ == end_; }

private:
    bool tag(uint8_t t);
    bool raw32(uint32_t* v);
    bool raw64(uint64_t* v);
    const uint8_t* p_;
    const uint8_t* end_;
};

// pstream frame descriptor
constexpr size_t kDescriptorSize = 20;
constexpr uint32_t kControlChannel = 0xffffffffu;
constexpr uint32_t kMaxFrame = 16u << 20;

struct Descriptor {
    uint32_t length = 0, channel = kControlChannel;
    int64_t offset = 0;
    uint32_t flags = 0;
};

void put_descriptor(std::vector<uint8_t>* out, const Descriptor& d);
Descriptor get_descriptor(const uint8_t* p);

} // namespace vine::pa
