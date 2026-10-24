// vine_pulse: the PulseAudio server for a rootless guest's audio HAL, as
// its own process. Audio plays and records through AAudio.
//
//   vine_pulse --socket <dir>/pulse/native [--clock]
//
// --clock: no host audio (played audio is dropped, capture is silence).
#include "aaudio_backend.h"
#include "pa_server.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

vine::pa::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server) g_server->stop();
}

int usage() {
    fprintf(stderr, "usage: vine_pulse --socket PATH [--clock]\n");
    return 2;
}

void log_line(const std::string& m) { fprintf(stderr, "vine_pulse: %s\n", m.c_str()); }

} // namespace

int main(int argc, char** argv) {
    std::string socket_path;
    bool clock = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) socket_path = argv[++i];
        else if (a == "--clock") clock = true;
        else return usage();
    }
    if (socket_path.empty()) return usage();

    vine::pa::ClockBackend clock_backend;
    vine::pa::AAudioBackend aaudio_backend(log_line);
    vine::pa::Backend* backend = clock ? static_cast<vine::pa::Backend*>(&clock_backend) : &aaudio_backend;
    vine::pa::Server server(backend, log_line);
    std::string error;
    if (!server.listen(socket_path, &error)) {
        log_line(error);
        return 1;
    }
    g_server = &server;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    log_line("serving on " + socket_path);
    return server.run() ? 0 : 1;
}
