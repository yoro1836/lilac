/* Prod-env audit probe: which x86_64 *legacy* syscall numbers does the
 * REAL zygote seccomp filter RET_TRAP such that tawcroot — with no
 * dispatch handler for them — answers -ENOSYS to the guest?
 *
 * Runs as a tawcroot guest under the prod-env layer (app uid,
 * untrusted_app, the zygote-installed filter). For a legacy NR the
 * kernel actually implements, the guest can only ever see -ENOSYS if
 * something TRAPPED the syscall and no handler serviced it — i.e. the
 * exact "trapped-but-unhandled" gap the legacy-trapset audit in
 * docs/tawcroot/status.md is about. (tawcroot only ever traps NRs it also
 * handles, so an -ENOSYS here means: Android trapped it AND tawcroot
 * has no handler.)
 *
 * Method: issue each candidate with benign args (bad fd / NULL / zero
 * timeout — never a real side effect, never a blocking call) via the
 * raw NR and classify errno. Three expectation classes:
 *   CTRL_OK    - must NOT be ENOSYS (Android allows it, or tawcroot
 *                handles it). Validates the probe isn't crying wolf.
 *   CTRL_NOSYS - ENOSYS is EXPECTED (tawcroot deliberately denies:
 *                openat2/faccessat2/clone3). Positive control that the
 *                ENOSYS detector actually fires.
 *   AUDIT      - unknown. ENOSYS == a real gap to report.
 *
 * Output: one line per NR on stdout (the broker relays it). Exit 42
 * iff every CTRL_OK avoided ENOSYS, every CTRL_NOSYS hit ENOSYS, and no
 * AUDIT NR hit ENOSYS; else exit 1. So the prod-env test can assert 42
 * and a human reading --nocapture sees the full table.
 *
 * x86_64-only (the NRs are lp64-x86_64 legacy numbers). On aarch64 the
 * same source compiles to a no-op that exits 42 — those NRs don't exist
 * there, so there's nothing to audit. */

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#if defined(__x86_64__)

#include <sys/syscall.h>

enum { CTRL_OK, CTRL_NOSYS, AUDIT };

struct probe {
	long nr;
	const char *name;
	int cls;
	/* Up to 6 args. Pointers are filled in at run time (can't be
	 * static initializers), so a small `kind` selects the arg shape. */
	int kind;
};

/* Arg shapes. Each is chosen to be non-blocking and side-effect-free:
 * bad fds surface EBADF, NULL paths/pointers surface EFAULT, zero
 * timeouts return immediately. Nothing here mutates the filesystem or
 * leaks an fd, except the three fd-creators which we close. */
enum {
	K_NONE,        /* () */
	K_NULLPATH,    /* (NULL, 0) — path syscalls -> EFAULT */
	K_NULLPATH3,   /* (NULL, NULL) or (NULL, 0, 0) -> EFAULT */
	K_BADFD_STATBUF, /* (-1, &buf) -> EBADF */
	K_POLL,        /* (NULL, 0, 0) -> 0 */
	K_SELECT,      /* (0, NULL, NULL, NULL, &zero_tv) -> 0 */
	K_EPOLL_WAIT,  /* (-1, buf, 1, 0) -> EBADF */
	K_FDCREATE0,   /* (0) then close(rv) — eventfd */
	K_EPOLL_CREATE,/* (1) then close(rv) */
	K_INOTIFY_INIT,/* () then close(rv) */
	K_SIGNALFD,    /* (-1, NULL, 8) -> EFAULT (no fd) */
	K_PIPE_NULL,   /* (NULL) -> EFAULT */
	K_ALARM0,      /* (0) -> 0 */
	K_TIME_NULL,   /* (NULL) -> current time */
	K_GETRLIMIT,   /* (RLIMIT_NOFILE, &buf) -> 0 */
	K_OPENAT_NULL, /* (AT_FDCWD, NULL, 0) -> EFAULT */
	K_DUP2_SELF,   /* (0, 0) -> 0 (dup stdin onto itself: no side effect) */
};

static long issue(long nr, int kind)
{
	char buf[512];
	long tv[2] = { 0, 0 };
	switch (kind) {
	case K_NONE:          return syscall(nr);
	case K_NULLPATH:      return syscall(nr, (long)0, 0);
	case K_NULLPATH3:     return syscall(nr, (long)0, (long)0, 0);
	case K_BADFD_STATBUF: return syscall(nr, -1, (long)buf);
	case K_POLL:          return syscall(nr, (long)0, 0, 0);
	case K_SELECT:        return syscall(nr, 0, (long)0, (long)0, (long)0, (long)tv);
	case K_EPOLL_WAIT:    return syscall(nr, -1, (long)buf, 1, 0);
	case K_FDCREATE0: {
		long r = syscall(nr, 0);
		if (r >= 0) syscall(SYS_close, r);
		return r;
	}
	case K_EPOLL_CREATE: {
		long r = syscall(nr, 1);
		if (r >= 0) syscall(SYS_close, r);
		return r;
	}
	case K_INOTIFY_INIT: {
		long r = syscall(nr);
		if (r >= 0) syscall(SYS_close, r);
		return r;
	}
	case K_SIGNALFD:      return syscall(nr, -1, (long)0, 8);
	case K_PIPE_NULL:     return syscall(nr, (long)0);
	case K_ALARM0:        return syscall(nr, 0);
	case K_TIME_NULL:     return syscall(nr, (long)0);
	case K_GETRLIMIT:     return syscall(nr, 7 /*RLIMIT_NOFILE*/, (long)buf);
	case K_OPENAT_NULL:   return syscall(nr, -100 /*AT_FDCWD*/, (long)0, 0);
	case K_DUP2_SELF:     return syscall(nr, 0, 0);
	}
	return syscall(nr);
}

int main(void)
{
	static const struct probe probes[] = {
		/* --- CTRL_OK: modern syscalls Android allows outright ------ */
		{ 39,  "getpid",        CTRL_OK, K_NONE },
		{ 257, "openat",        CTRL_OK, K_OPENAT_NULL },
		{ 262, "newfstatat",    CTRL_OK, K_NULLPATH3 },
		{ 271, "ppoll",         CTRL_OK, K_POLL },
		{ 281, "epoll_pwait",   CTRL_OK, K_EPOLL_WAIT },

		/* --- CTRL_OK: legacy NRs tawcroot ALREADY traps+handles ---- */
		{ 2,   "open",          CTRL_OK, K_NULLPATH },
		{ 4,   "stat",          CTRL_OK, K_NULLPATH3 },
		{ 6,   "lstat",         CTRL_OK, K_NULLPATH3 },
		{ 7,   "poll",          CTRL_OK, K_POLL },
		{ 21,  "access",        CTRL_OK, K_NULLPATH },
		{ 33,  "dup2",          CTRL_OK, K_DUP2_SELF },
		{ 78,  "getdents",      CTRL_OK, K_EPOLL_WAIT /* (-1,buf,n,..) EBADF */ },
		{ 111, "getpgrp",       CTRL_OK, K_NONE },
		{ 232, "epoll_wait",    CTRL_OK, K_EPOLL_WAIT },

		/* --- CTRL_NOSYS: tawcroot deliberately denies (fallback) --- */
		{ 435, "clone3",        CTRL_NOSYS, K_NULLPATH },
		{ 437, "openat2",       CTRL_NOSYS, K_NULLPATH3 },
		/* faccessat2 is trapped but SERVICED (routed to faccessat), so
		 * a NULL path surfaces EFAULT, not ENOSYS — it's a CTRL_OK. */
		{ 439, "faccessat2",    CTRL_OK, K_NULLPATH3 },

		/* --- AUDIT: the unknowns this issue is about --------------- */
		{ 23,  "select",        AUDIT, K_SELECT },
		{ 213, "epoll_create",  AUDIT, K_EPOLL_CREATE },
		{ 253, "inotify_init",  AUDIT, K_INOTIFY_INIT },
		{ 282, "signalfd",      AUDIT, K_SIGNALFD },
		{ 284, "eventfd",       AUDIT, K_FDCREATE0 },
		{ 22,  "pipe",          AUDIT, K_PIPE_NULL },
		{ 37,  "alarm",         AUDIT, K_ALARM0 },
		{ 201, "time",          AUDIT, K_TIME_NULL },
		{ 97,  "getrlimit",     AUDIT, K_GETRLIMIT },
		{ 85,  "creat",         AUDIT, K_NULLPATH },
		{ 133, "mknod",         AUDIT, K_NULLPATH3 },
		/* Modern siblings of the AUDIT set — expected allowed; if any
		 * of these is ENOSYS the fallback story is worse than thought. */
		{ 270, "pselect6",      CTRL_OK, K_NULLPATH3 },
		{ 291, "epoll_create1", CTRL_OK, K_NULLPATH },
		{ 294, "inotify_init1", CTRL_OK, K_NULLPATH },
		{ 289, "signalfd4",     CTRL_OK, K_SIGNALFD },
		{ 290, "eventfd2",      CTRL_OK, K_FDCREATE0 },
		{ 293, "pipe2",         CTRL_OK, K_NULLPATH },
	};

	int fails = 0, gaps = 0;
	for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
		const struct probe *p = &probes[i];
		errno = 0;
		long rv = issue(p->nr, p->kind);
		int e = (rv == -1) ? errno : 0;
		int is_nosys = (e == ENOSYS);

		const char *verdict;
		if (p->cls == CTRL_OK) {
			verdict = is_nosys ? "FAIL(unexpected ENOSYS)" : "ok";
			if (is_nosys) fails++;
		} else if (p->cls == CTRL_NOSYS) {
			verdict = is_nosys ? "ok(denied)" : "FAIL(expected ENOSYS)";
			if (!is_nosys) fails++;
		} else {
			verdict = is_nosys ? "GAP(ENOSYS)" : "ok(serviced)";
			if (is_nosys) gaps++;
		}
		dprintf(1, "NR %3ld %-14s rv=%ld errno=%d %s\n",
			p->nr, p->name, rv, e, verdict);
	}

	dprintf(1, "SUMMARY ctrl_fails=%d audit_gaps=%d\n", fails, gaps);
	return (fails == 0 && gaps == 0) ? 42 : 1;
}

#else /* !__x86_64__ */

int main(void)
{
	dprintf(1, "legacy-NR audit is x86_64-only; no-op on this arch\n");
	return 42;
}

#endif
