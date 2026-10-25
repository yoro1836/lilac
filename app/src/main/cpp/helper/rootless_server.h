#pragma once

#include "line_server.h"
#include "../rootless/rootless_instance.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vine {

struct RootlessOptions {
    std::string base_dir;   // every instance path the app sends must be inside this directory
    std::string lib_dir;    // the app's native library dir: tawcroot, binderd, vine_wl, ...
};

// The rootless engine behind the helper protocol, run as the app's own
// uid (no su): Waydroid images under tawcroot (docs/rootless.md). Same
// commands as HelperServer; snapshots and icons are not supported yet.
class RootlessServer : public LineServer {
public:
    RootlessServer(int in_fd, int out_fd, RootlessOptions options);
    ~RootlessServer() override;

private:
    void dispatch(const std::string& id, const std::string& cmd, const std::vector<std::string>& args) override;
    void tick() override;
    void shutdown() override;

    void cmd_start(const std::string& id, const std::vector<std::string>& args);
    void cmd_exec(const std::string& id, const std::vector<std::string>& args);
    void cmd_agent(const std::string& id, const std::vector<std::string>& args);
    void cmd_apps(const std::string& id, const std::vector<std::string>& args);
    void cmd_launch(const std::string& id, const std::vector<std::string>& args);
    void cmd_rmtree(const std::string& id, const std::vector<std::string>& args);

    rootless::Instance* find(const std::string& instance);
    rootless::Instance* running(const std::string& id, const std::vector<std::string>& args);
    bool path_allowed(const std::string& path) const;

    RootlessOptions options_;
    std::unordered_map<std::string, std::unique_ptr<rootless::Instance>> instances_;
    std::unordered_map<std::string, std::string> last_state_;
};

} // namespace vine
