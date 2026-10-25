#include "rootless_server.h"
#include "../diagnostics/host_probe.h"
#include "../display/wayland/input_lines.h"
#include "../utils/vine_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace vine {

namespace {

using rootless::Instance;

bool valid_name(const std::string& s, size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char c : s) {
        const bool ok = isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return s != "." && s != "..";
}

bool valid_package(const std::string& s) {
    if (s.empty() || s.size() > 255) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_')) return false;
    return true;
}

int to_int(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    return (end && *end == '\0') ? (int)v : fallback;
}

// "pkg/cls" lines out of cmd package's --components output
std::vector<std::string> components(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& raw : split_lines(text)) {
        const size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const std::string line = raw.substr(a, raw.find_last_not_of(" \t\r") + 1 - a);
        const size_t slash = line.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == line.size()) continue;
        bool ok = true;
        for (char c : line) ok = ok && (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '$' || c == '/');
        if (ok) out.push_back(line);
    }
    return out;
}

const char* const kLaunchScript =
    "c=$(cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \"$1\" | tail -n 1)\n"
    "case $c in */*) ;; *) echo \"no launcher activity in $1\"; exit 1 ;; esac\n"
    "out=$(cmd activity start-activity -n \"$c\" 2>&1)\n"
    "echo \"$out\"\n"
    "case $out in *Error*) exit 1 ;; esac\n";

} // namespace

RootlessServer::RootlessServer(int in_fd, int out_fd, RootlessOptions options)
    : LineServer(in_fd, out_fd), options_(std::move(options)) {}

RootlessServer::~RootlessServer() = default;

bool RootlessServer::path_allowed(const std::string& path) const {
    return !options_.base_dir.empty() && path.size() > options_.base_dir.size() && path[0] == '/' &&
           path_is_within(path, options_.base_dir);
}

Instance* RootlessServer::find(const std::string& instance) {
    auto it = instances_.find(instance);
    return it == instances_.end() ? nullptr : it->second.get();
}

// The instance named by args[0] if it is up, otherwise replies with an error
Instance* RootlessServer::running(const std::string& id, const std::vector<std::string>& args) {
    Instance* in = args.empty() ? nullptr : find(args[0]);
    if (!in || !in->active()) {
        reply(id, false, "instance is not running");
        return nullptr;
    }
    return in;
}

void RootlessServer::tick() {
    for (auto& [name, in] : instances_) {
        in->poll();
        const std::string now = rootless::state_name(in->state());
        std::string& last = last_state_[name];
        if (now != last) {
            last = now;
            emit("state", {name, now, in->state() == Instance::State::Error ? in->error() : ""});
        }
    }
}

void RootlessServer::shutdown() {
    for (auto& [name, in] : instances_) in->stop();
}

void RootlessServer::dispatch(const std::string& id, const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "hello") {
        reply(id, true, "proto=1\nengine=rootless\nuid=" + std::to_string(getuid()) + "\npid=" + std::to_string(getpid()) +
                            "\nbase=" + options_.base_dir);
    } else if (cmd == "probe") {
        reply(id, true, diagnostics::capabilities_to_text(diagnostics::probe_host()));
    } else if (cmd == "start") {
        cmd_start(id, args);
    } else if (cmd == "stop" || cmd == "kill") {
        Instance* in = args.empty() ? nullptr : find(args[0]);
        if (!in) return reply(id, true, "not running");
        in->stop();
        tick();
        reply(id, true, rootless::state_name(in->state()));
    } else if (cmd == "pause" || cmd == "resume") {
        Instance* in = args.empty() ? nullptr : find(args[0]);
        if (!in) return reply(id, false, "unknown instance");
        const bool ok = cmd == "pause" ? in->pause() : in->resume();
        tick();
        reply(id, ok, ok ? "" : "wrong state");
    } else if (cmd == "status") {
        Instance* in = args.empty() ? nullptr : find(args[0]);
        if (!in) return reply(id, true, "state=stopped\ninit_pid=-1\nboot=0\ncan_pause=0\nerror=");
        in->poll();
        reply(id, true, std::string("state=") + rootless::state_name(in->state()) + "\ninit_pid=" + std::to_string(in->init_pid()) +
                            "\nboot=" + (in->booted() ? "1" : "0") + "\ncan_pause=1\nerror=" + escape_line(in->error()));
    } else if (cmd == "list") {
        std::string out;
        for (const auto& [name, in] : instances_) out += name + "=" + rootless::state_name(in->state()) + "\n";
        reply(id, true, out);
    } else if (cmd == "diag") {
        Instance* in = args.empty() ? nullptr : find(args[0]);
        reply(id, in != nullptr, in ? in->diagnostics() : "unknown instance");
    } else if (cmd == "exec") {
        cmd_exec(id, args);
    } else if (cmd == "agent" || cmd == "agent_send") {
        cmd_agent(id, args);
    } else if (cmd == "apps") {
        cmd_apps(id, args);
    } else if (cmd == "launch") {
        cmd_launch(id, args);
    } else if (cmd == "icon") {
        reply(id, false, "no icons from rootless guests yet");
    } else if (cmd == "snapshot" || cmd == "restore" || cmd == "delete_snapshot") {
        reply(id, false, "Snapshots are not available for rootless instances yet");
    } else if (cmd == "snapshots") {
        reply(id, true, "");
    } else if (cmd == "rmtree") {
        cmd_rmtree(id, args);
    } else if (cmd == "quit") {
        reply(id, true, "");
        request_quit();
    } else {
        reply(id, false, "unknown command: " + cmd);
    }
}

void RootlessServer::cmd_start(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 2) return reply(id, false, "usage: start <instance> <path> [key=value ...]");
    const std::string& name = args[0];
    const std::string& path = args[1];
    if (!valid_name(name, 64)) return reply(id, false, "invalid instance id");
    if (!path_allowed(path)) return reply(id, false, "instance path is outside the instances directory");
    if (Instance* old = find(name)) {
        if (old->active()) return reply(id, false, "instance is already running");
        instances_.erase(name);
    }

    rootless::InstanceConfig cfg;
    cfg.id = name;
    cfg.path = path;
    cfg.lib_dir = options_.lib_dir;
    cfg.host_has_32bit = host_supports_aarch32();
    for (size_t i = 2; i < args.size(); ++i) {
        const size_t eq = args[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = args[i].substr(0, eq), v = args[i].substr(eq + 1);
        if (k == "width") cfg.width = to_int(v, cfg.width);
        else if (k == "height") cfg.height = to_int(v, cfg.height);
        else if (k == "dpi") cfg.dpi = to_int(v, cfg.dpi);
        else if (k == "refresh") cfg.refresh_hz = to_int(v, cfg.refresh_hz);
        else if (k == "boot_timeout") cfg.boot_timeout_s = to_int(v, cfg.boot_timeout_s);
        else if (k == "graphics") cfg.gpu = v == "gfxstream";
    }
    if (cfg.width < 64 || cfg.height < 64 || cfg.width > 8192 || cfg.height > 8192) {
        return reply(id, false, "unreasonable display size");
    }
    cfg.dpi = std::clamp(cfg.dpi, 80, 960);

    auto in = std::make_unique<Instance>(cfg);
    std::string error;
    if (!in->start(&error)) {
        emit("state", {name, "error", error});
        last_state_[name] = "error";
        return reply(id, false, error);
    }
    instances_[name] = std::move(in);
    last_state_[name] = "booting";
    emit("state", {name, "booting", ""});
    reply(id, true, "init_pid=-1");
}

void RootlessServer::cmd_exec(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 3) return reply(id, false, "usage: exec <instance> <timeout_ms> <program> [args...]");
    Instance* in = running(id, args);
    if (!in) return;
    const auto r = in->exec(std::vector<std::string>(args.begin() + 2, args.end()), std::max(100, to_int(args[1], 10000)));
    if (!r.error.empty()) return reply(id, false, r.error);
    reply(id, true, "exit=" + std::to_string(r.exit_code) + "\n" + r.output);
}

// There is no guest agent: input goes to vine_wl, "ping" says input works
void RootlessServer::cmd_agent(const std::string& id, const std::vector<std::string>& args) {
    const bool send_only = id == "-";
    if (args.size() < (send_only ? 2u : 3u)) return reply(id, false, "usage: agent <instance> <timeout_ms> <line>");
    Instance* in = running(id, args);
    if (!in) return;
    const std::string& line = send_only ? args[1] : args[2];
    if (!send_only) {
        if (line == "ping") return reply(id, true, "ok");
        return reply(id, false, "rootless guests have no agent");
    }
    // Keys the hwcomposer can't carry (APP_SWITCH, ...) go through `input`
    int keycode = 0, down = 0;
    if (sscanf(line.c_str(), "key %d %d", &keycode, &down) == 2 && vine::wl::evdev_for_keycode(keycode) == 0) {
        if (down) in->exec_detached({"input", "keyevent", std::to_string(keycode)});
        return;
    }
    in->send_input(line);
}

void RootlessServer::cmd_apps(const std::string& id, const std::vector<std::string>& args) {
    Instance* in = running(id, args);
    if (!in) return;
    const auto r = in->exec({"cmd", "package", "query-activities", "--brief", "--components", "-a", "android.intent.action.MAIN",
                             "-c", "android.intent.category.LAUNCHER"},
                            30000);
    if (!r.error.empty()) return reply(id, false, r.error);
    if (r.exit_code != 0) return reply(id, false, "cmd package failed: " + r.output.substr(0, 200));
    std::set<std::string> seen;
    std::string out;
    for (const auto& c : components(r.output)) {
        const std::string pkg = c.substr(0, c.find('/'));
        if (valid_package(pkg) && seen.insert(pkg).second) out += pkg + "\t" + pkg + "\n";
    }
    reply(id, true, out);
}

void RootlessServer::cmd_launch(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 2) return reply(id, false, "usage: launch <instance> <package>");
    Instance* in = running(id, args);
    if (!in) return;
    if (!valid_package(args[1])) return reply(id, false, "invalid package name");
    const auto r = in->exec({"sh", "-c", kLaunchScript, "sh", args[1]}, 15000);
    if (!r.error.empty()) return reply(id, false, r.error);
    reply(id, r.exit_code == 0, r.exit_code == 0 ? "" : "could not launch " + args[1]);
}

void RootlessServer::cmd_rmtree(const std::string& id, const std::vector<std::string>& args) {
    if (args.empty() || !path_allowed(args[0])) return reply(id, false, "path is outside the instances directory");
    for (auto it = instances_.begin(); it != instances_.end();) {
        const bool inside = path_is_within(it->second->config().path, args[0]);
        if (inside && it->second->active()) return reply(id, false, "instance is still running");
        if (inside) {
            last_state_.erase(it->first);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
    struct stat st{};
    if (lstat(args[0].c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return reply(id, false, "refusing to delete a symlink");
    const bool ok = remove_recursive(args[0]);
    reply(id, ok, ok ? "" : "could not delete everything");
}

} // namespace vine
