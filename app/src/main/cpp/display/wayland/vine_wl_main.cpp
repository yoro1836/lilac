// vine_wl: the Wayland server for a rootless guest's hwcomposer, as its
// own process. Frames go to a vinefb file, which the app displays.
//
//   vine_wl --socket <dir>/wayland-0 --vinefb <file> [--create WxH] [--input PATH] [--refresh HZ]
//
// --create makes the vinefb file first (dev loop; the app normally
// creates it with the instance's display size). --input listens for the
// guest agent's touch/key/text lines (input_lines.h). --refresh is the
// output's refresh rate (60 by default), the guest's frame rate.
//
//   vine_wl --send PATH LINE...
//
// writes LINEs to such an input socket (dev loop).
#include "vinefb_sink.h"
#include "wl_server.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

vine::wl::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server) g_server->stop();
}

int usage() {
    fprintf(stderr, "usage: vine_wl --socket PATH --vinefb PATH [--create WxH] [--input PATH] [--refresh HZ]\n"
                    "       vine_wl --send PATH LINE...\n");
    return 2;
}

int send_lines(const char* path, int n, char** lines) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) return usage();
    strcpy(sa.sun_path, path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        fprintf(stderr, "vine_wl: connect %s: %s\n", path, strerror(errno));
        return 1;
    }
    std::string text;
    for (int i = 0; i < n; ++i) text += std::string(lines[i]) + "\n";
    const bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    close(fd);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--send")) return send_lines(argv[2], argc - 3, argv + 3);
    std::string socket_path, vinefb_path, input_path;
    unsigned w = 0, h = 0, refresh = 60;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) return usage();
        if (a == "--socket") socket_path = argv[++i];
        else if (a == "--vinefb") vinefb_path = argv[++i];
        else if (a == "--input") input_path = argv[++i];
        else if (a == "--refresh") {
            if (sscanf(argv[++i], "%u", &refresh) != 1 || refresh < 1 || refresh > 240) return usage();
        } else if (a == "--create") {
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
    server.set_refresh_mhz(refresh * 1000);
    if (!server.listen(socket_path, &error)) {
        fprintf(stderr, "vine_wl: %s\n", error.c_str());
        return 1;
    }
    if (!input_path.empty() && !server.listen_input(input_path, &error)) {
        fprintf(stderr, "vine_wl: %s\n", error.c_str());
        return 1;
    }
    g_server = &server;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "vine_wl: serving %ux%u at %u Hz on %s\n", sink.width(), sink.height(), refresh, socket_path.c_str());
    const bool ok = server.run();
    fprintf(stderr, "vine_wl: stopped after %llu frames\n", (unsigned long long)server.frames());
    return ok ? 0 : 1;
}
