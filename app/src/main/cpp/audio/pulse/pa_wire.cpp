#include "pa_wire.h"

#include <cstring>

namespace vine::pa {

namespace {

enum Tag : uint8_t {
    kTagString = 't', kTagStringNull = 'N', kTagU32 = 'L', kTagU8 = 'B', kTagU64 = 'R', kTagS64 = 'r',
    kTagSampleSpec = 'a', kTagArbitrary = 'x', kTagTrue = '1', kTagFalse = '0', kTagTimeval = 'T',
    kTagUsec = 'U', kTagChannelMap = 'm', kTagCvolume = 'v', kTagProplist = 'P', kTagFormatInfo = 'f',
};

constexpr uint8_t kEncodingPcm = 1;
constexpr uint8_t kMaxChannels = 32;

} // namespace

size_t sample_size(uint8_t format) {
    switch (format) {
    case kU8: case kAlaw: case kUlaw: return 1;
    case kS16le: case kS16be: return 2;
    case kS24le: case kS24be: return 3;
    case kFloat32le: case kFloat32be: case kS32le: case kS32be: case kS24_32le: case kS24_32be: return 4;
    default: return 0;
    }
}

size_t frame_size(const SampleSpec& ss) { return sample_size(ss.format) * ss.channels; }

bool spec_valid(const SampleSpec& ss) {
    return sample_size(ss.format) && ss.channels >= 1 && ss.channels <= kMaxChannels && ss.rate >= 1 &&
           ss.rate <= 384000;
}

uint64_t bytes_to_usec(uint64_t bytes, const SampleSpec& ss) {
    const size_t fs = frame_size(ss);
    return fs && ss.rate ? bytes / fs * 1000000ull / ss.rate : 0;
}

// ---- writer -----------------------------------------------------------

void TagWriter::raw32(uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    d_.insert(d_.end(), b, b + 4);
}

TagWriter& TagWriter::u32(uint32_t v) {
    d_.push_back(kTagU32);
    raw32(v);
    return *this;
}

TagWriter& TagWriter::u8(uint8_t v) {
    d_.push_back(kTagU8);
    d_.push_back(v);
    return *this;
}

TagWriter& TagWriter::u64(uint64_t v) {
    d_.push_back(kTagU64);
    raw32((uint32_t)(v >> 32));
    raw32((uint32_t)v);
    return *this;
}

TagWriter& TagWriter::s64(int64_t v) {
    d_.push_back(kTagS64);
    raw32((uint32_t)((uint64_t)v >> 32));
    raw32((uint32_t)v);
    return *this;
}

TagWriter& TagWriter::usec(uint64_t v) {
    d_.push_back(kTagUsec);
    raw32((uint32_t)(v >> 32));
    raw32((uint32_t)v);
    return *this;
}

TagWriter& TagWriter::boolean(bool v) {
    d_.push_back(v ? kTagTrue : kTagFalse);
    return *this;
}

TagWriter& TagWriter::str(const char* s) {
    if (!s) {
        d_.push_back(kTagStringNull);
        return *this;
    }
    d_.push_back(kTagString);
    d_.insert(d_.end(), s, s + strlen(s) + 1);
    return *this;
}

TagWriter& TagWriter::spec(const SampleSpec& ss) {
    d_.push_back(kTagSampleSpec);
    d_.push_back(ss.format);
    d_.push_back(ss.channels);
    raw32(ss.rate);
    return *this;
}

TagWriter& TagWriter::channel_map(uint8_t channels, const uint8_t* positions) {
    d_.push_back(kTagChannelMap);
    d_.push_back(channels);
    d_.insert(d_.end(), positions, positions + channels);
    return *this;
}

TagWriter& TagWriter::timeval(uint32_t sec, uint32_t usec) {
    d_.push_back(kTagTimeval);
    raw32(sec);
    raw32(usec);
    return *this;
}

TagWriter& TagWriter::empty_proplist() {
    d_.push_back(kTagProplist);
    d_.push_back(kTagStringNull);
    return *this;
}

TagWriter& TagWriter::pcm_format_info() {
    d_.push_back(kTagFormatInfo);
    u8(kEncodingPcm);
    return empty_proplist();
}

// ---- reader -----------------------------------------------------------

bool TagReader::tag(uint8_t t) {
    if (p_ >= end_ || *p_ != t) return false;
    ++p_;
    return true;
}

bool TagReader::raw32(uint32_t* v) {
    if (end_ - p_ < 4) return false;
    *v = (uint32_t)p_[0] << 24 | (uint32_t)p_[1] << 16 | (uint32_t)p_[2] << 8 | p_[3];
    p_ += 4;
    return true;
}

bool TagReader::raw64(uint64_t* v) {
    uint32_t hi, lo;
    if (!raw32(&hi) || !raw32(&lo)) return false;
    *v = (uint64_t)hi << 32 | lo;
    return true;
}

bool TagReader::u32(uint32_t* v) { return tag(kTagU32) && raw32(v); }

bool TagReader::u8(uint8_t* v) {
    if (!tag(kTagU8) || p_ >= end_) return false;
    *v = *p_++;
    return true;
}

bool TagReader::u64(uint64_t* v) { return tag(kTagU64) && raw64(v); }

bool TagReader::s64(int64_t* v) {
    uint64_t u;
    if (!tag(kTagS64) || !raw64(&u)) return false;
    *v = (int64_t)u;
    return true;
}

bool TagReader::usec(uint64_t* v) { return tag(kTagUsec) && raw64(v); }

bool TagReader::boolean(bool* v) {
    if (tag(kTagTrue)) *v = true;
    else if (tag(kTagFalse)) *v = false;
    else return false;
    return true;
}

bool TagReader::str(std::string* s, bool* is_null) {
    if (tag(kTagStringNull)) {
        s->clear();
        if (is_null) *is_null = true;
        return true;
    }
    if (!tag(kTagString)) return false;
    const void* nul = memchr(p_, 0, (size_t)(end_ - p_));
    if (!nul) return false;
    s->assign(reinterpret_cast<const char*>(p_), (size_t)(static_cast<const uint8_t*>(nul) - p_));
    p_ = static_cast<const uint8_t*>(nul) + 1;
    if (is_null) *is_null = false;
    return true;
}

bool TagReader::spec(SampleSpec* ss) {
    uint32_t rate;
    if (!tag(kTagSampleSpec) || end_ - p_ < 2) return false;
    ss->format = p_[0];
    ss->channels = p_[1];
    p_ += 2;
    if (!raw32(&rate)) return false;
    ss->rate = rate;
    return true;
}

bool TagReader::channel_map(uint8_t* channels, uint8_t positions[32]) {
    if (!tag(kTagChannelMap) || p_ >= end_) return false;
    const uint8_t n = *p_++;
    if (n > kMaxChannels || end_ - p_ < n) return false;
    memcpy(positions, p_, n);
    p_ += n;
    *channels = n;
    return true;
}

bool TagReader::cvolume(uint8_t* channels) {
    if (!tag(kTagCvolume) || p_ >= end_) return false;
    const uint8_t n = *p_++;
    if (n > kMaxChannels || end_ - p_ < 4 * n) return false;
    p_ += 4 * n;
    *channels = n;
    return true;
}

bool TagReader::timeval(uint32_t* sec, uint32_t* usec) { return tag(kTagTimeval) && raw32(sec) && raw32(usec); }

bool TagReader::proplist() {
    if (!tag(kTagProplist)) return false;
    for (;;) {
        std::string key;
        bool null = false;
        if (!str(&key, &null)) return false;
        if (null) return true;
        uint32_t len, len2;
        if (!u32(&len) || !tag(kTagArbitrary) || !raw32(&len2) || len != len2 || (size_t)(end_ - p_) < len) return false;
        p_ += len;
    }
}

bool TagReader::format_info(uint8_t* encoding) { return tag(kTagFormatInfo) && u8(encoding) && proplist(); }

// ---- frames -----------------------------------------------------------

void put_descriptor(std::vector<uint8_t>* out, const Descriptor& d) {
    const uint32_t w[5] = {d.length, d.channel, (uint32_t)((uint64_t)d.offset >> 32), (uint32_t)d.offset, d.flags};
    for (uint32_t v : w) {
        const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        out->insert(out->end(), b, b + 4);
    }
}

Descriptor get_descriptor(const uint8_t* p) {
    uint32_t w[5];
    for (int i = 0; i < 5; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    Descriptor d;
    d.length = w[0];
    d.channel = w[1];
    d.offset = (int64_t)((uint64_t)w[2] << 32 | w[3]);
    d.flags = w[4];
    return d;
}

} // namespace vine::pa
