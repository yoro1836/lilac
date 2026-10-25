#pragma once

#include "agent_link.h"
#include "line_server.h"
#include "../container/namespace_manager.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

namespace vine {

struct HelperOptions {
    std::string base_dir;          // every instance path the app sends must be inside this directory
    std::string qemu_arm_path;     // static qemu-arm for 32-bit ARM guests, may be empty
    int owner_uid = -1;            // the app's uid, used to hand files back to it
    int owner_gid = -1;
};

// Root side of Wayondroid, spoken over stdin and stdout as tab separated lines (see HelperProtocol.kt)
class HelperServer : public LineServer {
public:
    HelperServer(int in_fd, int out_fd, HelperOptions options);
    ~HelperServer() override;

private:
    struct Job {
        pid_t pid = -1;
        std::string kind;       // "snapshot", "restore"
        std::string instance;
        std::string name;
        bool resume_after = false;
    };

    void dispatch(const std::string& id, const std::string& cmd, const std::vector<std::string>& args) override;
    void tick() override;
    // Stops every guest
    void shutdown() override;

    void cmd_start(const std::string& id, const std::vector<std::string>& args);
    void cmd_stop(const std::string& id, const std::vector<std::string>& args, bool force);
    void cmd_status(const std::string& id, const std::vector<std::string>& args);
    void cmd_exec(const std::string& id, const std::vector<std::string>& args);
    void cmd_agent(const std::string& id, const std::vector<std::string>& args);
    void cmd_apps(const std::string& id, const std::vector<std::string>& args);
    void cmd_launch(const std::string& id, const std::vector<std::string>& args);
    void cmd_icon(const std::string& id, const std::vector<std::string>& args);
    void cmd_snapshot(const std::string& id, const std::vector<std::string>& args, bool restore);
    void cmd_list_snapshots(const std::string& id, const std::vector<std::string>& args);
    void cmd_delete_snapshot(const std::string& id, const std::vector<std::string>& args);
    void cmd_rmtree(const std::string& id, const std::vector<std::string>& args);

    Container* find(const std::string& instance);
    bool path_allowed(const std::string& path) const;
    bool is_active(const Container* c) const;
    AgentLink* agent_for(const std::string& instance);

    HelperOptions options_;
    std::unordered_map<std::string, int64_t> handles_;
    std::unordered_map<std::string, std::string> last_state_;
    std::unordered_map<std::string, std::unique_ptr<AgentLink>> agents_;
    std::vector<Job> jobs_;
};

// Entry point of the helper executable: parses the command line and runs a HelperServer on stdin and stdout.
int helper_main(int argc, char** argv);

} // namespace vine
