// vine_wl: the Wayland server for a rootless guest's hwcomposer, as its
// own process. Frames go to a vinefb file, which the app displays.
//
//   vine_wl --socket <dir>/wayland-0 --vinefb <file> [--create WxH]
//
// --create makes the vinefb file first (dev loop; the app normally
// creates it with the instance's display size).
#include "vinefb_sink.h"
#include "wl_server.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

vine::wl::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server) g_server->stop();
}

int usage() {
    fprintf(stderr, "usage: vine_wl --socket PATH --vinefb PATH [--create WxH]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string socket_path, vinefb_path;
    unsigned w = 0, h = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) return usage();
        if (a == "--socket") socket_path = argv[++i];
        else if (a == "--vinefb") vinefb_path = argv[++i];
        else if (a == "--create") {
            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h) return usage();
        } else return usage();
    }
    if (socket_path.empty() || vinefb_path.empty()) return usage();

    std::string error;
    if (w && !vine::wl::create_vinefb(vinefb_path, w, h, &error)) {
        fprintf(stderr, "vine_wl: %s\n", error.c_str());
        return 1;
    }
    vine::wl::VineFbSink sink;
    if (!sink.open(vinefb_path)) {
        fprintf(stderr, "vine_wl: %s is not a vinefb file\n", vinefb_path.c_str());
        return 1;
    }
    vine::wl::Server server(&sink, [](const std::string& m) { fprintf(stderr, "vine_wl: %s\n", m.c_str()); });
    if (!server.listen(socket_path, &error)) {
        fprintf(stderr, "vine_wl: %s\n", error.c_str());
        return 1;
    }
    g_server = &server;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "vine_wl: serving %ux%u on %s\n", sink.width(), sink.height(), socket_path.c_str());
    const bool ok = server.run();
    fprintf(stderr, "vine_wl: stopped after %llu frames\n", (unsigned long long)server.frames());
    return ok ? 0 : 1;
}
