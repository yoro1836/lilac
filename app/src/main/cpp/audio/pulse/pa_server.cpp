#include "pa_server.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace vine::pa {

namespace {

// native-common.h
enum Command : uint32_t {
    kError = 0, kTimeout, kReply,
    kCreatePlaybackStream, kDeletePlaybackStream, kCreateRecordStream, kDeleteRecordStream,
    kExit, kAuth, kSetClientName, kLookupSink, kLookupSource, kDrainPlaybackStream, kStat,
    kGetPlaybackLatency, kCreateUploadStream, kDeleteUploadStream, kFinishUploadStream, kPlaySample,
    kRemoveSample, kGetServerInfo, kGetSinkInfo, kGetSinkInfoList, kGetSourceInfo, kGetSourceInfoList,
    kGetModuleInfo, kGetModuleInfoList, kGetClientInfo, kGetClientInfoList, kGetSinkInputInfo,
    kGetSinkInputInfoList, kGetSourceOutputInfo, kGetSourceOutputInfoList, kGetSampleInfo,
    kGetSampleInfoList, kSubscribe, kSetSinkVolume, kSetSinkInputVolume, kSetSourceVolume, kSetSinkMute,
    kSetSourceMute, kCorkPlaybackStream, kFlushPlaybackStream, kTriggerPlaybackStream, kSetDefaultSink,
    kSetDefaultSource, kSetPlaybackStreamName, kSetRecordStreamName, kKillClient, kKillSinkInput,
    kKillSourceOutput, kLoadModule, kUnloadModule, kAddAutoload, kRemoveAutoload, kGetAutoloadInfo,
    kGetAutoloadInfoList, kGetRecordLatency, kCorkRecordStream, kFlushRecordStream, kPrebufPlaybackStream,
    kRequest, kOverflow, kUnderflow, kPlaybackStreamKilled, kRecordStreamKilled, kSubscribeEvent,
    kMoveSinkInput, kMoveSourceOutput, kSetSinkInputMute, kSuspendSink, kSuspendSource,
    kSetPlaybackStreamBufferAttr, kSetRecordStreamBufferAttr, kUpdatePlaybackStreamSampleRate,
    kUpdateRecordStreamSampleRate, kPlaybackStreamSuspended, kRecordStreamSuspended,
    kPlaybackStreamMoved, kRecordStreamMoved, kUpdateRecordStreamProplist, kUpdatePlaybackStreamProplist,
    kUpdateClientProplist, kRemoveRecordStreamProplist, kRemovePlaybackStreamProplist,
    kRemoveClientProplist, kStarted, kExtension, kGetCardInfo, kGetCardInfoList, kSetCardProfile,
    kClientEvent, kPlaybackStreamEvent, kRecordStreamEvent, kPlaybackBufferAttrChanged,
    kRecordBufferAttrChanged, kSetSinkPort, kSetSourcePort, kSetSourceOutputVolume,
    kSetSourceOutputMute, kSetPortLatencyOffset, kEnableSrbchannel, kDisableSrbchannel,
    kRegisterMemfdShmid,
};

// def.h
enum Error : uint32_t {
    kErrAccess = 1, kErrCommand = 2, kErrInvalid = 3, kErrNoEntity = 5, kErrProtocol = 7, kErrNotSupported = 19,
};

constexpr uint32_t kProtocolVersion = 32;
constexpr uint32_t kVersionMask = 0x0000ffffu;
constexpr uint32_t kInvalidIndex = 0xffffffffu;
constexpr uint32_t kMaxQueue = 4u << 20;
constexpr uint64_t kTlengthUsec = 2000000, kProcessUsec = 20000, kFragsizeUsec = 20000;
// What the host's own buffering adds, roughly; reported as the device latency
constexpr uint64_t kDeviceLatencyUsec = 20000;
constexpr const char* kSinkName = "vine_output";
constexpr const char* kSourceName = "vine_input";

uint32_t usec_to_bytes(uint64_t usec, const SampleSpec& ss) {
    const uint64_t frames = (usec * ss.rate + 999999) / 1000000;
    return (uint32_t)std::min<uint64_t>(frames * frame_size(ss), kMaxQueue);
}

uint32_t round_down(uint32_t v, size_t fs) { return fs ? v - v % (uint32_t)fs : v; }

void now_timeval(uint32_t* sec, uint32_t* usec) {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    *sec = (uint32_t)tv.tv_sec;
    *usec = (uint32_t)tv.tv_usec;
}

// Front-trimmed byte queue
struct ByteQueue {
    std::vector<uint8_t> d;
    size_t head = 0;
    size_t size() const { return d.size() - head; }
    void push(const uint8_t* p, size_t n) {
        if (head > 65536 && head * 2 > d.size()) {
            d.erase(d.begin(), d.begin() + (ptrdiff_t)head);
            head = 0;
        }
        d.insert(d.end(), p, p + n);
    }
    void pop(uint8_t* out, size_t n) {
        if (out) memcpy(out, d.data() + head, n);
        head += n;
        if (head == d.size()) {
            d.clear();
            head = 0;
        }
    }
    void clear() {
        d.clear();
        head = 0;
    }
};

} // namespace

// ---- streams ----------------------------------------------------------

// State shared with a device thread lives under `mu`
struct Playback {
    uint32_t index = 0;
    SampleSpec ss;
    uint8_t channels = 0;
    uint8_t map[32] = {};
    uint32_t maxlength = 0, tlength = 0, prebuf = 0, minreq = 0;
    int64_t requested = 0; // loop thread only: asked for, not yet received
    std::vector<uint32_t> drain_tags;

    std::mutex mu;
    ByteQueue queue;
    int64_t write_index = 0, read_index = 0;
    uint64_t underrun_for = 0, playing_for = 0;
    bool corked = false, prebuffering = true;
    bool started_event = false, underflow_event = false;

    std::unique_ptr<Device> device; // last: stopped before the rest goes

    void pull(uint8_t* dst, size_t bytes) {
        std::lock_guard<std::mutex> lock(mu);
        size_t got = 0;
        if (!corked) {
            if (prebuffering && queue.size() >= std::max<size_t>(prebuf, 1)) {
                prebuffering = false;
                started_event = true;
            }
            if (!prebuffering) {
                got = std::min(bytes, queue.size());
                got -= got % std::max<size_t>(frame_size(ss), 1);
                queue.pop(dst, got);
                read_index += (int64_t)got;
                playing_for += got;
                if (got < bytes) {
                    underrun_for += bytes - got;
                    underflow_event = true;
                    if (prebuf) prebuffering = true;
                } else {
                    underrun_for = 0;
                }
            }
        }
        memset(dst + got, ss.format == kU8 ? 0x80 : 0, bytes - got);
    }
};

struct Record {
    uint32_t index = 0;
    SampleSpec ss;
    uint8_t channels = 0;
    uint8_t map[32] = {};
    uint32_t maxlength = 0, fragsize = 0;

    std::mutex mu;
    ByteQueue queue;
    int64_t write_index = 0, read_index = 0;
    bool corked = false;

    std::unique_ptr<Device> device;

    void push(const uint8_t* src, size_t bytes) {
        std::lock_guard<std::mutex> lock(mu);
        if (corked) return;
        queue.push(src, bytes);
        write_index += (int64_t)bytes;
        if (queue.size() > maxlength) {
            const size_t drop = queue.size() - maxlength;
            queue.pop(nullptr, drop);
            read_index += (int64_t)drop;
        }
    }
};

// ---- connection -------------------------------------------------------

class Connection {
public:
    Connection(Server* server, int fd) : server_(server), fd_(fd) {}
    ~Connection() {
        playbacks_.clear();
        records_.clear();
        if (fd_ >= 0) close(fd_);
    }
    int fd() const { return fd_; }
    bool wants_write() const { return !out_.empty(); }
    bool on_readable();
    bool flush();
    // Moves audio and events between the devices and the client
    void service();

private:
    void packet(const uint8_t* p, size_t n);
    void memblock(const Descriptor& d, const uint8_t* p);
    void send_packet(const TagWriter& w);
    TagWriter reply(uint32_t tag) {
        TagWriter w;
        w.u32(kReply).u32(tag);
        return w;
    }
    void ack(uint32_t tag) { send_packet(reply(tag)); }
    void error(uint32_t tag, uint32_t code) {
        TagWriter w;
        w.u32(kError).u32(tag).u32(code);
        send_packet(w);
    }
    void create_playback(uint32_t tag, TagReader& r);
    void create_record(uint32_t tag, TagReader& r);
    void playback_latency(uint32_t tag, TagReader& r);
    void record_latency(uint32_t tag, TagReader& r);
    Playback* playback(uint32_t index) {
        auto it = playbacks_.find(index);
        return it == playbacks_.end() ? nullptr : it->second.get();
    }
    Record* record(uint32_t index) {
        auto it = records_.find(index);
        return it == records_.end() ? nullptr : it->second.get();
    }

    Server* server_;
    int fd_;
    std::vector<uint8_t> in_, out_;
    uint32_t version_ = 0;
    bool authorized_ = false, broken_ = false;
    uint32_t next_index_ = 0;
    std::map<uint32_t, std::unique_ptr<Playback>> playbacks_;
    std::map<uint32_t, std::unique_ptr<Record>> records_;
};

void Connection::send_packet(const TagWriter& w) {
    Descriptor d;
    d.length = (uint32_t)w.data().size();
    put_descriptor(&out_, d);
    out_.insert(out_.end(), w.data().begin(), w.data().end());
}

bool Connection::on_readable() {
    uint8_t buf[65536];
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 8) + CMSG_SPACE(64)];
    iovec iov{buf, sizeof buf};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;
    const ssize_t n = recvmsg(fd_, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0) return errno == EAGAIN || errno == EINTR;
    if (n == 0) return false;
    // Credentials ride along; fds (memfd pools) are never negotiated
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
        for (size_t i = 0; i < count; ++i) close(fds[i]);
    }
    in_.insert(in_.end(), buf, buf + n);
    size_t pos = 0;
    while (!broken_ && in_.size() - pos >= kDescriptorSize) {
        const Descriptor d = get_descriptor(&in_[pos]);
        if (d.length > kMaxFrame) {
            broken_ = true;
            break;
        }
        if (in_.size() - pos - kDescriptorSize < d.length) break;
        const uint8_t* payload = &in_[pos + kDescriptorSize];
        if (d.channel == kControlChannel) packet(payload, d.length);
        else memblock(d, payload);
        pos += kDescriptorSize + d.length;
    }
    in_.erase(in_.begin(), in_.begin() + (ptrdiff_t)pos);
    service();
    return flush() && !broken_;
}

bool Connection::flush() {
    size_t sent = 0;
    while (sent < out_.size()) {
        const ssize_t n = send(fd_, out_.data() + sent, out_.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            return false;
        }
        sent += (size_t)n;
    }
    out_.erase(out_.begin(), out_.begin() + (ptrdiff_t)sent);
    return true;
}

void Connection::memblock(const Descriptor& d, const uint8_t* p) {
    Playback* s = playback(d.channel);
    if (!s || !d.length) return;
    {
        std::lock_guard<std::mutex> lock(s->mu);
        // Relative seeks forward leave a gap of silence; backwards ones
        // (rewriting queued audio) are not supported and append.
        if ((d.flags & 0xff) == 0 && d.offset > 0) {
            const std::vector<uint8_t> gap((size_t)std::min<int64_t>(d.offset, kMaxQueue), 0);
            s->queue.push(gap.data(), gap.size());
            s->write_index += (int64_t)gap.size();
        }
        s->queue.push(p, d.length);
        s->write_index += d.length;
        if (s->queue.size() > s->maxlength) {
            const size_t drop = s->queue.size() - s->maxlength;
            s->queue.pop(nullptr, drop);
            s->read_index += (int64_t)drop;
        }
    }
    s->requested = std::max<int64_t>(0, s->requested - d.length);
}

void Connection::service() {
    for (auto& [index, s] : playbacks_) {
        bool started, underflow;
        int64_t queued, read_index;
        {
            std::lock_guard<std::mutex> lock(s->mu);
            started = s->started_event;
            underflow = s->underflow_event;
            s->started_event = s->underflow_event = false;
            queued = (int64_t)s->queue.size();
            read_index = s->read_index;
        }
        if (started && version_ >= 13) {
            TagWriter w;
            w.u32(kStarted).u32(kInvalidIndex).u32(index);
            send_packet(w);
        }
        if (underflow && queued == 0) {
            TagWriter w;
            w.u32(kUnderflow).u32(kInvalidIndex).u32(index);
            if (version_ >= 23) w.s64(read_index);
            send_packet(w);
        }
        const int64_t missing = (int64_t)s->tlength - queued - s->requested;
        if (missing >= (int64_t)s->minreq) {
            TagWriter w;
            w.u32(kRequest).u32(kInvalidIndex).u32(index).u32((uint32_t)missing);
            send_packet(w);
            s->requested += missing;
        }
        if (!s->drain_tags.empty() && queued == 0) {
            for (uint32_t tag : s->drain_tags) ack(tag);
            s->drain_tags.clear();
        }
    }
    for (auto& [index, s] : records_) {
        std::vector<uint8_t> chunk;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(s->mu);
                if (s->queue.size() < s->fragsize) break;
                chunk.resize(s->fragsize);
                s->queue.pop(chunk.data(), chunk.size());
                s->read_index += (int64_t)chunk.size();
            }
            Descriptor d;
            d.length = (uint32_t)chunk.size();
            d.channel = index;
            put_descriptor(&out_, d);
            out_.insert(out_.end(), chunk.begin(), chunk.end());
        }
    }
}

void Connection::create_playback(uint32_t tag, TagReader& r) {
    auto s = std::make_unique<Playback>();
    uint32_t sink_index, syncid;
    std::string sink_name;
    bool corked = false;
    uint8_t vol_channels;
    if (!r.spec(&s->ss) || !r.channel_map(&s->channels, s->map) || !r.u32(&sink_index) || !r.str(&sink_name) ||
        !r.u32(&s->maxlength) || !r.boolean(&corked) || !r.u32(&s->tlength) || !r.u32(&s->prebuf) ||
        !r.u32(&s->minreq) || !r.u32(&syncid) || !r.cvolume(&vol_channels)) {
        broken_ = true;
        return;
    }
    // The rest (flags, proplist, formats) changes nothing here
    if (!spec_valid(s->ss) || s->channels != s->ss.channels) {
        error(tag, kErrInvalid);
        return;
    }
    const size_t fs = frame_size(s->ss);
    if (s->maxlength == kInvalidIndex || s->maxlength > kMaxQueue) s->maxlength = kMaxQueue;
    s->maxlength = std::max<uint32_t>(round_down(s->maxlength, fs), (uint32_t)fs);
    if (s->tlength == kInvalidIndex) s->tlength = usec_to_bytes(kTlengthUsec, s->ss);
    s->tlength = std::min(std::max<uint32_t>(round_down(s->tlength, fs), (uint32_t)fs), s->maxlength);
    if (s->minreq == kInvalidIndex)
        s->minreq = std::min(usec_to_bytes(kProcessUsec, s->ss), round_down(s->tlength / 4, fs));
    s->minreq = std::max<uint32_t>(round_down(s->minreq, fs), (uint32_t)fs);
    if (s->tlength < s->minreq + fs) s->tlength = s->minreq + (uint32_t)fs;
    const uint32_t max_prebuf = s->tlength + (uint32_t)fs - s->minreq;
    if (s->prebuf == kInvalidIndex || s->prebuf > max_prebuf) s->prebuf = max_prebuf;
    s->prebuf = round_down(s->prebuf, fs);
    s->corked = corked;
    s->prebuffering = s->prebuf > 0;
    s->index = next_index_++;

    Playback* raw = s.get();
    Server* server = server_;
    s->device = server_->backend()->open_output(s->ss, [raw, server](uint8_t* dst, size_t bytes) {
        raw->pull(dst, bytes);
        server->wake();
    });
    if (!s->device) {
        error(tag, kErrNoEntity);
        return;
    }
    s->requested = s->tlength;
    TagWriter w = reply(tag);
    w.u32(s->index).u32(s->index).u32(s->tlength);
    w.u32(s->maxlength).u32(s->tlength).u32(s->prebuf).u32(s->minreq);
    w.spec(s->ss).channel_map(s->channels, s->map).u32(0).str(kSinkName).boolean(false);
    if (version_ >= 13) w.usec(kDeviceLatencyUsec);
    if (version_ >= 21) w.pcm_format_info();
    send_packet(w);
    server_->log("playback stream " + std::to_string(s->index) + ": " + std::to_string(s->ss.rate) + " Hz, " +
                 std::to_string(s->ss.channels) + " ch, format " + std::to_string(s->ss.format));
    playbacks_[s->index] = std::move(s);
}

void Connection::create_record(uint32_t tag, TagReader& r) {
    auto s = std::make_unique<Record>();
    uint32_t source_index;
    std::string source_name;
    bool corked = false;
    if (!r.spec(&s->ss) || !r.channel_map(&s->channels, s->map) || !r.u32(&source_index) ||
        !r.str(&source_name) || !r.u32(&s->maxlength) || !r.boolean(&corked) || !r.u32(&s->fragsize)) {
        broken_ = true;
        return;
    }
    if (!spec_valid(s->ss) || s->channels != s->ss.channels) {
        error(tag, kErrInvalid);
        return;
    }
    const size_t fs = frame_size(s->ss);
    if (s->maxlength == kInvalidIndex || s->maxlength > kMaxQueue) s->maxlength = kMaxQueue;
    s->maxlength = std::max<uint32_t>(round_down(s->maxlength, fs), (uint32_t)fs);
    if (s->fragsize == kInvalidIndex) s->fragsize = usec_to_bytes(kFragsizeUsec, s->ss);
    s->fragsize = std::min(std::max<uint32_t>(round_down(s->fragsize, fs), (uint32_t)fs), s->maxlength);
    s->corked = corked;
    s->index = next_index_++;

    Record* raw = s.get();
    Server* server = server_;
    s->device = server_->backend()->open_input(s->ss, [raw, server](const uint8_t* src, size_t bytes) {
        raw->push(src, bytes);
        server->wake();
    });
    if (!s->device) {
        error(tag, kErrNoEntity);
        return;
    }
    TagWriter w = reply(tag);
    w.u32(s->index).u32(s->index);
    w.u32(s->maxlength).u32(s->fragsize);
    w.spec(s->ss).channel_map(s->channels, s->map).u32(0).str(kSourceName).boolean(false);
    if (version_ >= 13) w.usec(kDeviceLatencyUsec);
    if (version_ >= 22) w.pcm_format_info();
    send_packet(w);
    server_->log("record stream " + std::to_string(s->index) + ": " + std::to_string(s->ss.rate) + " Hz, " +
                 std::to_string(s->ss.channels) + " ch, format " + std::to_string(s->ss.format));
    records_[s->index] = std::move(s);
}

void Connection::playback_latency(uint32_t tag, TagReader& r) {
    uint32_t index, sec, usec;
    if (!r.u32(&index) || !r.timeval(&sec, &usec)) {
        broken_ = true;
        return;
    }
    Playback* s = playback(index);
    if (!s) {
        error(tag, kErrNoEntity);
        return;
    }
    uint32_t now_sec, now_usec;
    now_timeval(&now_sec, &now_usec);
    std::lock_guard<std::mutex> lock(s->mu);
    TagWriter w = reply(tag);
    w.usec(kDeviceLatencyUsec).usec(0).boolean(!s->corked && !s->prebuffering);
    w.timeval(sec, usec).timeval(now_sec, now_usec).s64(s->write_index).s64(s->read_index);
    if (version_ >= 13) w.u64(s->underrun_for).u64(s->playing_for);
    send_packet(w);
}

void Connection::record_latency(uint32_t tag, TagReader& r) {
    uint32_t index, sec, usec;
    if (!r.u32(&index) || !r.timeval(&sec, &usec)) {
        broken_ = true;
        return;
    }
    Record* s = record(index);
    if (!s) {
        error(tag, kErrNoEntity);
        return;
    }
    uint32_t now_sec, now_usec;
    now_timeval(&now_sec, &now_usec);
    std::lock_guard<std::mutex> lock(s->mu);
    TagWriter w = reply(tag);
    w.usec(0).usec(kDeviceLatencyUsec).boolean(!s->corked);
    w.timeval(sec, usec).timeval(now_sec, now_usec).s64(s->write_index).s64(s->read_index);
    send_packet(w);
}

void Connection::packet(const uint8_t* p, size_t n) {
    TagReader r(p, n);
    uint32_t command, tag;
    if (!r.u32(&command) || !r.u32(&tag)) {
        broken_ = true;
        return;
    }
    static const bool trace = getenv("VINE_PULSE_TRACE") != nullptr;
    if (trace) server_->log("command " + std::to_string(command));
    if (command == kAuth) {
        uint32_t version;
        if (!r.u32(&version)) {
            broken_ = true;
            return;
        }
        version_ = std::min(version & kVersionMask, kProtocolVersion);
        if (version_ < 13) {
            error(tag, kErrNotSupported);
            broken_ = true;
            return;
        }
        authorized_ = true; // anyone who can reach the socket
        TagWriter w = reply(tag);
        w.u32(kProtocolVersion); // no SHM, no memfd flags
        send_packet(w);
        return;
    }
    if (!authorized_) {
        error(tag, kErrAccess);
        return;
    }
    uint32_t index = kInvalidIndex;
    switch (command) {
    case kSetClientName: {
        TagWriter w = reply(tag);
        w.u32(0);
        send_packet(w);
        return;
    }
    case kCreatePlaybackStream: create_playback(tag, r); return;
    case kCreateRecordStream: create_record(tag, r); return;
    case kDeletePlaybackStream:
    case kDeleteRecordStream:
        if (!r.u32(&index)) break;
        if (command == kDeletePlaybackStream ? playbacks_.erase(index) : records_.erase(index)) ack(tag);
        else error(tag, kErrNoEntity);
        return;
    case kGetPlaybackLatency: playback_latency(tag, r); return;
    case kGetRecordLatency: record_latency(tag, r); return;
    case kCorkPlaybackStream:
    case kCorkRecordStream: {
        bool b;
        if (!r.u32(&index) || !r.boolean(&b)) break;
        if (Playback* s = command == kCorkPlaybackStream ? playback(index) : nullptr) {
            std::lock_guard<std::mutex> lock(s->mu);
            s->corked = b;
        } else if (Record* rs = command == kCorkRecordStream ? record(index) : nullptr) {
            std::lock_guard<std::mutex> lock(rs->mu);
            rs->corked = b;
        } else {
            error(tag, kErrNoEntity);
            return;
        }
        ack(tag);
        return;
    }
    case kFlushPlaybackStream:
    case kFlushRecordStream:
        if (!r.u32(&index)) break;
        if (Playback* s = command == kFlushPlaybackStream ? playback(index) : nullptr) {
            std::lock_guard<std::mutex> lock(s->mu);
            s->write_index -= (int64_t)s->queue.size();
            s->queue.clear();
            s->prebuffering = s->prebuf > 0;
            s->requested = 0;
        } else if (Record* rs = command == kFlushRecordStream ? record(index) : nullptr) {
            std::lock_guard<std::mutex> lock(rs->mu);
            rs->read_index += (int64_t)rs->queue.size();
            rs->queue.clear();
        } else {
            error(tag, kErrNoEntity);
            return;
        }
        ack(tag);
        return;
    case kTriggerPlaybackStream:
    case kPrebufPlaybackStream:
    case kDrainPlaybackStream: {
        if (!r.u32(&index)) break;
        Playback* s = playback(index);
        if (!s) {
            error(tag, kErrNoEntity);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s->mu);
            // A drain plays out what is buffered, as a trigger does
            s->prebuffering = command == kPrebufPlaybackStream && s->prebuf > 0;
        }
        if (command == kDrainPlaybackStream) s->drain_tags.push_back(tag);
        else ack(tag);
        return;
    }
    case kSetPlaybackStreamBufferAttr:
    case kSetRecordStreamBufferAttr: {
        if (!r.u32(&index)) break;
        if (Playback* s = command == kSetPlaybackStreamBufferAttr ? playback(index) : nullptr) {
            uint32_t maxlength, tlength, prebuf, minreq;
            if (!r.u32(&maxlength) || !r.u32(&tlength) || !r.u32(&prebuf) || !r.u32(&minreq)) break;
            const size_t fs = frame_size(s->ss);
            if (tlength != kInvalidIndex)
                s->tlength = std::min(std::max<uint32_t>(round_down(tlength, fs), s->minreq + (uint32_t)fs), s->maxlength);
            TagWriter w = reply(tag);
            w.u32(s->maxlength).u32(s->tlength).u32(s->prebuf).u32(s->minreq).usec(kDeviceLatencyUsec);
            send_packet(w);
        } else if (Record* rs = command == kSetRecordStreamBufferAttr ? record(index) : nullptr) {
            uint32_t maxlength, fragsize;
            if (!r.u32(&maxlength) || !r.u32(&fragsize)) break;
            const size_t fs = frame_size(rs->ss);
            if (fragsize != kInvalidIndex)
                rs->fragsize = std::min(std::max<uint32_t>(round_down(fragsize, fs), (uint32_t)fs), rs->maxlength);
            TagWriter w = reply(tag);
            w.u32(rs->maxlength).u32(rs->fragsize).usec(kDeviceLatencyUsec);
            send_packet(w);
        } else {
            error(tag, kErrNoEntity);
        }
        return;
    }
    case kGetServerInfo: {
        const SampleSpec ss;
        const uint8_t map[2] = {1, 2}; // front left, front right
        TagWriter w = reply(tag);
        w.str("pulseaudio").str("12.0").str("vine").str("vine").spec(ss).str(kSinkName).str(kSourceName).u32(0);
        if (version_ >= 15) w.channel_map(2, map);
        send_packet(w);
        return;
    }
    case kLookupSink:
    case kLookupSource: {
        TagWriter w = reply(tag);
        w.u32(0);
        send_packet(w);
        return;
    }
    case kStat: {
        TagWriter w = reply(tag);
        w.u32(0).u32(0).u32(0).u32(0).u32(0);
        send_packet(w);
        return;
    }
    // Accepted and ignored: names, properties, volumes, events
    case kSubscribe:
    case kSetPlaybackStreamName:
    case kSetRecordStreamName:
    case kUpdateClientProplist:
    case kUpdatePlaybackStreamProplist:
    case kUpdateRecordStreamProplist:
    case kRemoveClientProplist:
    case kRemovePlaybackStreamProplist:
    case kRemoveRecordStreamProplist:
    case kSetSinkInputVolume:
    case kSetSinkInputMute:
    case kSetSourceOutputVolume:
    case kSetSourceOutputMute:
    case kUpdatePlaybackStreamSampleRate:
    case kUpdateRecordStreamSampleRate:
        ack(tag);
        return;
    default:
        server_->log("unsupported command " + std::to_string(command));
        error(tag, kErrNotSupported);
        return;
    }
    broken_ = true; // a known command with a malformed body
}

// ---- server -----------------------------------------------------------

Server::Server(Backend* backend, Log log) : backend_(backend), log_(std::move(log)) {
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) wake_[0] = wake_[1] = -1;
    if (pipe2(stop_, O_CLOEXEC | O_NONBLOCK) != 0) stop_[0] = stop_[1] = -1;
}

Server::~Server() {
    conns_.clear();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(path_.c_str());
    }
    for (int f : wake_)
        if (f >= 0) close(f);
    for (int f : stop_)
        if (f >= 0) close(f);
}

bool Server::listen(const std::string& path, std::string* error) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) {
        if (error) *error = "socket path too long: " + path;
        return false;
    }
    memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + strerror(errno);
        return false;
    }
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(fd, 8) != 0) {
        if (error) *error = "listen on " + path + ": " + strerror(errno);
        close(fd);
        return false;
    }
    listen_fd_ = fd;
    path_ = path;
    return true;
}

void Server::wake() {
    if (wake_[1] >= 0) (void)!write(wake_[1], "w", 1);
}

void Server::stop() {
    if (stop_[1] >= 0) (void)!write(stop_[1], "s", 1);
}

bool Server::run() {
    for (;;) {
        std::vector<pollfd> pfds;
        pfds.push_back({stop_[0], POLLIN, 0});
        pfds.push_back({wake_[0], POLLIN, 0});
        pfds.push_back({listen_fd_, POLLIN, 0});
        for (auto& c : conns_) pfds.push_back({c->fd(), (short)(POLLIN | (c->wants_write() ? POLLOUT : 0)), 0});
        if (poll(pfds.data(), pfds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pfds[0].revents) return true;
        if (pfds[1].revents) {
            char buf[256];
            while (read(wake_[0], buf, sizeof buf) > 0) {}
        }
        if (pfds[2].revents & POLLIN) {
            const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                conns_.push_back(std::make_unique<Connection>(this, fd));
                log("client connected");
            }
        }
        for (size_t i = 3, k = 0; i < pfds.size(); ++i) {
            Connection* c = conns_[k].get();
            bool keep = true;
            if (pfds[i].revents & POLLIN) keep = c->on_readable();
            else if (pfds[i].revents & (POLLERR | POLLHUP)) keep = false;
            if (keep) {
                c->service();
                keep = c->flush();
            }
            if (keep) {
                ++k;
            } else {
                conns_.erase(conns_.begin() + (ptrdiff_t)k);
                log("client gone");
            }
        }
    }
}

// ---- clock backend ----------------------------------------------------

namespace {

class ClockDevice : public Device {
public:
    ClockDevice(const SampleSpec& ss, std::function<void(uint8_t*, size_t)> tick)
        : ss_(ss), tick_(std::move(tick)), thread_([this] { loop(); }) {}
    ~ClockDevice() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

private:
    void loop() {
        // 10 ms periods against a steady clock
        const size_t frames = std::max<size_t>(ss_.rate / 100, 1);
        std::vector<uint8_t> buf(frames * frame_size(ss_));
        auto next = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mu_);
        while (!stop_) {
            next += std::chrono::microseconds(frames * 1000000ull / ss_.rate);
            if (cv_.wait_until(lock, next, [this] { return stop_; })) break;
            lock.unlock();
            tick_(buf.data(), buf.size());
            lock.lock();
        }
    }
    SampleSpec ss_;
    std::function<void(uint8_t*, size_t)> tick_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_; // last: started once the rest is set up
};

} // namespace

std::unique_ptr<Device> ClockBackend::open_output(const SampleSpec& ss, Pull pull) {
    auto tap = tap_;
    return std::make_unique<ClockDevice>(ss, [pull, tap](uint8_t* buf, size_t n) {
        pull(buf, n);
        if (tap) tap(buf, n);
    });
}

std::unique_ptr<Device> ClockBackend::open_input(const SampleSpec& ss, Push push) {
    return std::make_unique<ClockDevice>(ss, [push, ss](uint8_t* buf, size_t n) {
        memset(buf, ss.format == kU8 ? 0x80 : 0, n);
        push(buf, n);
    });
}

} // namespace vine::pa
