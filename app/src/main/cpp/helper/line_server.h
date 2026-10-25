#pragma once

#include <string>
#include <vector>

namespace vine {

// The helper protocol's transport: tab separated request lines on in_fd
// ("id cmd args..."), replies ("id ok|err payload") and events
// ("* name fields...") on out_fd. See HelperProtocol.kt.
class LineServer {
public:
    LineServer(int in_fd, int out_fd) : in_fd_(in_fd), out_fd_(out_fd) {}
    virtual ~LineServer() = default;

    // Serves requests until stdin closes or "quit" arrives, then calls shutdown()
    int run();

    // Also takes requests on a unix socket (debug builds: adb run-as can
    // drive the engine as the app). Replies go back on that connection.
    bool listen_debug(const std::string& path, std::string* error);

protected:
    virtual void dispatch(const std::string& id, const std::string& cmd, const std::vector<std::string>& args) = 0;
    // Called at least every 200 ms
    virtual void tick() {}
    virtual void shutdown() {}

    void reply(const std::string& id, bool ok, const std::string& payload);
    void emit(const std::string& event, const std::vector<std::string>& fields);
    void request_quit() { quit_ = true; }

private:
    void handle_line(const std::string& line);
    void write_out(const std::string& line);
    void serve_debug();

    int in_fd_;
    int out_fd_;
    std::string inbox_;
    bool quit_ = false;
    int debug_fd_ = -1;
    bool in_debug_ = false;
};

// Sends one request line to a server's debug socket and prints the reply
int line_server_send(const std::string& path, const std::vector<std::string>& request);

} // namespace vine
