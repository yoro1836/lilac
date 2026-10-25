/* The SIGSYS handler runs with every guest signal blocked, so a call
 * it makes that sleeps (accept, recv) sleeps through SIGCHLD and the
 * like: zygote, waiting in accept for its next command, then never
 * reaps a dead system_server and Android never restarts. Wait for the
 * fd and for the guest's deliverable signals together instead, and let
 * a signal run the way the kernel would: the call restarts after its
 * handler (SA_RESTART) or fails EINTR. */
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include "blocking.h"
#include "errno_neg.h"
#include "raw_sys.h"
#include "sysnr.h"

#define SA_RESTART_ 0x10000000ul

struct k_pollfd { int fd; short events, revents; };
struct k_timespec { long sec, nsec; };

/* SIGCHLD, SIGCONT, SIGURG, SIGWINCH: ignored when left at SIG_DFL. */
static int default_ignored(int sig)
{
	return sig == 17 || sig == 18 || sig == 23 || sig == 28;
}

/* What the pending deliverable signals want: TAWC_ERESTARTSYS,
 * TAWC_EINTR, or 0 when none of them does anything (consumed here, as
 * delivery would have discarded them). */
static long pending_action(uint64_t deliverable, int sfd)
{
	uint64_t pending = 0;
	TAWC_RAW(__NR_rt_sigpending, (long)&pending, 8, 0, 0, 0, 0);
	pending &= deliverable;
	long r = 0;
	for (int sig = 1; sig <= 64; ++sig) {
		if (!(pending & (1ull << (sig - 1)))) continue;
		unsigned long act[4] = { 0 };
		TAWC_RAW(__NR_rt_sigaction, sig, 0, (long)act, 8, 0, 0);
		if (act[0] == 1 || (act[0] == 0 && default_ignored(sig))) continue;
		if (act[0] > 1 && !(act[1] & SA_RESTART_)) return TAWC_EINTR;
		r = TAWC_ERESTARTSYS;  /* restarting handler, or a default stop/kill */
	}
	if (!r) {
		/* Only discards pending: drop them so the fd stops polling. */
		unsigned char info[128];
		while (TAWC_RAW(TAWC_SYS_read, sfd, (long)info, sizeof info, 0, 0, 0) > 0) {}
	}
	return r;
}

long tawcroot_wait_fd(int fd, short events, ucontext_t *uc)
{
	if (!uc) return 0;
	long fl = TAWC_RAW(TAWC_SYS_fcntl, fd, F_GETFL, 0, 0, 0, 0);
	if (fl < 0 || (fl & O_NONBLOCK)) return 0;
	struct k_timespec timeout = { 0, 0 }, *tp = 0;
	struct { long sec, usec; } tv = { 0, 0 };
	uint32_t tl = sizeof tv;
	if (TAWC_RAW(TAWC_SYS_getsockopt, fd, 1 /*SOL_SOCKET*/, events & 4 ? 21 : 20 /*SO_SNDTIMEO / SO_RCVTIMEO*/,
		     (long)&tv, (long)&tl, 0) == 0 && (tv.sec || tv.usec)) {
		timeout.sec = tv.sec;
		timeout.nsec = tv.usec * 1000;
		tp = &timeout;
	}
	uint64_t deliverable = ~*(uint64_t *)&uc->uc_sigmask &
		~((1ull << (31 - 1)) | (1ull << (9 - 1)) | (1ull << (19 - 1)));  /* SIGSYS, SIGKILL, SIGSTOP */
	long sfd = TAWC_RAW(__NR_signalfd4, -1, (long)&deliverable, 8, O_CLOEXEC | O_NONBLOCK, 0, 0);
	if (sfd < 0) return 0;
	long r;
	for (;;) {
		struct k_pollfd p[2] = { { fd, events, 0 }, { (int)sfd, 1 /*POLLIN*/, 0 } };
		long n = TAWC_RAW(__NR_ppoll, (long)p, 2, (long)tp, 0, 8, 0);
		if (n == TAWC_EINTR) continue;
		if (n < 0 || p[0].revents) { r = 0; break; }
		if (n == 0) { r = TAWC_EAGAIN; break; }
		if (p[1].revents && (r = pending_action(deliverable, (int)sfd))) break;
	}
	TAWC_RAW(TAWC_SYS_close, sfd, 0, 0, 0, 0, 0);
	return r;
}
