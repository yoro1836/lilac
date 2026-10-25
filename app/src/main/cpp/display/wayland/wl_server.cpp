#include "wl_server.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vine::wl {

namespace {

// Global names in the registry, and what each one binds
enum Global : uint32_t { G_COMPOSITOR = 1, G_SHM, G_WM_BASE, G_OUTPUT, G_SEAT, G_COUNT };

struct GlobalInfo {
    const char* interface;
    uint32_t version;
};
const GlobalInfo kGlobals[G_COUNT] = {
    {nullptr, 0},
    {"wl_compositor", 4},
    {"wl_shm", 1},
    {"xdg_wm_base", 1},
    {"wl_output", 3},
    {"wl_seat", 5},
};

constexpr uint32_t kDisplayId = 1;
constexpr size_t kMaxMessage = 4096;
constexpr size_t kMaxFds = 28;

uint32_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

} // namespace

class Client;

// One request being decoded
struct Msg {
    Client* client;
    uint32_t id;
    uint16_t opcode;
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint32_t u32() {
        if (end - p < 4) { ok = false; return 0; }
        uint32_t v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    std::string str() {
        const uint32_t len = u32();
        if (!ok || len == 0) return {};
        const uint32_t padded = (len + 3) & ~3u;
        if ((size_t)(end - p) < padded || p[len - 1] != 0) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), len - 1);
        p += padded;
        return s;
    }
    int fd();
};

struct Object {
    Client* client = nullptr;
    uint32_t id = 0;
    uint32_t version = 1;
    virtual ~Object() = default;
    virtual void request(Msg& m) = 0;
};

// Shared mapping of a wl_shm_pool, outlives the pool while buffers use it
struct PoolMem {
    int fd = -1;
    uint8_t* data = nullptr;
    size_t size = 0;
    ~PoolMem() {
        if (data) munmap(data, size);
        if (fd >= 0) close(fd);
    }
    bool map(size_t want) {
        void* p = mmap(nullptr, want, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) return false;
        if (data) munmap(data, size);
        data = static_cast<uint8_t*>(p);
        size = want;
        return true;
    }
};

class Client {
public:
    Client(Server* server, int fd);
    ~Client() {
        objects_.clear();
        for (int f : in_fds_) close(f);
        for (int f : out_fds_) close(f);
        if (fd_ >= 0) close(fd_);
    }

    int fd() const { return fd_; }
    bool wants_write() const { return !out_.empty(); }
    Server* server() const { return server_; }

    // Builds one event; the size is patched in when it goes out of scope
    class Event {
    public:
        Event(Client* c, uint32_t id, uint16_t opcode) : c_(c), start_(c->out_.size()) {
            put(id);
            put(opcode);
        }
        ~Event() {
            const uint32_t size = (uint32_t)(c_->out_.size() - start_);
            uint32_t word;
            memcpy(&word, &c_->out_[start_ + 4], 4);
            word |= size << 16;
            memcpy(&c_->out_[start_ + 4], &word, 4);
        }
        Event& u32(uint32_t v) { put(v); return *this; }
        Event& i32(int32_t v) { put((uint32_t)v); return *this; }
        Event& str(const std::string& s) {
            put((uint32_t)s.size() + 1);
            bytes(s.c_str(), s.size() + 1);
            return *this;
        }
        Event& arr(const void* data, size_t n) {
            put((uint32_t)n);
            bytes(data, n);
            return *this;
        }
        Event& fd(int f) { c_->out_fds_.push_back(f); return *this; }

    private:
        void put(uint32_t v) { bytes(&v, 4); }
        void bytes(const void* data, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(data);
            c_->out_.insert(c_->out_.end(), b, b + n);
            while (c_->out_.size() % 4) c_->out_.push_back(0);
        }
        Client* c_;
        size_t start_;
    };

    Event event(uint32_t id, uint16_t opcode) { return Event(this, id, opcode); }

    template <typename T>
    T* add(uint32_t id, uint32_t version) {
        if (id == 0 || objects_.count(id)) {
            error(kDisplayId, 0, "invalid new id " + std::to_string(id));
            return nullptr;
        }
        auto obj = std::make_unique<T>();
        obj->client = this;
        obj->id = id;
        obj->version = version;
        T* raw = obj.get();
        objects_[id] = std::move(obj);
        return raw;
    }

    template <typename T>
    T* get(uint32_t id) {
        auto it = objects_.find(id);
        return it == objects_.end() ? nullptr : dynamic_cast<T*>(it->second.get());
    }

    // Removed once the current request is done; client-created ids get delete_id
    void destroy(uint32_t id) {
        dead_.push_back(id);
    }

    void error(uint32_t object, uint32_t code, const std::string& msg) {
        event(kDisplayId, 0).u32(object).u32(code).str(msg);
        broken_ = true;
        server_->log("wayland client error: " + msg);
    }

    uint32_t next_serial() { return ++server_->serial_; }
    void count_frame() { server_->frames_++; }

    // false: drop the client
    bool on_readable();
    bool flush();

    int take_fd() {
        if (in_fds_.empty()) return -1;
        const int f = in_fds_.front();
        in_fds_.pop_front();
        return f;
    }

    // The mapped toplevel's surface id, 0 if none
    uint32_t toplevel_surface() const;
    // Seat events for this client's touch and keyboard objects
    void deliver(const InputEvent& e);

private:
    void dispatch(uint32_t id, uint16_t opcode, const uint8_t* body, size_t len);
    void reap() {
        for (uint32_t id : dead_) {
            if (objects_.erase(id) && id < 0xff000000u) event(kDisplayId, 1).u32(id);
        }
        dead_.clear();
    }

    Server* server_;
    int fd_;
    std::vector<uint8_t> in_;
    std::deque<int> in_fds_;
    std::vector<uint8_t> out_;
    std::vector<int> out_fds_;
    std::unordered_map<uint32_t, std::unique_ptr<Object>> objects_;
    std::vector<uint32_t> dead_;
    bool broken_ = false;
};

int Msg::fd() {
    const int f = client->take_fd();
    if (f < 0) ok = false;
    return f;
}

// ---- objects ----------------------------------------------------------

struct Callback : Object {
    void request(Msg&) override {}
    void done(uint32_t data) {
        client->event(id, 0).u32(data);
        client->destroy(id);
    }
};

struct Region : Object {
    void request(Msg& m) override {
        if (m.opcode == 0) client->destroy(id); // add/subtract: input and opaque hints, unused
    }
};

struct Buffer : Object {
    std::shared_ptr<PoolMem> mem;
    int32_t offset = 0, width = 0, height = 0, stride = 0;
    uint32_t format = 0;
    void request(Msg& m) override {
        if (m.opcode == 0) client->destroy(id);
    }
    const uint8_t* pixels() const {
        if (!mem || !mem->data || offset < 0 || width <= 0 || height <= 0 || stride < width * 4) return nullptr;
        if ((uint64_t)offset + (uint64_t)stride * (uint64_t)height > mem->size) return nullptr;
        return mem->data + offset;
    }
};

struct ShmPool : Object {
    std::shared_ptr<PoolMem> mem = std::make_shared<PoolMem>();
    void request(Msg& m) override {
        switch (m.opcode) {
        case 0: { // create_buffer
            const uint32_t nid = m.u32();
            const int32_t offset = m.i32(), w = m.i32(), h = m.i32(), stride = m.i32();
            const uint32_t format = m.u32();
            if (!m.ok) return;
            if (format != kShmArgb8888 && format != kShmXrgb8888 && format != kShmAbgr8888 && format != kShmXbgr8888) {
                client->error(id, 0, "unsupported shm format");
                return;
            }
            if (Buffer* b = client->add<Buffer>(nid, 1)) {
                b->mem = mem;
                b->offset = offset;
                b->width = w;
                b->height = h;
                b->stride = stride;
                b->format = format;
            }
            break;
        }
        case 1: client->destroy(id); break;
        case 2: { // resize
            const int32_t size = m.i32();
            if (m.ok && (size <= 0 || !mem->map((size_t)size))) client->error(id, 2, "pool resize failed");
            break;
        }
        }
    }
};

struct Shm : Object {
    void request(Msg& m) override {
        if (m.opcode != 0) return;
        const uint32_t nid = m.u32();
        const int fd = m.fd();
        const int32_t size = m.i32();
        if (!m.ok) {
            if (fd >= 0) close(fd);
            client->error(id, 1, "bad create_pool");
            return;
        }
        ShmPool* pool = client->add<ShmPool>(nid, 1);
        if (!pool) {
            close(fd);
            return;
        }
        pool->mem->fd = fd;
        if (size <= 0 || !pool->mem->map((size_t)size)) client->error(id, 2, "cannot map shm pool");
    }
    void announce() {
        for (uint32_t f : {kShmArgb8888, kShmXrgb8888, kShmAbgr8888, kShmXbgr8888}) client->event(id, 0).u32(f);
    }
};

struct XdgSurface;
struct Toplevel;

struct Surface : Object {
    uint32_t pending_buffer = 0;
    bool pending_attach = false;
    std::vector<uint32_t> pending_frames;
    XdgSurface* xdg = nullptr;
    bool initial_commit_done = false;

    void request(Msg& m) override;
    void commit();
    ~Surface() override;
};

struct Toplevel : Object {
    XdgSurface* xdg = nullptr;
    void request(Msg& m) override {
        if (m.opcode == 0) client->destroy(id); // the rest (title, app id, maximize, ...) is ignored
    }
    ~Toplevel() override;
};

struct XdgSurface : Object {
    Surface* surface = nullptr;
    Toplevel* toplevel = nullptr;
    bool configured = false;

    void request(Msg& m) override;
    void configure() {
        FrameSink* sink = client->server()->sink();
        if (toplevel) {
            const uint32_t states[2] = {1 /*maximized*/, 4 /*activated*/};
            client->event(toplevel->id, 0).i32((int32_t)sink->width()).i32((int32_t)sink->height()).arr(states, sizeof states);
        }
        client->event(id, 0).u32(client->next_serial());
        configured = true;
    }
    ~XdgSurface() override;
};

// Each side forgets the other when either goes away first
Surface::~Surface() {
    if (xdg) xdg->surface = nullptr;
}
Toplevel::~Toplevel() {
    if (xdg) xdg->toplevel = nullptr;
}
XdgSurface::~XdgSurface() {
    if (surface) surface->xdg = nullptr;
    if (toplevel) toplevel->xdg = nullptr;
}

// A popup gets its configure and is never shown
struct Positioner : Object {
    void request(Msg& m) override {
        if (m.opcode == 0) client->destroy(id);
    }
};

struct Popup : Object {
    void request(Msg& m) override {
        if (m.opcode == 0) client->destroy(id);
    }
};

void Surface::request(Msg& m) {
    switch (m.opcode) {
    case 0: client->destroy(id); break;
    case 1: // attach
        pending_buffer = m.u32();
        m.i32();
        m.i32();
        pending_attach = true;
        break;
    case 3: { // frame
        const uint32_t cb = m.u32();
        if (m.ok && client->add<Callback>(cb, 1)) pending_frames.push_back(cb);
        break;
    }
    case 6: commit(); break;
    default: break; // damage, regions, transform, scale, damage_buffer
    }
}

void Surface::commit() {
    if (xdg && !initial_commit_done) {
        initial_commit_done = true;
        if (!xdg->configured) xdg->configure();
    }
    if (pending_attach) {
        pending_attach = false;
        Buffer* b = pending_buffer ? client->get<Buffer>(pending_buffer) : nullptr;
        if (b && xdg && xdg->toplevel) {
            if (const uint8_t* px = b->pixels()) {
                client->server()->sink()->frame(px, (uint32_t)b->width, (uint32_t)b->height, (uint32_t)b->stride, b->format);
                client->count_frame();
            }
        }
        // The pixels are copied, the client may reuse the buffer right away
        if (b) client->event(b->id, 0);
    }
    const uint32_t t = now_ms();
    for (uint32_t cb : pending_frames)
        if (Callback* c = client->get<Callback>(cb)) c->done(t);
    pending_frames.clear();
}

void XdgSurface::request(Msg& m) {
    switch (m.opcode) {
    case 0: client->destroy(id); break;
    case 1: { // get_toplevel
        const uint32_t nid = m.u32();
        if (!m.ok) return;
        if (Toplevel* t = client->add<Toplevel>(nid, version)) {
            t->xdg = this;
            toplevel = t;
        }
        break;
    }
    case 2: { // get_popup
        const uint32_t nid = m.u32();
        if (!m.ok) return;
        if (Popup* p = client->add<Popup>(nid, version)) {
            client->event(p->id, 0).i32(0).i32(0).i32(1).i32(1); // xdg_popup.configure
            client->event(id, 0).u32(client->next_serial());
        }
        break;
    }
    default: break; // set_window_geometry, ack_configure
    }
}

struct WmBase : Object {
    void request(Msg& m) override {
        switch (m.opcode) {
        case 0: client->destroy(id); break;
        case 1: client->add<Positioner>(m.u32(), version); break;
        case 2: { // get_xdg_surface
            const uint32_t nid = m.u32();
            Surface* s = client->get<Surface>(m.u32());
            if (!m.ok) return;
            if (!s) {
                client->error(id, 0, "get_xdg_surface on a non-surface");
                return;
            }
            if (XdgSurface* x = client->add<XdgSurface>(nid, version)) {
                x->surface = s;
                s->xdg = x;
            }
            break;
        }
        default: break; // pong
        }
    }
};

struct Compositor : Object {
    void request(Msg& m) override {
        const uint32_t nid = m.u32();
        if (!m.ok) return;
        if (m.opcode == 0) client->add<Surface>(nid, version);
        else if (m.opcode == 1) client->add<Region>(nid, version);
    }
};

struct Output : Object {
    void request(Msg& m) override {
        if (m.opcode == 0) client->destroy(id);
    }
    void announce() {
        FrameSink* sink = client->server()->sink();
        const int32_t w = (int32_t)sink->width(), h = (int32_t)sink->height();
        // geometry: x, y, physical mm (about 400 dpi), subpixel unknown, make, model, normal transform
        client->event(id, 0).i32(0).i32(0).i32(w * 254 / 4000).i32(h * 254 / 4000).i32(0).str("Wayondroid").str("vinefb").i32(0);
        client->event(id, 1).u32(0x3 /*current|preferred*/).i32(w).i32(h).i32(60000);
        if (version >= 2) {
            client->event(id, 3).i32(1);
            client->event(id, 2);
        }
    }
};

struct InputDevice : Object {
    enum Kind { Pointer, Keyboard, Touch };
    Kind kind = Pointer;
    uint32_t entered = 0; // keyboard: surface it entered
    void request(Msg& m) override {
        // wl_pointer.release is opcode 1, keyboard/touch release opcode 0
        if (m.opcode == 1 || (m.opcode == 0 && kind != Pointer)) client->destroy(id);
    }
    void keymap() {
        // No keymap: the hwcomposer reads evdev codes as they are
        const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) client->event(id, 0).u32(0 /*no_keymap*/).fd(fd).u32(0);
        if (version >= 4) client->event(id, 5).i32(0).i32(0); // repeat_info: no repeat
    }
};

constexpr uint32_t kSeatKeyboard = 2, kSeatTouch = 4;

struct Seat : Object {
    void request(Msg& m) override {
        if (m.opcode <= 2) {
            InputDevice* d = client->add<InputDevice>(m.u32(), version);
            if (!d) return;
            d->kind = (InputDevice::Kind)m.opcode;
            if (d->kind == InputDevice::Keyboard) d->keymap();
        } else if (m.opcode == 3) {
            client->destroy(id);
        }
    }
    void announce() {
        client->event(id, 0).u32(kSeatKeyboard | kSeatTouch);
        if (version >= 2) client->event(id, 1).str("seat0");
    }
};

struct Registry : Object {
    void request(Msg& m) override {
        if (m.opcode != 0) return;
        const uint32_t name = m.u32();
        const std::string iface = m.str();
        const uint32_t want = m.u32();
        const uint32_t nid = m.u32();
        if (!m.ok) return;
        if (name == 0 || name >= G_COUNT || iface != kGlobals[name].interface || want == 0 || want > kGlobals[name].version) {
            client->error(id, 0, "bad bind of " + iface);
            return;
        }
        switch (name) {
        case G_COMPOSITOR: client->add<Compositor>(nid, want); break;
        case G_SHM:
            if (Shm* s = client->add<Shm>(nid, want)) s->announce();
            break;
        case G_WM_BASE: client->add<WmBase>(nid, want); break;
        case G_OUTPUT:
            if (Output* o = client->add<Output>(nid, want)) o->announce();
            break;
        case G_SEAT:
            if (Seat* s = client->add<Seat>(nid, want)) s->announce();
            break;
        }
    }
    void announce() {
        for (uint32_t g = 1; g < G_COUNT; ++g) client->event(id, 0).u32(g).str(kGlobals[g].interface).u32(kGlobals[g].version);
    }
};

struct DisplayObj : Object {
    void request(Msg& m) override {
        const uint32_t nid = m.u32();
        if (!m.ok) return;
        if (m.opcode == 0) { // sync
            if (Callback* c = client->add<Callback>(nid, 1)) c->done(client->next_serial());
        } else if (m.opcode == 1) { // get_registry
            if (Registry* r = client->add<Registry>(nid, 1)) r->announce();
        }
    }
};

// ---- input ------------------------------------------------------------

uint32_t Client::toplevel_surface() const {
    for (const auto& [oid, obj] : objects_) {
        const auto* s = dynamic_cast<const Surface*>(obj.get());
        if (s && s->xdg && s->xdg->toplevel && s->initial_commit_done) return oid;
    }
    return 0;
}

void Client::deliver(const InputEvent& e) {
    const uint32_t surface = toplevel_surface();
    if (!surface) return;
    FrameSink* sink = server_->sink();
    auto fixed = [](double v) { return (int32_t)(v * 256.0); };
    const int32_t x = fixed(e.x * sink->width()), y = fixed(e.y * sink->height());
    const uint32_t t = now_ms();
    for (const auto& [oid, obj] : objects_) {
        auto* d = dynamic_cast<InputDevice*>(obj.get());
        if (!d) continue;
        if (d->kind == InputDevice::Touch) {
            switch (e.kind) {
            case InputEvent::TouchDown: event(oid, 0).u32(next_serial()).u32(t).u32(surface).i32(e.id).i32(x).i32(y); break;
            case InputEvent::TouchUp: event(oid, 1).u32(next_serial()).u32(t).i32(e.id); break;
            case InputEvent::TouchMotion: event(oid, 2).u32(t).i32(e.id).i32(x).i32(y); break;
            case InputEvent::TouchFrame: event(oid, 3); break;
            case InputEvent::TouchCancel: event(oid, 4); break;
            case InputEvent::Key: break;
            }
        } else if (d->kind == InputDevice::Keyboard && e.kind == InputEvent::Key) {
            if (d->entered != surface) {
                event(oid, 1).u32(next_serial()).u32(surface).arr(nullptr, 0);
                d->entered = surface;
            }
            event(oid, 3).u32(next_serial()).u32(t).u32(e.key).u32(e.down ? 1 : 0);
        }
    }
    flush();
}

// ---- connection -------------------------------------------------------

Client::Client(Server* server, int fd) : server_(server), fd_(fd) {
    add<DisplayObj>(kDisplayId, 1);
}

bool Client::on_readable() {
    uint8_t buf[kMaxMessage];
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxFds)];
    iovec iov{buf, sizeof buf};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;
    const ssize_t n = recvmsg(fd_, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0) return errno == EAGAIN || errno == EINTR;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
        for (size_t i = 0; i < count; ++i) in_fds_.push_back(fds[i]);
    }
    if (n == 0) return false;
    in_.insert(in_.end(), buf, buf + n);

    size_t pos = 0;
    while (!broken_ && in_.size() - pos >= 8) {
        uint32_t id, word;
        memcpy(&id, &in_[pos], 4);
        memcpy(&word, &in_[pos + 4], 4);
        const size_t size = word >> 16;
        if (size < 8 || size % 4) {
            error(kDisplayId, 1, "bad message size");
            break;
        }
        if (in_.size() - pos < size) break;
        dispatch(id, (uint16_t)(word & 0xffff), &in_[pos + 8], size - 8);
        reap();
        pos += size;
    }
    in_.erase(in_.begin(), in_.begin() + (ptrdiff_t)pos);
    return flush() && !broken_;
}

void Client::dispatch(uint32_t id, uint16_t opcode, const uint8_t* body, size_t len) {
    auto it = objects_.find(id);
    if (it == objects_.end()) {
        // Requests on an object the client already destroyed are harmless
        return;
    }
    Msg m{this, id, opcode, body, body + len};
    it->second->request(m);
    if (!m.ok && !broken_) error(id, 1, "malformed request " + std::to_string(opcode));
}

bool Client::flush() {
    size_t sent = 0;
    while (sent < out_.size()) {
        iovec iov{out_.data() + sent, out_.size() - sent};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxFds)];
        const size_t nfds = std::min(out_fds_.size(), kMaxFds);
        if (nfds) {
            mh.msg_control = ctrl;
            mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
            cmsghdr* c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
            memcpy(CMSG_DATA(c), out_fds_.data(), sizeof(int) * nfds);
        }
        const ssize_t n = sendmsg(fd_, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            return false;
        }
        for (size_t i = 0; i < nfds; ++i) close(out_fds_[i]);
        out_fds_.erase(out_fds_.begin(), out_fds_.begin() + (ptrdiff_t)nfds);
        sent += (size_t)n;
    }
    out_.erase(out_.begin(), out_.begin() + (ptrdiff_t)sent);
    return true;
}

// ---- server -----------------------------------------------------------

Server::Server(FrameSink* sink, Log log) : sink_(sink), log_(std::move(log)) {
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) wake_[0] = wake_[1] = -1;
}

Server::~Server() {
    clients_.clear();
    for (auto& in : inputs_) close(in.fd);
    if (input_fd_ >= 0) {
        close(input_fd_);
        unlink(input_path_.c_str());
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(path_.c_str());
    }
    for (int f : wake_)
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

void Server::accept_client() {
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    clients_.push_back(std::make_unique<Client>(this, fd));
    log("wayland client connected");
}

bool Server::listen_input(const std::string& path, std::string* error) {
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
    if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(fd, 4) != 0) {
        if (error) *error = "listen on " + path + ": " + strerror(errno);
        close(fd);
        return false;
    }
    input_fd_ = fd;
    input_path_ = path;
    return true;
}

void Server::accept_input() {
    const int fd = accept4(input_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd >= 0) inputs_.push_back({fd, {}});
}

bool Server::read_input(InputConn& c) {
    char buf[4096];
    const ssize_t n = read(c.fd, buf, sizeof buf);
    if (n < 0) return errno == EAGAIN || errno == EINTR;
    if (n == 0) return false;
    c.buf.append(buf, (size_t)n);
    size_t start = 0;
    for (size_t nl; (nl = c.buf.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::vector<InputEvent> evs;
        std::string error;
        if (!parse_input_line(c.buf.substr(start, nl - start), &evs, &error)) {
            log(error);
            continue;
        }
        for (const InputEvent& e : evs) inject(e);
    }
    c.buf.erase(0, start);
    return c.buf.size() < 65536;
}

Client* Server::focus() const {
    // The newest client with a toplevel: a restarted hwcomposer replaces the old one
    for (auto it = clients_.rbegin(); it != clients_.rend(); ++it)
        if ((*it)->toplevel_surface()) return it->get();
    return nullptr;
}

bool Server::inject(const InputEvent& e) {
    Client* c = focus();
    if (!c) return false;
    c->deliver(e);
    return true;
}

bool Server::run() {
    for (;;) {
        std::vector<pollfd> pfds;
        pfds.push_back({wake_[0], POLLIN, 0});
        pfds.push_back({listen_fd_, POLLIN, 0});
        pfds.push_back({input_fd_, POLLIN, 0});
        for (auto& in : inputs_) pfds.push_back({in.fd, POLLIN, 0});
        const size_t first_client = pfds.size();
        for (auto& c : clients_) pfds.push_back({c->fd(), (short)(POLLIN | (c->wants_write() ? POLLOUT : 0)), 0});
        if (poll(pfds.data(), pfds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pfds[0].revents) return true;
        if (pfds[1].revents & POLLIN) accept_client();
        if (pfds[2].revents & POLLIN) accept_input();
        for (size_t i = 3, k = 0; i < first_client; ++i) {
            if ((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) && !read_input(inputs_[k])) {
                close(inputs_[k].fd);
                inputs_.erase(inputs_.begin() + (ptrdiff_t)k);
            } else {
                ++k;
            }
        }
        for (size_t i = first_client, k = 0; i < pfds.size(); ++i) {
            Client* c = clients_[k].get();
            bool keep = true;
            if (pfds[i].revents & POLLIN) keep = c->on_readable();
            else if (pfds[i].revents & (POLLERR | POLLHUP)) keep = false;
            if (keep && (pfds[i].revents & POLLOUT)) keep = c->flush();
            if (keep) {
                ++k;
            } else {
                clients_.erase(clients_.begin() + (ptrdiff_t)k);
                log("wayland client gone");
            }
        }
    }
}

void Server::stop() {
    if (wake_[1] >= 0) (void)!write(wake_[1], "x", 1);
}

} // namespace vine::wl
