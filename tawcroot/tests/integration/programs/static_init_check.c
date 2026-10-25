/* `tawcroot --init`: the first guest process is a fake pid 1. Checks
 * getpid/getppid on both sides, orphan adoption (subreaper), and that
 * init only receives signals it has a handler for, as a real namespace
 * init does. With a host pid as argv[1], also checks that the guest
 * can neither see nor signal it, kill(-1) included. With `caps nspawn`
 * or `caps all`, checks the fake capability set instead; with `rlimit`,
 * that raising a hard limit past the host's clamps instead of failing;
 * with `ids`, that /proc/<pid>/status shows virtual Uid/Gid lines and
 * SO_PEERCRED shows init as pid 1; with `abstract NAME` (an abstract
 * socket the host listens on), that the session has its own abstract
 * namespace.
 *
 * Exit 42 on success; every other code names the failed step. pids go
 * through raw syscalls so libc caching can't hide the translation. */

#define _GNU_SOURCE
#include <dirent.h>
#include <linux/capability.h>
#include <stddef.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static long raw_getpid(void) { return syscall(SYS_getpid); }
static long raw_getppid(void) { return syscall(SYS_getppid); }

static volatile sig_atomic_t got_usr1;
static void on_usr1(int sig) { (void)sig; got_usr1 = 1; }

/* Fork, run `fn` in the child and return its exit status (-1 on error). */
static int in_child(int (*fn)(void))
{
	pid_t c = fork();
	if (c < 0) return -1;
	if (c == 0) _exit(fn());
	int status = 0;
	if (waitpid(c, &status, 0) != c || !WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}

static int child_ids(void)
{
	if (raw_getppid() != 1) return 1;
	if (raw_getpid() == 1) return 2;
	return 0;
}

static int child_signals_init(void)
{
	if (kill(1, 0) != 0) return 1;
	if (kill(1, SIGTERM) != 0) return 2;
	if (kill(1, SIGKILL) != 0) return 3;
	if (syscall(SYS_tgkill, 1, 1, SIGTERM) != 0) return 4;
	if (kill(1, SIGUSR1) != 0) return 5;
	return 0;
}

static int listed(long pid)
{
	DIR *d = opendir("/proc");
	if (!d) return -1;
	int found = 0;
	for (struct dirent *e; (e = readdir(d));)
		if (atol(e->d_name) == pid) found = 1;
	closedir(d);
	return found;
}

/* The host process `host` stays out of reach; kill(-1) still reaches
 * the guest's own processes. */
static int isolation(long host)
{
	if (kill((pid_t)host, 0) == 0 || errno != ESRCH) return 1;
	if (syscall(SYS_pidfd_open, host, 0) >= 0 || errno != ESRCH) return 2;
	if (syscall(SYS_tgkill, host, host, 0) == 0 || errno != ESRCH) return 3;
	if (listed(host) != 0) return 4;

	pid_t c = fork();
	if (c < 0) return 5;
	if (c == 0) { for (;;) pause(); }
	if (listed(c) != 1) return 6;
	if (kill(-1, SIGTERM) != 0) return 7;
	int status = 0;
	if (waitpid(c, &status, 0) != c || !WIFSIGNALED(status) ||
	    WTERMSIG(status) != SIGTERM) return 8;
	return 0;
}

/* What Android init does before exec'ing a service with capabilities:
 * keep only CAP_SYSLOG + CAP_SETPCAP. nspawn's set has no CAP_SYSLOG. */
static int caps(int all)
{
	struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct d[2];
	if (syscall(SYS_capget, &h, d) != 0) return 1;
	if (all ? d[0].permitted != 0xffffffffu || d[1].permitted != 0x1ff
		: d[0].permitted != 0xfdecafffu || d[1].permitted != 0) return 2;
	if (prctl(PR_CAPBSET_READ, CAP_SYSLOG) != all) return 3;
	memset(d, 0, sizeof d);
	d[0].permitted = d[0].effective = 1u << CAP_SETPCAP;
	d[1].permitted = d[1].inheritable = 1u << (CAP_SYSLOG - 32);
	long r = syscall(SYS_capset, &h, d);
	if (all ? r != 0 : r != -1 || errno != EPERM) return 4;
	/* Shadow securebits: keepcaps, then its lock (as Android's init). */
	if (prctl(PR_SET_KEEPCAPS, 1) != 0 || prctl(PR_GET_KEEPCAPS) != 1) return 5;
	if (prctl(PR_GET_SECUREBITS) != 1 << 4) return 6;
	if (prctl(PR_SET_SECUREBITS, (1 << 4) | (1 << 5)) != 0) return 7;
	if (prctl(PR_SET_KEEPCAPS, 0) != -1 || errno != EPERM) return 8;
	if (prctl(PR_SET_SECUREBITS, 1 << 4) != -1 || errno != EPERM) return 9;
	return 0;
}

static int rlimit(void)
{
	struct rlimit old, big = { RLIM_INFINITY, RLIM_INFINITY }, now;
	if (getrlimit(RLIMIT_MEMLOCK, &old) != 0) return 1;
	if (setrlimit(RLIMIT_MEMLOCK, &big) != 0) return 2;
	if (getrlimit(RLIMIT_MEMLOCK, &now) != 0) return 3;
	if (now.rlim_max < old.rlim_max || now.rlim_cur != now.rlim_max) return 4;
	big.rlim_cur = RLIM_INFINITY; big.rlim_max = 0;
	if (setrlimit(RLIMIT_MEMLOCK, &big) == 0 || errno != EINVAL) return 5;
	return 0;
}

/* First number of `key` in /proc/<pid>/status (pid 0: self), or -1. */
static long status_id(long pid, const char *key)
{
	char path[64], buf[4096];
	if (pid) snprintf(path, sizeof path, "/proc/%ld/status", pid);
	else snprintf(path, sizeof path, "/proc/self/status");
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	long n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	char *line = strstr(buf, key);
	return line ? strtol(line + strlen(key), NULL, 10) : -1;
}

static int ids(void)
{
	if (status_id(0, "\nUid:") != 0 || status_id(0, "\nGid:") != 0) return 1;
	int go[2], done[2], sv[2];
	if (pipe(go) || pipe(done) || socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 2;
	pid_t c = fork();
	if (c < 0) return 3;
	if (c == 0) {
		char b = 0;
		struct ucred cr;
		socklen_t len = sizeof cr;
		if (getsockopt(sv[1], SOL_SOCKET, SO_PEERCRED, &cr, &len) || cr.pid != 1) _exit(4);
		if (setresgid(1001, 1001, 1001) || setresuid(1000, 1000, 1000)) _exit(1);
		if (status_id(0, "\nUid:") != 1000) _exit(2);
		if (write(go[1], &b, 1) != 1 || read(done[0], &b, 1) != 1) _exit(3);
		_exit(0);
	}
	close(go[1]);
	close(done[0]);
	char b = 0;
	int e = 0;
	if (read(go[0], &b, 1) != 1) e = 4;
	else if (status_id(c, "\nUid:") != 1000) e = 5;
	else if (status_id(c, "\nGid:") != 1001) e = 6;
	if (write(done[1], &b, 1) != 1 && !e) e = 7;
	int status = 0;
	if (waitpid(c, &status, 0) != c || !WIFEXITED(status)) return 8;
	return e ? e : WEXITSTATUS(status) ? 10 + WEXITSTATUS(status) : 0;
}

static socklen_t abstract_addr(struct sockaddr_un *sa, const char *name)
{
	memset(sa, 0, sizeof *sa);
	sa->sun_family = AF_UNIX;
	size_t n = strlen(name);
	memcpy(sa->sun_path + 1, name, n);
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static int abstract(const char *name)
{
	struct sockaddr_un sa, got;
	socklen_t len = abstract_addr(&sa, name), glen = sizeof got;
	int c = socket(AF_UNIX, SOCK_STREAM, 0);
	if (c < 0) return 1;
	if (connect(c, (struct sockaddr *)&sa, len) == 0) return 2;  /* the host's */
	close(c);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0 || bind(s, (struct sockaddr *)&sa, len) || listen(s, 1)) return 3;
	if (getsockname(s, (struct sockaddr *)&got, &glen) || glen != len || memcmp(&got, &sa, len)) return 4;
	c = socket(AF_UNIX, SOCK_STREAM, 0);
	if (c < 0 || connect(c, (struct sockaddr *)&sa, len)) return 5;
	glen = sizeof got;
	if (getpeername(c, (struct sockaddr *)&got, &glen) || glen != len || memcmp(&got, &sa, len)) return 6;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "abstract")) {
		int e = abstract(argv[2]);
		return e ? 90 + e : 42;
	}
	if (argc > 1 && !strcmp(argv[1], "ids")) {
		int e = ids();
		return e ? 80 + e : 42;
	}
	if (argc > 1 && !strcmp(argv[1], "rlimit")) {
		int e = rlimit();
		return e ? 70 + e : 42;
	}
	if (argc > 2 && !strcmp(argv[1], "caps")) {
		int e = caps(!strcmp(argv[2], "all"));
		return e ? 60 + e : 42;
	}
	if (raw_getpid() != 1) return 1;
	if (raw_getppid() != 0) return 2;
	if (in_child(child_ids) != 0) return 3;

	/* A grandchild orphaned by its parent's exit reparents to us. */
	int p[2];
	if (pipe(p) != 0) return 10;
	pid_t c = fork();
	if (c < 0) return 11;
	if (c == 0) {
		if (fork() == 0) {
			for (int i = 0; i < 500 && raw_getppid() != 1; i++)
				usleep(10000);
			char ok = raw_getppid() == 1 ? 'o' : 'x';
			(void)!write(p[1], &ok, 1);
			_exit(7);
		}
		_exit(0);
	}
	int status = 0;
	if (waitpid(c, &status, 0) != c) return 12;
	char ok = 0;
	if (read(p[0], &ok, 1) != 1 || ok != 'o') return 13;
	pid_t orphan = wait(&status);
	if (orphan <= 0 || orphan == c) return 14;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) return 15;

	/* SIGTERM/SIGKILL with no handler are dropped; SIGUSR1 has one. */
	struct sigaction sa = { .sa_handler = on_usr1, .sa_flags = SA_RESTART };
	if (sigaction(SIGUSR1, &sa, 0) != 0) return 20;
	int r = in_child(child_signals_init);
	if (r != 0) return 20 + r;
	for (int i = 0; i < 500 && !got_usr1; i++) usleep(10000);
	if (!got_usr1) return 30;
	if (argc > 1) {
		int e = isolation(atol(argv[1]));
		if (e) return 40 + e;
	}
	return 42;
}
