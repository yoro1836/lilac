#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

namespace vine {

// Guests get their own netns with a veth eth0 in a /30 of 10.77.<slot>.0, NATed out by the host, best effort
struct NetPlan {
    int slot = 0;            // 1..250, unique among running containers
    std::string host_if;     // "vh<slot>"
    std::string subnet;      // "10.77.<slot>.0/30"
    std::string host_ip;     // 10.77.<slot>.1
    std::string guest_ip;    // 10.77.<slot>.2
    std::string dns = "8.8.8.8";
};

NetPlan make_net_plan(int slot);

// Runs in the guest netns before init, brings up lo and eth0 with ioctls only
bool configure_guest_network(const NetPlan& plan, std::string* error);

// Host side of the veth pair and the NAT rules. Lives as long as the container.
class HostNat {
public:
    ~HostNat() { teardown(); }

    // guest_pid is any process in the guest netns, false if the veth pair could not be made
    bool setup(pid_t guest_pid, const NetPlan& plan, std::string* error);
    void teardown();
    bool active() const { return active_; }

private:
    bool run(const std::vector<std::string>& args, std::string* error, bool required);

    bool active_ = false;
    std::string ip_;
    std::string iptables_;
    std::string host_if_;
    std::vector<std::vector<std::string>> undo_;
};

} // namespace vine
