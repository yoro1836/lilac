#include "line_server.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <poll.h>
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
            quit_ = true; // the app is gone
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

int LineServer::run() {
    emit("ready", {"1"});
    char buf[65536];
    while (!quit_) {
        pollfd p{in_fd_, POLLIN, 0};
        const int pr = poll(&p, 1, 200);
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
    return 0;
}

} // namespace vine
