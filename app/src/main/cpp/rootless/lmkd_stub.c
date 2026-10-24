/* vine_lmkd: stands in for Android's lmkd inside a rootless guest.
 *
 * The real lmkd needs PSI or memcg, both closed to an app uid, and the
 * host's own lmkd already manages the app as a whole. system_server still
 * expects an lmkd on /dev/socket/lmkd: without one every oom-adj update
 * waits ~3 s for a connection, which stalls the whole framework. This
 * accepts the connection and answers the two requests that expect a
 * reply (kill count: 0, property reload: ok); the rest is accepted and
 * ignored. Protocol: system/memory/lmkd/include/lmkd.h (Android 13).
 *
 *   vine_lmkd            the socket comes from init (ANDROID_SOCKET_lmkd)
 *   vine_lmkd PATH       listens on PATH itself (tests)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum { LMK_GETKILLCNT = 4, LMK_UPDATE_PROPS = 7 };
#define MAX_CLIENTS 8

static int open_socket(int argc, char **argv)
{
    if (argc > 1) {
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        if (strlen(argv[1]) >= sizeof sa.sun_path) return -1;
        strcpy(sa.sun_path, argv[1]);
        unlink(argv[1]);
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) return -1;
        return fd;
    }
    const char *env = getenv("ANDROID_SOCKET_lmkd");
    return env ? atoi(env) : -1;
}

/* Reply for one request, or 0 bytes */
static size_t reply_for(const int32_t *req, size_t len, int32_t out[2])
{
    if (len < sizeof(int32_t)) return 0;
    int32_t cmd = (int32_t)ntohl((uint32_t)req[0]);
    if (cmd != LMK_GETKILLCNT && cmd != LMK_UPDATE_PROPS) return 0;
    out[0] = (int32_t)htonl((uint32_t)cmd);
    out[1] = 0;
    return 2 * sizeof(int32_t);
}

int main(int argc, char **argv)
{
    int lfd = open_socket(argc, argv);
    if (lfd < 0 || listen(lfd, MAX_CLIENTS) != 0) {
        fprintf(stderr, "vine_lmkd: no control socket\n");
        return 1;
    }
    struct pollfd pfd[1 + MAX_CLIENTS];
    int n = 1;
    pfd[0] = (struct pollfd){ .fd = lfd, .events = POLLIN };
    for (;;) {
        if (poll(pfd, (nfds_t)n, -1) < 0) continue;
        if ((pfd[0].revents & POLLIN) && n < 1 + MAX_CLIENTS) {
            int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) pfd[n++] = (struct pollfd){ .fd = c, .events = POLLIN };
        }
        for (int i = 1; i < n; ++i) {
            if (!pfd[i].revents) continue;
            int32_t req[16], rep[2];
            ssize_t got = recv(pfd[i].fd, req, sizeof req, 0);
            if (got <= 0) {
                close(pfd[i].fd);
                pfd[i--] = pfd[--n];
                continue;
            }
            size_t r = reply_for(req, (size_t)got, rep);
            if (r) (void)send(pfd[i].fd, rep, r, MSG_NOSIGNAL);
        }
    }
}
