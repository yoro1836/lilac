#pragma once

#include <string>

namespace vine {

// Link to the guest agent on shared/agent.sock, replies end with a lone "." line
class AgentLink {
public:
    explicit AgentLink(std::string socket_path) : path_(std::move(socket_path)) {}
    ~AgentLink() { close_link(); }

    AgentLink(const AgentLink&) = delete;
    AgentLink& operator=(const AgentLink&) = delete;

    // Sends a line and returns the reply without its closing "." line, false on timeout or no agent
    bool request(const std::string& line, std::string* reply, int timeout_ms, std::string* error);

    // Fire and forget for high-rate input. Drops the message when the agent is absent or too slow.
    void send(const std::string& line);

    void close_link();

private:
    bool ensure_connected(std::string* error);

    std::string path_;
    int fd_ = -1;
};

} // namespace vine
