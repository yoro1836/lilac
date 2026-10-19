#include "net_setup.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <net/if.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vine {

NetPlan make_net_plan(int slot) {
    NetPlan p;
    p.slot = slot;
    p.host_if = "vh" + std::to_string(slot);
    const std::string base = "10.77." + std::to_string(slot) + ".";
    p.subnet = base + "0/30";
    p.host_ip = base + "1";
    p.guest_ip = base + "2";
    return p;
}

namespace {

void fill_addr(sockaddr* out, const char* text) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, text, &sin.sin_addr);
    memcpy(out, &sin, sizeof(sin));
}

bool set_link_up(int sock, const char* name) {
    ifreq ifr{};
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) return false;
    ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP | IFF_RUNNING);
    return ioctl(sock, SIOCSIFFLAGS, &ifr) == 0;
}

} // namespace

bool configure_guest_network(const NetPlan& plan, std::string* error) {
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        if (error) *error = std::string("socket: ") + strerror(errno);
        return false;
    }
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg + ": " + strerror(errno);
        close(sock);
        return false;
    };

    if (!set_link_up(sock, "lo")) return fail("bring lo up");
    if (plan.guest_ip.empty() || if_nametoindex("eth0") == 0) {
        close(sock);
        return true; // loopback only
    }

    ifreq ifr{};
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    fill_addr(&ifr.ifr_addr, plan.guest_ip.c_str());
    if (ioctl(sock, SIOCSIFADDR, &ifr) != 0) return fail("set eth0 address");
    fill_addr(&ifr.ifr_netmask, "255.255.255.252");
    if (ioctl(sock, SIOCSIFNETMASK, &ifr) != 0) return fail("set eth0 netmask");
    if (!set_link_up(sock, "eth0")) return fail("bring eth0 up");

    rtentry rt{};
    fill_addr(&rt.rt_dst, "0.0.0.0");
    fill_addr(&rt.rt_genmask, "0.0.0.0");
    fill_addr(&rt.rt_gateway, plan.host_ip.c_str());
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    if (ioctl(sock, SIOCADDRT, &rt) != 0 && errno != EEXIST) return fail("add default route");

    close(sock);
    return true;
}

bool HostNat::run(const std::vector<std::string>& args, std::string* error, bool required) {
    std::string out;
    const int rc = exec_capture(args, &out, 10000, 4096);
    if (rc == 0) return true;
    std::string cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? "" : " ") + a;
    const std::string msg = cmd + " -> exit " + std::to_string(rc) + (out.empty() ? "" : " (" + out.substr(0, 200) + ")");
    if (required) {
        if (error) *error = msg;
        VINE_LOGE("net: %s", msg.c_str());
    } else {
        VINE_LOGW("net: %s", msg.c_str());
    }
    return false;
}

bool HostNat::setup(pid_t guest_pid, const NetPlan& plan, std::string* error) {
    ip_ = find_executable("ip");
    if (ip_.empty()) {
        if (error) *error = "no `ip` tool found on the host";
        return false;
    }
    host_if_ = plan.host_if;

    std::string ignored;
    exec_capture({ip_, "link", "del", host_if_}, &ignored, 5000, 512); // leftover from a crashed run

    if (!run({ip_, "link", "add", host_if_, "type", "veth", "peer", "name", "eth0", "netns", std::to_string(guest_pid)},
             error, true)) {
        return false;
    }
    undo_.push_back({ip_, "link", "del", host_if_});
    if (!run({ip_, "addr", "add", plan.host_ip + "/30", "dev", host_if_}, error, true) ||
        !run({ip_, "link", "set", host_if_, "up"}, error, true)) {
        teardown();
        return false;
    }
    active_ = true;

    write_file("/proc/sys/net/ipv4/ip_forward", "1");

    iptables_ = find_executable("iptables");
    if (!iptables_.empty()) {
        const std::vector<std::vector<std::string>> rules = {
            {"-t", "nat", "POSTROUTING", "-s", plan.subnet, "!", "-o", host_if_, "-j", "MASQUERADE"},
            {"FORWARD", "-i", host_if_, "-j", "ACCEPT"},
            {"FORWARD", "-o", host_if_, "-j", "ACCEPT"},
        };
        for (const auto& r : rules) {
            std::vector<std::string> add = {iptables_}, del = {iptables_};
            const size_t chain = r[0] == "-t" ? 2 : 0;
            for (size_t i = 0; i < chain; ++i) { add.push_back(r[i]); del.push_back(r[i]); }
            add.push_back(chain ? "-A" : "-I");
            del.push_back("-D");
            add.push_back(r[chain]);
            del.push_back(r[chain]);
            for (size_t i = chain + 1; i < r.size(); ++i) { add.push_back(r[i]); del.push_back(r[i]); }
            if (run(add, nullptr, false)) undo_.push_back(del);
        }
    } else {
        VINE_LOGW("net: no iptables, the guest will not reach the internet");
    }

    // Android routes per network, so traffic from the guest must be pointed at the table of the current uplink
    std::string route;
    if (exec_capture({ip_, "route", "get", "1.1.1.1"}, &route, 5000, 2048) == 0) {
        const size_t at = route.find(" dev ");
        if (at != std::string::npos) {
            std::string uplink = route.substr(at + 5, route.find_first_of(" \n", at + 5) - (at + 5));
            if (!uplink.empty() &&
                run({ip_, "rule", "add", "from", plan.subnet, "lookup", uplink, "priority", "9000"}, nullptr, false)) {
                undo_.push_back({ip_, "rule", "del", "from", plan.subnet, "lookup", uplink, "priority", "9000"});
            }
        }
    }
    VINE_LOGI("net: %s up, guest will use %s", host_if_.c_str(), plan.guest_ip.c_str());
    return true;
}

void HostNat::teardown() {
    for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
        std::string out;
        exec_capture(*it, &out, 5000, 512);
    }
    undo_.clear();
    active_ = false;
}

} // namespace vine
