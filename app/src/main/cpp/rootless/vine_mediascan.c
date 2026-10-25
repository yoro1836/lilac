/* vine_mediascan: tells MediaProvider about files apps write to shared
 * storage in a rootless guest.
 *
 * Without FUSE (see vold_fuse_shim.c) apps write straight into the lower
 * filesystem, so MediaProvider never hears of new, changed or deleted
 * files and galleries and pickers miss them until the next full scan.
 * This watches the lower directory with inotify and, once things settle,
 * asks MediaProvider to scan what changed (`content call ... scan_file`,
 * which takes a file or a directory). Android/ is left out: app-private
 * files are not media.
 *
 *   vine_mediascan LOWER UPPER     e.g. /data/media/0 /storage/emulated/0
 *   VINE_MEDIASCAN_CMD=PROG        runs PROG UPPER_PATH instead of content (tests)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WATCHES 8192
#define MAX_PENDING 64
#define QUIET_MS 1500   /* scan once nothing changed for this long */
#define MAX_WAIT_MS 6000 /* or this long after the first change */
#define WATCH_MASK (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_DELETE_SELF)

static const char *lower, *upper;
static size_t lower_len;
static int ifd;
static char *watched[MAX_WATCHES]; /* by watch descriptor */
static char pending[MAX_PENDING][PATH_MAX];
static int npending;
static int overflow; /* too much changed: scan the whole volume */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* App-private Android/ is not media */
static int skipped(const char *path)
{
    const char *rel = path + lower_len;
    return !strcmp(rel, "/Android") || !strncmp(rel, "/Android/", 9);
}

static void watch_tree(const char *dir)
{
    if (skipped(dir)) return;
    int wd = inotify_add_watch(ifd, dir, WATCH_MASK | IN_ONLYDIR);
    if (wd < 0 || wd >= MAX_WATCHES) return;
    free(watched[wd]);
    watched[wd] = strdup(dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char sub[PATH_MAX];
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR || !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) < (int)sizeof sub) watch_tree(sub);
    }
    closedir(d);
}

static void add_pending(const char *path)
{
    for (int i = 0; i < npending; ++i)
        if (!strcmp(pending[i], path)) return;
    if (npending == MAX_PENDING) {
        overflow = 1;
        return;
    }
    snprintf(pending[npending++], PATH_MAX, "%s", path);
}

static void run_scan(const char *method, const char *arg)
{
    const pid_t pid = fork();
    if (pid == 0) {
        const char *cmd = getenv("VINE_MEDIASCAN_CMD");
        if (cmd) execl(cmd, cmd, method, arg, (char *)NULL);
        else execl("/system/bin/content", "content", "call", "--uri", "content://media", "--method", method, "--arg", arg,
                   (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

static void flush(void)
{
    if (overflow) {
        run_scan("scan_volume", "external_primary");
    } else {
        char path[PATH_MAX];
        for (int i = 0; i < npending; ++i)
            if (snprintf(path, sizeof path, "%s%s", upper, pending[i] + lower_len) < (int)sizeof path)
                run_scan("scan_file", path);
    }
    npending = 0;
    overflow = 0;
}

static void handle(const struct inotify_event *ev)
{
    if (ev->wd < 0 || ev->wd >= MAX_WATCHES || !watched[ev->wd]) return;
    if (ev->mask & (IN_DELETE_SELF | IN_IGNORED)) {
        free(watched[ev->wd]);
        watched[ev->wd] = NULL;
        return;
    }
    if (!ev->len) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", watched[ev->wd], ev->name) >= (int)sizeof path || skipped(path)) return;
    if (ev->mask & IN_ISDIR) {
        if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
            watch_tree(path);
            add_pending(path); /* a moved-in tree may hold files already */
        } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
            add_pending(watched[ev->wd]);
        }
        return;
    }
    if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) add_pending(watched[ev->wd]); /* the scan drops what is gone */
    else if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) add_pending(path);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: vine_mediascan LOWER UPPER\n");
        return 2;
    }
    lower = argv[1];
    upper = argv[2];
    lower_len = strlen(lower);
    ifd = inotify_init1(IN_CLOEXEC);
    if (ifd < 0) {
        perror("vine_mediascan: inotify");
        return 1;
    }
    watch_tree(lower);
    long long first = 0, last = 0;
    char buf[16384] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        int timeout = -1;
        if (npending || overflow) {
            const long long t = now_ms();
            long long due = last + QUIET_MS;
            if (due > first + MAX_WAIT_MS) due = first + MAX_WAIT_MS;
            timeout = due > t ? (int)(due - t) : 0;
        }
        struct pollfd p = { .fd = ifd, .events = POLLIN };
        const int r = poll(&p, 1, timeout);
        if (r < 0 && errno != EINTR) return 1;
        if (r > 0) {
            const ssize_t n = read(ifd, buf, sizeof buf);
            for (char *at = buf; n > 0 && at < buf + n;) {
                const struct inotify_event *ev = (const struct inotify_event *)at;
                if (ev->mask & IN_Q_OVERFLOW) overflow = 1;
                else handle(ev);
                at += sizeof *ev + ev->len;
            }
            if (npending || overflow) {
                last = now_ms();
                if (!first) first = last;
            }
            continue;
        }
        if (npending || overflow) {
            flush();
            first = last = 0;
        }
    }
}
