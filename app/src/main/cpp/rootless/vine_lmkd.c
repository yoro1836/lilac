/* vine_lmkd: Android's lmkd for a rootless guest.
 *
 * The real lmkd needs PSI or memcg, both closed to an app uid. Without an
 * lmkd every oom-adj update stalls system_server ~3 s, and without kills
 * the guest keeps every app it ever started until the host's lmkd kills
 * the whole app, guest and all. So this takes system_server's process
 * priorities on /dev/socket/lmkd and, once a second, kills the least
 * important guest app while
 *   - the apps' private memory is over the instance's budget (cached
 *     apps only), or
 *   - the host runs low on memory (cached apps, then background ones).
 * Protocol: system/memory/lmkd/include/lmkd.h (Android 13).
 *
 *   vine_lmkd [--budget-mb N]        the socket comes from init
 *   vine_lmkd [--budget-mb N] PATH   listens on PATH itself (tests)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    LMK_PROCPRIO = 1,
    LMK_PROCREMOVE = 2,
    LMK_PROCPURGE = 3,
    LMK_GETKILLCNT = 4,
    LMK_SUBSCRIBE = 5,
    LMK_PROCKILL = 6,
    LMK_UPDATE_PROPS = 7,
};
#define MAX_CLIENTS 8
#define MAX_PROCS 2048

enum {
    ADJ_CACHED = 900,     /* CACHED_APP_MIN_ADJ */
    ADJ_PREVIOUS = 700,   /* PREVIOUS_APP_ADJ: the last app, services, cached */
    TICK_MS = 1000,
    PRESSED_TICK_MS = 250, /* after a kill, look again soon */
};

struct proc {
    int32_t pid, uid, adj;
};

static struct proc procs[MAX_PROCS];
static int nprocs;
static int kill_count;
static long page_kb;

static struct proc *find(int32_t pid)
{
    for (int i = 0; i < nprocs; ++i)
        if (procs[i].pid == pid) return &procs[i];
    return NULL;
}

static void forget(int32_t pid)
{
    struct proc *p = find(pid);
    if (p) *p = procs[--nprocs];
}

/* Resident memory not shared with other processes, in KB; -1 if gone */
static long private_kb(int32_t pid)
{
    char path[32], buf[128];
    snprintf(path, sizeof path, "/proc/%d/statm", (int)pid);
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    long size = 0, resident = 0, shared = 0;
    int ok = fgets(buf, sizeof buf, f) && sscanf(buf, "%ld %ld %ld", &size, &resident, &shared) == 3;
    fclose(f);
    if (!ok) return -1;
    return resident > shared ? (resident - shared) * page_kb : 0;
}

/* MemAvailable and MemTotal in KB, 0 if unknown */
static void host_memory(long *avail, long *total)
{
    char line[128];
    *avail = *total = 0;
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "MemTotal: %ld", total);
        sscanf(line, "MemAvailable: %ld", avail);
    }
    fclose(f);
}

/* The lowest adj that may be killed now, or INT32_MAX for none */
static int32_t kill_floor(long budget_kb, long used_kb, long avail, long total)
{
    if (total > 0 && avail > 0) {
        if (avail < total / 16) return ADJ_PREVIOUS;
        if (avail < total / 8) return ADJ_CACHED;
    }
    if (budget_kb > 0 && used_kb > budget_kb) return ADJ_CACHED;
    return INT32_MAX;
}

static void send_ints(int fd, const int32_t *v, int n)
{
    int32_t out[4];
    for (int i = 0; i < n; ++i) out[i] = (int32_t)htonl((uint32_t)v[i]);
    (void)send(fd, out, (size_t)n * sizeof(int32_t), MSG_NOSIGNAL);
}

/* Kills one app if memory is short; true if it did */
static int check_memory(long budget_kb, const int *subscribers, const struct pollfd *pfd, int n)
{
    long used = 0;
    for (int i = 0; i < nprocs;) {
        long kb = private_kb(procs[i].pid);
        if (kb < 0) {
            procs[i] = procs[--nprocs]; /* died without a PROCREMOVE yet */
            continue;
        }
        used += kb;
        ++i;
    }
    long avail, total;
    host_memory(&avail, &total);
    const int32_t floor = kill_floor(budget_kb, used, avail, total);
    struct proc *victim = NULL;
    long victim_kb = 0;
    for (int i = 0; i < nprocs; ++i) {
        struct proc *p = &procs[i];
        if (p->adj < floor) continue;
        long kb = private_kb(p->pid);
        if (!victim || p->adj > victim->adj || (p->adj == victim->adj && kb > victim_kb)) {
            victim = p;
            victim_kb = kb;
        }
    }
    if (!victim || kill(victim->pid, SIGKILL) != 0) return 0;
    fprintf(stderr, "vine_lmkd: killed pid %d (adj %d, %ld KB): apps %ld KB of %ld, host %ld of %ld KB free\n",
            (int)victim->pid, (int)victim->adj, victim_kb, used, budget_kb, avail, total);
    ++kill_count;
    const int32_t note[3] = { LMK_PROCKILL, victim->pid, victim->uid };
    for (int i = 1; i < n; ++i)
        if (subscribers[i]) send_ints(pfd[i].fd, note, 3);
    forget(victim->pid);
    return 1;
}

static void handle(int fd, const int32_t *req, size_t len, int *subscribed)
{
    if (len < sizeof(int32_t)) return;
    int32_t v[8];
    size_t cnt = len / sizeof(int32_t);
    if (cnt > 8) cnt = 8;
    for (size_t i = 0; i < cnt; ++i) v[i] = (int32_t)ntohl((uint32_t)req[i]);
    switch (v[0]) {
    case LMK_PROCPRIO:
        if (cnt >= 4) {
            struct proc *p = find(v[1]);
            if (!p && nprocs < MAX_PROCS) p = &procs[nprocs++];
            if (p) *p = (struct proc){ v[1], v[2], v[3] };
        }
        break;
    case LMK_PROCREMOVE:
        if (cnt >= 2) forget(v[1]);
        break;
    case LMK_PROCPURGE:
        nprocs = 0;
        break;
    case LMK_SUBSCRIBE:
        *subscribed = 1;
        break;
    case LMK_GETKILLCNT: {
        const int32_t r[2] = { LMK_GETKILLCNT, kill_count };
        send_ints(fd, r, 2);
        break;
    }
    case LMK_UPDATE_PROPS: {
        const int32_t r[2] = { LMK_UPDATE_PROPS, 0 };
        send_ints(fd, r, 2);
        break;
    }
    default: break; /* LMK_TARGET and the rest */
    }
}

static int open_socket(const char *path)
{
    if (path) {
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        if (strlen(path) >= sizeof sa.sun_path) return -1;
        strcpy(sa.sun_path, path);
        unlink(path);
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) return -1;
        return fd;
    }
    const char *env = getenv("ANDROID_SOCKET_lmkd");
    return env ? atoi(env) : -1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    long budget_kb = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--budget-mb") && i + 1 < argc) budget_kb = atol(argv[++i]) * 1024;
        else path = argv[i];
    }
    page_kb = sysconf(_SC_PAGESIZE) / 1024;
    int lfd = open_socket(path);
    if (lfd < 0 || listen(lfd, MAX_CLIENTS) != 0) {
        fprintf(stderr, "vine_lmkd: no control socket\n");
        return 1;
    }
    struct pollfd pfd[1 + MAX_CLIENTS];
    int subscribed[1 + MAX_CLIENTS] = { 0 };
    int n = 1;
    pfd[0] = (struct pollfd){ .fd = lfd, .events = POLLIN };
    int tick = TICK_MS;
    int64_t next = now_ms() + tick;
    for (;;) {
        int64_t wait = next - now_ms();
        if (poll(pfd, (nfds_t)n, wait > 0 ? (int)wait : 0) < 0 && errno != EINTR) return 1;
        if ((pfd[0].revents & POLLIN) && n < 1 + MAX_CLIENTS) {
            int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                subscribed[n] = 0;
                pfd[n++] = (struct pollfd){ .fd = c, .events = POLLIN };
            }
        }
        for (int i = 1; i < n; ++i) {
            if (!pfd[i].revents) continue;
            int32_t req[16];
            ssize_t got = recv(pfd[i].fd, req, sizeof req, 0);
            if (got <= 0) {
                close(pfd[i].fd);
                subscribed[i] = subscribed[n - 1];
                pfd[i--] = pfd[--n];
                continue;
            }
            handle(pfd[i].fd, req, (size_t)got, &subscribed[i]);
        }
        if (now_ms() >= next) {
            tick = check_memory(budget_kb, subscribed, pfd, n) ? PRESSED_TICK_MS : TICK_MS;
            next = now_ms() + tick;
        }
    }
}
