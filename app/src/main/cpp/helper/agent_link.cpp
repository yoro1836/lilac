#include "agent_link.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vine {

namespace {
constexpr size_t kMaxReply = 8 * 1024 * 1024;
}

void AgentLink::close_link() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool AgentLink::ensure_connected(std::string* error) {
    if (fd_ >= 0) return true;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) {
        if (error) *error = "agent socket path too long";
        return false;
    }
    memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + strerror(errno);
        return false;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = std::string("the guest agent is not running (") + strerror(errno) + ")";
        close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

void AgentLink::send(const std::string& line) {
    std::string ignored;
    if (!ensure_connected(&ignored)) return;
    const std::string msg = line + "\n";
    const ssize_t n = ::send(fd_, msg.data(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) close_link();
    else if (n >= 0 && (size_t)n < msg.size()) close_link(); // half a line would corrupt the stream
}

bool AgentLink::request(const std::string& line, std::string* reply, int timeout_ms, std::string* error) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected(error)) return false;
        const std::string msg = line + "\n";
        if (::send(fd_, msg.data(), msg.size(), MSG_NOSIGNAL) != (ssize_t)msg.size()) {
            close_link(); // stale connection from an earlier boot, reconnect once
            continue;
        }
        std::string buf;
        const int64_t deadline = monotonic_ms() + timeout_ms;
        for (;;) {
            if (buf == ".\n" || (buf.size() >= 3 && buf.compare(buf.size() - 3, 3, "\n.\n") == 0)) {
                buf.resize(buf.size() >= 3 && buf != ".\n" ? buf.size() - 2 : 0);
                if (reply) *reply = buf;
                return true;
            }
            const int left = (int)(deadline - monotonic_ms());
            if (left <= 0 || buf.size() > kMaxReply) {
                if (error) *error = left <= 0 ? "the guest agent did not answer in time" : "the guest agent reply is too large";
                close_link(); // a late reply would be mistaken for the next one
                return false;
            }
            pollfd p{fd_, POLLIN, 0};
            const int pr = poll(&p, 1, left);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) continue;
            char chunk[8192];
            const ssize_t n = read(fd_, chunk, sizeof(chunk));
            if (n <= 0) {
                close_link();
                if (buf.empty() && attempt == 0) break; // the agent restarted, try a fresh connection
                if (error) *error = "the guest agent closed the connection";
                return false;
            }
            buf.append(chunk, (size_t)n);
        }
    }
    if (error && error->empty()) *error = "the guest agent is not reachable";
    return false;
}

} // namespace vine
