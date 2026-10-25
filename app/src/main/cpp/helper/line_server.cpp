#include "line_server.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vine {

void LineServer::write_out(const std::string& line) {
    const std::string msg = line + "\n";
    const char* p = msg.data();
    size_t left = msg.size();
    while (left > 0) {
        const ssize_t n = write(out_fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!in_debug_) quit_ = true; // the app is gone
            return;
        }
        p += n;
        left -= (size_t)n;
    }
}

void LineServer::reply(const std::string& id, bool ok, const std::string& payload) {
    if (id == "-") return;
    write_out(id + "\t" + (ok ? "ok" : "err") + "\t" + escape_line(payload));
}

void LineServer::emit(const std::string& event, const std::vector<std::string>& fields) {
    std::string line = "*\t" + event;
    for (const auto& f : fields) line += "\t" + escape_line(f);
    write_out(line);
}

void LineServer::handle_line(const std::string& line) {
    const auto fields = split_fields(line, '\t');
    if (fields.size() < 2) return;
    std::vector<std::string> args;
    for (size_t i = 2; i < fields.size(); ++i) args.push_back(unescape_line(fields[i]));
    dispatch(fields[0], fields[1], args);
}

bool LineServer::listen_debug(const std::string& path, std::string* error) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) {
        if (error) *error = "socket path too long: " + path;
        return false;
    }
    memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    unlink(path.c_str());
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(fd, 4) != 0) {
        if (error) *error = "listen on " + path + ": " + strerror(errno);
        if (fd >= 0) close(fd);
        return false;
    }
    debug_fd_ = fd;
    return true;
}

// One request per connection, answered before the next is taken
void LineServer::serve_debug() {
    const int c = accept4(debug_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) return;
    timeval tv{2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string line;
    char ch;
    while (line.size() < 65536 && read(c, &ch, 1) == 1 && ch != '\n') line += ch;
    const int saved = out_fd_;
    out_fd_ = c;
    in_debug_ = true;
    if (!line.empty()) handle_line(line);
    in_debug_ = false;
    out_fd_ = saved;
    close(c);
}

int line_server_send(const std::string& path, const std::vector<std::string>& request) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path || request.empty()) return 2;
    memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        fprintf(stderr, "connect %s: %s\n", path.c_str(), strerror(errno));
        return 1;
    }
    std::string line = "d\t" + request[0];
    for (size_t i = 1; i < request.size(); ++i) line += "\t" + escape_line(request[i]);
    line += "\n";
    if (write(fd, line.data(), line.size()) != (ssize_t)line.size()) return 1;
    std::string reply;
    char buf[65536];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) reply.append(buf, (size_t)n);
    close(fd);
    // "d ok|err payload", then any events the request caused
    bool ok = false;
    for (const auto& l : split_lines(reply)) {
        const auto f = split_fields(l, '\t');
        if (f.size() >= 2 && f[0] == "d") {
            ok = f[1] == "ok";
            printf("%s%s", f.size() > 2 ? unescape_line(f[2]).c_str() : "", ok ? "" : "\n");
        }
    }
    return ok ? 0 : 1;
}

int LineServer::run() {
    emit("ready", {"1"});
    char buf[65536];
    while (!quit_) {
        pollfd fds[2] = {{in_fd_, POLLIN, 0}, {debug_fd_, POLLIN, 0}};
        const int pr = poll(fds, debug_fd_ >= 0 ? 2 : 1, 200);
        if (pr > 0 && debug_fd_ >= 0 && (fds[1].revents & POLLIN)) serve_debug();
        const pollfd& p = fds[0];
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(in_fd_, buf, sizeof(buf));
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                quit_ = true;
            } else if (n > 0) {
                inbox_.append(buf, (size_t)n);
                size_t nl;
                while (!quit_ && (nl = inbox_.find('\n')) != std::string::npos) {
                    std::string line = inbox_.substr(0, nl);
                    inbox_.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) handle_line(line);
                }
            }
        }
        tick();
    }
    shutdown();
    if (debug_fd_ >= 0) close(debug_fd_);
    return 0;
}

} // namespace vine
