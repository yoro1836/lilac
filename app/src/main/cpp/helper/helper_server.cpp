#include "helper_server.h"
#include "../container/guest_exec.h"
#include "../diagnostics/host_probe.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vine {

namespace {

const char* const kStateNames[] = {"stopped", "booting", "running", "error", "paused"};

const char* state_name(ContainerStatus s) { return kStateNames[(int)s]; }

bool valid_name(const std::string& s, size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return s != "." && s != "..";
}

uint64_t allocated_bytes(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_blocks * 512ULL : 0;
}

uint64_t free_bytes(const std::string& path) {
    struct statvfs sv{};
    return statvfs(path.c_str(), &sv) == 0 ? (uint64_t)sv.f_bavail * sv.f_frsize : 0;
}

int to_int(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    return (end && *end == '\0') ? (int)v : fallback;
}

// Lines of an agent reply without the leading "ok ..." line; false if the agent said "err" or nothing
bool agent_payload(const std::string& reply, std::string* payload, std::string* error) {
    const size_t nl = reply.find('\n');
    const std::string first = reply.substr(0, nl);
    if (first.rfind("ok", 0) != 0) {
        if (error) *error = first.rfind("err", 0) == 0 ? first.substr(first.size() > 4 ? 4 : first.size()) : "unexpected agent reply";
        return false;
    }
    if (payload) *payload = nl == std::string::npos ? "" : reply.substr(nl + 1);
    return true;
}

} // namespace

HelperServer::HelperServer(int in_fd, int out_fd, HelperOptions options)
    : in_fd_(in_fd), out_fd_(out_fd), options_(std::move(options)) {}

HelperServer::~HelperServer() = default;

void HelperServer::write_out(const std::string& line) {
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

void HelperServer::reply(const std::string& id, bool ok, const std::string& payload) {
    if (id == "-") return;
    write_out(id + "\t" + (ok ? "ok" : "err") + "\t" + escape_line(payload));
}

void HelperServer::emit(const std::string& event, const std::vector<std::string>& fields) {
    std::string line = "*\t" + event;
    for (const auto& f : fields) line += "\t" + escape_line(f);
    write_out(line);
}

bool HelperServer::path_allowed(const std::string& path) const {
    return !options_.base_dir.empty() && path.size() > options_.base_dir.size() && path[0] == '/' &&
           path_is_within(path, options_.base_dir);
}

bool HelperServer::is_active(const Container* c) const {
    return c && (c->status() == ContainerStatus::BOOTING || c->status() == ContainerStatus::RUNNING || c->status() == ContainerStatus::PAUSED);
}

Container* HelperServer::find(const std::string& instance) {
    auto it = handles_.find(instance);
    return it == handles_.end() ? nullptr : NamespaceManager::instance().get_container(it->second);
}

AgentLink* HelperServer::agent_for(const std::string& instance) {
    Container* c = find(instance);
    if (!is_active(c)) return nullptr;
    auto& slot = agents_[instance];
    if (!slot) slot = std::make_unique<AgentLink>(c->config().shared_dir + "/agent.sock");
    return slot.get();
}

int HelperServer::run() {
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
    for (auto& entry : handles_) {
        if (Container* c = NamespaceManager::instance().get_container(entry.second)) c->stop(3000);
    }
    NamespaceManager::instance().shutdown();
    return 0;
}

void HelperServer::tick() {
    NamespaceManager& mgr = NamespaceManager::instance();
    mgr.poll_all();
    for (auto& entry : handles_) {
        Container* c = mgr.get_container(entry.second);
        if (!c) continue;
        const std::string now = state_name(c->status());
        std::string& last = last_state_[entry.first];
        if (now != last) {
            last = now;
            emit("state", {entry.first, now, c->status() == ContainerStatus::ERROR ? c->last_error() : ""});
            if (!is_active(c)) agents_.erase(entry.first);
        }
    }
    for (size_t i = 0; i < jobs_.size();) {
        int st = 0;
        const pid_t w = waitpid(jobs_[i].pid, &st, WNOHANG);
        if (w == 0) {
            ++i;
            continue;
        }
        const Job job = jobs_[i];
        jobs_.erase(jobs_.begin() + (long)i);
        const bool ok = w == job.pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (job.resume_after) {
            if (Container* c = find(job.instance)) c->resume();
        }
        emit(job.kind, {job.instance, job.name, ok ? "ok" : "err", ok ? "" : "copy failed (out of space?)"});
    }
}

void HelperServer::handle_line(const std::string& line) {
    const auto fields = split_fields(line, '\t');
    if (fields.size() < 2) return;
    std::vector<std::string> args;
    for (size_t i = 2; i < fields.size(); ++i) args.push_back(unescape_line(fields[i]));
    dispatch(fields[0], fields[1], args);
}

void HelperServer::dispatch(const std::string& id, const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "hello") {
        reply(id, true, "proto=1\nuid=" + std::to_string(getuid()) + "\npid=" + std::to_string(getpid()) + "\nbase=" + options_.base_dir);
    } else if (cmd == "probe") {
        reply(id, true, diagnostics::capabilities_to_text(diagnostics::probe_host()));
    } else if (cmd == "start") {
        cmd_start(id, args);
    } else if (cmd == "stop") {
        cmd_stop(id, args, false);
    } else if (cmd == "kill") {
        cmd_stop(id, args, true);
    } else if (cmd == "pause" || cmd == "resume") {
        Container* c = args.empty() ? nullptr : find(args[0]);
        if (!c) return reply(id, false, "unknown instance");
        const bool ok = cmd == "pause" ? c->pause() : c->resume();
        reply(id, ok, ok ? "" : (c->can_pause() ? "wrong state" : "this device cannot freeze a guest"));
    } else if (cmd == "status") {
        cmd_status(id, args);
    } else if (cmd == "list") {
        std::string out;
        for (const auto& entry : handles_) {
            if (Container* c = NamespaceManager::instance().get_container(entry.second)) {
                out += entry.first + "=" + state_name(c->status()) + "\n";
            }
        }
        reply(id, true, out);
    } else if (cmd == "diag") {
        Container* c = args.empty() ? nullptr : find(args[0]);
        reply(id, c != nullptr, c ? c->diagnostics() : "unknown instance");
    } else if (cmd == "exec") {
        cmd_exec(id, args);
    } else if (cmd == "agent" || cmd == "agent_send") {
        cmd_agent(id, args);
    } else if (cmd == "apps") {
        cmd_apps(id, args);
    } else if (cmd == "launch") {
        cmd_launch(id, args);
    } else if (cmd == "icon") {
        cmd_icon(id, args);
    } else if (cmd == "snapshot") {
        cmd_snapshot(id, args, false);
    } else if (cmd == "restore") {
        cmd_snapshot(id, args, true);
    } else if (cmd == "snapshots") {
        cmd_list_snapshots(id, args);
    } else if (cmd == "delete_snapshot") {
        cmd_delete_snapshot(id, args);
    } else if (cmd == "rmtree") {
        cmd_rmtree(id, args);
    } else if (cmd == "quit") {
        reply(id, true, "");
        quit_ = true;
    } else {
        reply(id, false, "unknown command: " + cmd);
    }
}

void HelperServer::cmd_start(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 2) return reply(id, false, "usage: start <instance> <path> [key=value ...]");
    const std::string& instance = args[0];
    const std::string& path = args[1];
    if (!valid_name(instance, 64)) return reply(id, false, "invalid instance id");
    if (!path_allowed(path)) return reply(id, false, "instance path is outside the instances directory");

    NamespaceManager& mgr = NamespaceManager::instance();
    if (Container* old = find(instance)) {
        if (is_active(old)) return reply(id, false, "instance is already running");
        mgr.remove_container(handles_[instance]);
        handles_.erase(instance);
        last_state_.erase(instance);
    }

    ContainerConfig cfg = make_container_config(instance, path);
    cfg.owner_uid = options_.owner_uid;
    cfg.owner_gid = options_.owner_gid;
    cfg.qemu_arm_path = options_.qemu_arm_path;
    cfg.rooted = true;
    for (size_t i = 2; i < args.size(); ++i) {
        const size_t eq = args[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string k = args[i].substr(0, eq), v = args[i].substr(eq + 1);
        if (k == "ram_mb") cfg.ram_mb = to_int(v, cfg.ram_mb);
        else if (k == "cpu_cores") cfg.cpu_cores = to_int(v, cfg.cpu_cores);
        else if (k == "data_size_mb") cfg.data_size_mb = to_int(v, cfg.data_size_mb);
        else if (k == "width") cfg.display_width = to_int(v, cfg.display_width);
        else if (k == "height") cfg.display_height = to_int(v, cfg.display_height);
        else if (k == "dpi") cfg.display_dpi = to_int(v, cfg.display_dpi);
        else if (k == "network") cfg.network = v;
        else if (k == "qemu32") cfg.needs_qemu_32bit = v == "1";
        else if (k == "hardware") cfg.hardware = v;
        else if (k == "allow_unpatched") cfg.allow_unpatched_init = v == "1";
        else if (k == "boot_timeout") cfg.boot_timeout_s = to_int(v, cfg.boot_timeout_s);
    }
    if (cfg.display_width < 64 || cfg.display_height < 64 || cfg.display_width > 8192 || cfg.display_height > 8192) {
        return reply(id, false, "unreasonable display size");
    }
    if (cfg.network != "nat" && cfg.network != "none" && cfg.network != "host") cfg.network = "nat";

    const int64_t handle = mgr.start_container(cfg);
    if (handle == 0) {
        emit("state", {instance, "error", mgr.last_start_error()});
        last_state_[instance] = "error";
        return reply(id, false, mgr.last_start_error());
    }
    handles_[instance] = handle;
    last_state_[instance] = "booting";
    emit("state", {instance, "booting", ""});
    reply(id, true, "init_pid=" + std::to_string(mgr.get_container(handle)->init_pid()));
}

void HelperServer::cmd_stop(const std::string& id, const std::vector<std::string>& args, bool force) {
    Container* c = args.empty() ? nullptr : find(args[0]);
    if (!c) return reply(id, true, "not running");
    if (force) c->kill_now();
    else c->stop(args.size() > 1 ? std::max(500, to_int(args[1], 4000)) : 4000);
    tick();
    reply(id, true, state_name(c->status()));
}

void HelperServer::cmd_status(const std::string& id, const std::vector<std::string>& args) {
    Container* c = args.empty() ? nullptr : find(args[0]);
    if (!c) return reply(id, true, "state=stopped\ninit_pid=-1\nboot=0\ncan_pause=0\nerror=");
    c->poll();
    reply(id, true, std::string("state=") + state_name(c->status()) + "\ninit_pid=" + std::to_string(c->init_pid()) +
                        "\nboot=" + (c->boot_signal_seen() ? "1" : "0") + "\ncan_pause=" + (c->can_pause() ? "1" : "0") +
                        "\nerror=" + escape_line(c->last_error()));
}

void HelperServer::cmd_exec(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 3) return reply(id, false, "usage: exec <instance> <timeout_ms> <program> [args...]");
    Container* c = find(args[0]);
    if (!is_active(c)) return reply(id, false, "instance is not running");
    GuestExecResult r;
    const std::vector<std::string> argv(args.begin() + 2, args.end());
    guest_exec(c->init_pid(), argv, std::max(100, to_int(args[1], 10000)), &r);
    if (!r.error.empty()) return reply(id, false, r.error);
    reply(id, true, "exit=" + std::to_string(r.timed_out ? -1 : r.exit_code) + "\n" + r.output);
}

void HelperServer::cmd_agent(const std::string& id, const std::vector<std::string>& args) {
    // agent <instance> <timeout_ms> <line>, or agent_send <instance> <line> without a reply
    const bool send_only = id == "-";
    if (args.size() < (send_only ? 2u : 3u)) return reply(id, false, "usage: agent <instance> <timeout_ms> <line>");
    AgentLink* link = agent_for(args[0]);
    if (!link) return reply(id, false, "instance is not running");
    if (send_only) {
        link->send(args[1]);
        return;
    }
    std::string text, error;
    if (!link->request(args[2], &text, std::max(100, to_int(args[1], 5000)), &error)) return reply(id, false, error);
    reply(id, true, text);
}

void HelperServer::cmd_apps(const std::string& id, const std::vector<std::string>& args) {
    Container* c = args.empty() ? nullptr : find(args[0]);
    if (!is_active(c)) return reply(id, false, "instance is not running");
    std::string text, payload, error;
    AgentLink* link = agent_for(args[0]);
    if (link && link->request("apps", &text, 20000, &error) && agent_payload(text, &payload, &error)) {
        return reply(id, true, payload);
    }
    // Without the agent only package names are known
    GuestExecResult r;
    guest_exec(c->init_pid(), {"pm", "list", "packages", "-3"}, 30000, &r);
    if (!r.error.empty() || r.exit_code != 0) {
        return reply(id, false, r.error.empty() ? "pm failed: " + r.output.substr(0, 200) : r.error);
    }
    std::string out;
    for (const auto& line : split_lines(r.output)) {
        if (line.rfind("package:", 0) != 0) continue;
        const std::string pkg = line.substr(8);
        out += pkg + "\t" + pkg + "\n";
    }
    reply(id, true, out);
}

void HelperServer::cmd_launch(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 2) return reply(id, false, "usage: launch <instance> <package>");
    Container* c = find(args[0]);
    if (!is_active(c)) return reply(id, false, "instance is not running");
    for (char ch : args[1]) {
        if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '_')) return reply(id, false, "invalid package name");
    }
    std::string text, payload, error;
    AgentLink* link = agent_for(args[0]);
    if (link && link->request("launch " + args[1], &text, 10000, &error) && agent_payload(text, &payload, &error)) {
        return reply(id, true, "");
    }
    GuestExecResult r;
    guest_exec(c->init_pid(), {"monkey", "-p", args[1], "-c", "android.intent.category.LAUNCHER", "1"}, 15000, &r);
    if (!r.error.empty()) return reply(id, false, r.error);
    reply(id, r.exit_code == 0, r.exit_code == 0 ? "" : "could not launch " + args[1]);
}

void HelperServer::cmd_icon(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 2) return reply(id, false, "usage: icon <instance> <package>");
    AgentLink* link = agent_for(args[0]);
    if (!link) return reply(id, false, "instance is not running");
    std::string text, payload, error;
    if (!link->request("icon " + args[1], &text, 15000, &error) || !agent_payload(text, &payload, &error)) {
        return reply(id, false, error.empty() ? "no icon" : error);
    }
    reply(id, true, payload);
}

void HelperServer::cmd_snapshot(const std::string& id, const std::vector<std::string>& args, bool restore) {
    if (args.size() < 3) return reply(id, false, "usage: snapshot <instance> <path> <name>");
    const std::string& instance = args[0];
    const std::string& path = args[1];
    const std::string& name = args[2];
    if (!path_allowed(path) || !valid_name(name, 48)) return reply(id, false, "invalid path or snapshot name");
    for (const auto& j : jobs_) {
        if (j.instance == instance) return reply(id, false, "another snapshot operation is still running");
    }

    const std::string data = path + "/data.img";
    const std::string snap = path + "/snapshots/" + name + ".img";
    Container* c = find(instance);
    const bool active = is_active(c);
    if (!is_regular_file(data) && !restore) return reply(id, false, "this instance keeps /data in a folder, snapshots need the disk image");

    std::string src = restore ? snap : data;
    std::string dst = restore ? data : snap;
    if (restore && active) return reply(id, false, "stop the instance before restoring a snapshot");
    if (!is_regular_file(src)) return reply(id, false, restore ? "no such snapshot" : "nothing to snapshot yet");
    if (free_bytes(path) < allocated_bytes(src) + 64ULL * 1024 * 1024) return reply(id, false, "not enough free storage for the copy");

    Job job;
    job.kind = restore ? "restore" : "snapshot";
    job.instance = instance;
    job.name = name;
    if (active && !restore) {
        if (c->status() == ContainerStatus::PAUSED) {
            job.resume_after = false;
        } else if (c->pause()) {
            job.resume_after = true;
        } else {
            return reply(id, false, "stop the instance first, this device cannot freeze it while copying");
        }
    }
    mkdirs(path + "/snapshots");

    const pid_t pid = fork();
    if (pid < 0) {
        if (job.resume_after) c->resume();
        return reply(id, false, std::string("fork: ") + strerror(errno));
    }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
        }
        const std::string tmp = dst + ".tmp";
        bool ok = copy_file_sparse(src, tmp);
        if (ok) ok = rename(tmp.c_str(), dst.c_str()) == 0;
        if (!ok) unlink(tmp.c_str());
        _exit(ok ? 0 : 1);
    }
    job.pid = pid;
    jobs_.push_back(job);
    reply(id, true, "started");
}

void HelperServer::cmd_list_snapshots(const std::string& id, const std::vector<std::string>& args) {
    if (args.empty() || !path_allowed(args[0])) return reply(id, false, "invalid path");
    const std::string dir = args[0] + "/snapshots";
    std::string out;
    if (DIR* d = opendir(dir.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".img") == 0) names.push_back(n.substr(0, n.size() - 4));
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            struct stat st{};
            if (stat((dir + "/" + n + ".img").c_str(), &st) != 0) continue;
            out += n + "\t" + std::to_string((long long)st.st_size) + "\t" + std::to_string((long long)st.st_blocks * 512) + "\t" +
                   std::to_string((long long)st.st_mtime) + "\n";
        }
    }
    reply(id, true, out);
}

void HelperServer::cmd_delete_snapshot(const std::string& id, const std::vector<std::string>& args) {
    if (args.size() < 2 || !path_allowed(args[0]) || !valid_name(args[1], 48)) return reply(id, false, "invalid path or snapshot name");
    unlink((args[0] + "/snapshots/" + args[1] + ".img").c_str());
    reply(id, true, "");
}

void HelperServer::cmd_rmtree(const std::string& id, const std::vector<std::string>& args) {
    if (args.empty() || !path_allowed(args[0])) return reply(id, false, "path is outside the instances directory");
    for (auto it = handles_.begin(); it != handles_.end();) {
        Container* c = NamespaceManager::instance().get_container(it->second);
        const bool inside = c && path_is_within(c->config().instance_path, args[0]);
        if (inside && is_active(c)) return reply(id, false, "instance is still running");
        if (inside) {
            NamespaceManager::instance().remove_container(it->second);
            last_state_.erase(it->first);
            it = handles_.erase(it);
        } else {
            ++it;
        }
    }
    struct stat st{};
    if (lstat(args[0].c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return reply(id, false, "refusing to delete a symlink");
    const bool ok = remove_recursive(args[0]);
    reply(id, ok, ok ? "" : "could not delete everything");
}

int helper_main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    HelperOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--base" && has_value) opts.base_dir = argv[++i];
        else if (a == "--qemu" && has_value) opts.qemu_arm_path = argv[++i];
        else if (a == "--uid" && has_value) opts.owner_uid = atoi(argv[++i]);
        else if (a == "--gid" && has_value) opts.owner_gid = atoi(argv[++i]);
    }
    if (opts.owner_gid < 0) opts.owner_gid = opts.owner_uid;
    if (geteuid() != 0) {
        fprintf(stderr, "vine_helper must run as root\n");
        return 3;
    }
    if (opts.base_dir.empty()) {
        fprintf(stderr, "usage: vine_helper --base <instances dir> [--uid N] [--gid N] [--qemu path]\n");
        return 2;
    }
    umask(022);
    std::string self = argc > 0 ? argv[0] : "";
    const size_t slash = self.rfind('/');
    NamespaceManager::instance().init(opts.base_dir, slash == std::string::npos ? "" : self.substr(0, slash));
    HelperServer server(STDIN_FILENO, STDOUT_FILENO, opts);
    return server.run();
}

} // namespace vine
