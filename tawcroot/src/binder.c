/* In-handler binder shim: guest binder fds are connections to binderd.
 * See include/binder.h and include/binder_wire.h.
 *
 * The guest's binder fd is the process connection (SOCK_SEQPACKET), so
 * poll/epoll on it works; binderd keeps a BW_POLL byte queued while the
 * process has work. It lives at fd >= TAWCROOT_RESERVED_FD_BASE, which
 * makes its close and mmap trap: mmap maps the receive buffer memfd
 * binderd writes transactions into, read-only, as the kernel does.
 *
 * Each guest thread gets its own connection plus a staging memfd shared
 * with binderd that carries requests and responses, so nothing large
 * lives on the handler stack.
 *
 * Async-signal-safe: raw syscalls, one spinlock around the tables.
 */

#include <stddef.h>
#include <stdint.h>

#include <asm/unistd.h>
#include <linux/android/binder.h>

#include "binder.h"
#include "binder_wire.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "identity.h"
#include "io.h"
#include "namespace.h"
#include "path.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

#define AF_UNIX_          1
#define SOCK_SEQPACKET_   5
#define SOCK_CLOEXEC_     02000000
#define MSG_DONTWAIT_     0x40
#define MSG_NOSIGNAL_     0x4000
#define MSG_CMSG_CLOEXEC_ 0x40000000
#define SOL_SOCKET_       1
#define SCM_RIGHTS_       1
#define SO_RCVTIMEO_      20
#define POLLIN_           1
#define PROT_READ_        1
#define PROT_WRITE_       2
#define MAP_SHARED_       1
#define MAP_TYPE_         0x0f
#define MAP_PRIVATE_      2
#define MAP_ANONYMOUS_    0x20
#define F_DUPFD_          0
#define F_DUPFD_CLOEXEC_  1030
#define SIGSYS_           31
#define SIGKILL_          9
#define SIGSTOP_          19

#define MAX_BINDERS 16
#define MAX_THREADS 512

/* Tail of the request area: fds to send (then the ones received) and
 * PTR copy positions. Kept off the handler stack (stack_budget.h). */
#define SCRATCH_FDS    (BW_STAGE_REQ - BW_MAX_FDS * 4)
#define MAX_PTRS       1024
#define SCRATCH_PTRS   (SCRATCH_FDS - MAX_PTRS * 24)
#define REQ_LIMIT      SCRATCH_PTRS

struct k_iovec { void *base; size_t len; };

struct k_msghdr {
	void *name;
	int namelen;
	struct k_iovec *iov;
	size_t iovlen;
	void *control;
	size_t controllen;
	unsigned flags;
};

struct k_cmsghdr { size_t len; int level; int type; };

#define CMSG_ALIGN_(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE_(n) (CMSG_ALIGN_(sizeof(struct k_cmsghdr)) + CMSG_ALIGN_(n))
#define CMSG_LEN_(n)   (CMSG_ALIGN_(sizeof(struct k_cmsghdr)) + (n))
#define CMSG_DATA_(c)  ((unsigned char *)(c) + CMSG_ALIGN_(sizeof(struct k_cmsghdr)))

struct k_pollfd { int fd; short events, revents; };

struct k_sockaddr_un { unsigned short family; char path[108]; };

struct binder_slot {
	int fd;               /* guest binder fd = process connection; 0 = free */
	int buf_fd;           /* reserved: the receive buffer memfd */
	int nonblock;
	long owner;           /* host pid that opened it */
	uint64_t map_base, map_size;
	uint8_t *alias;       /* our RW view of the buffer */
};

struct thread_slot {
	int binder;           /* index + 1 into g_binders; 0 = free */
	int tid;
	int sock;             /* reserved */
	int sigfd;            /* reserved, -1 until needed */
	uint64_t sigmask;     /* what sigfd watches */
	uint8_t *stage;
};

static struct binder_slot g_binders[MAX_BINDERS];
static struct thread_slot g_threads[MAX_THREADS];
static volatile int g_lock;

/* Pid whose tables this memory is. A MADV_WIPEONFORK page: a fork
 * child reads 0 and takes over its private copy, while a CLONE_VM child
 * (vfork, posix_spawn) still reads its parent's pid and must leave the
 * tables alone. ART's Runtime.exec child closes every fd before exec. */
#define MADV_WIPEONFORK_ 18
static volatile long *g_owner;

static void lock(void)
{
	while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) {}
}

static void unlock(void)
{
	__atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

/* ---- helpers ------------------------------------------------------------ */

static int own_tables(void)
{
	if (!g_owner) return 1;
	long pid = tawc_getpid();
	if (!*g_owner) *g_owner = pid;
	return *g_owner == pid;
}

static void owner_setup(void)
{
	if (!g_owner) {
		long m = tawc_mmap(0, 4096, PROT_READ_ | PROT_WRITE_, MAP_PRIVATE_ | MAP_ANONYMOUS_, -1, 0);
		if (m < 0 && m > -4096) return;
		if (TAWC_RAW(TAWC_SYS_madvise, m, 4096, MADV_WIPEONFORK_, 0, 0, 0) < 0) {
			tawc_munmap((void *)m, 4096);
			return;
		}
		g_owner = (volatile long *)m;
	}
	*g_owner = tawc_getpid();
}

/* Move fd to a reserved high slot; closes the original. */
static long reserve(long fd)
{
	long hi = TAWC_RAW(TAWC_SYS_fcntl, fd, F_DUPFD_CLOEXEC_, TAWCROOT_RESERVED_FD_BASE, 0, 0, 0);
	tawc_close((int)fd);
	if (hi < 0) return hi;
	long rv = tawcroot_fd_record_reserved((int)hi);
	if (rv < 0) {
		tawc_close((int)hi);
		return rv;
	}
	return hi;
}

static void unreserve(int fd)
{
	if (fd < 0) return;
	tawcroot_fd_forget_reserved(fd);
	tawc_close(fd);
}

static void *map_shared(int fd, size_t len, int prot)
{
	long p = TAWC_RAW(TAWC_SYS_mmap, 0, (long)len, prot, MAP_SHARED_, fd, 0);
	return p < 0 && p > -4096 ? (void *)0 : (void *)p;
}

static long new_memfd(const char *name, size_t size)
{
	long fd = TAWC_RAW(TAWC_SYS_memfd_create, (long)name, MFD_CLOEXEC, 0, 0, 0, 0);
	if (fd < 0) return fd;
	long rv = TAWC_RAW(TAWC_SYS_ftruncate, fd, (long)size, 0, 0, 0, 0);
	if (rv < 0) {
		tawc_close((int)fd);
		return rv;
	}
	return fd;
}

/* noinline: keep the two cmsg buffers out of one caller frame. */
__attribute__((noinline))
static long send_msg(int sock, const struct bw_msg *m, const int *fds, int nfds)
{
	char ctrl[CMSG_SPACE_(sizeof(int) * BW_MAX_FDS)];
	struct k_iovec iov = { (void *)m, sizeof *m };
	struct k_msghdr mh = { .iov = &iov, .iovlen = 1 };
	if (nfds > 0) {
		struct k_cmsghdr *cm = (struct k_cmsghdr *)ctrl;
		cm->len = CMSG_LEN_(sizeof(int) * (size_t)nfds);
		cm->level = SOL_SOCKET_;
		cm->type = SCM_RIGHTS_;
		memcpy(CMSG_DATA_(cm), fds, sizeof(int) * (size_t)nfds);
		mh.control = ctrl;
		mh.controllen = CMSG_SPACE_(sizeof(int) * (size_t)nfds);
	}
	long rv = TAWC_RAW(TAWC_SYS_sendmsg, sock, (long)&mh, MSG_NOSIGNAL_, 0, 0, 0);
	return rv == (long)sizeof *m ? 0 : (rv < 0 ? rv : TAWC_EIO);
}

/* Receive one message; up to `max` fds land in `fds` (CLOEXEC, as the
 * kernel's binder installs them), extras are closed. Returns bytes or
 * -errno; 0 is hangup. */
__attribute__((noinline))
static long recv_msg(int sock, struct bw_msg *m, int *fds, int *nfds, int max, int flags)
{
	char ctrl[CMSG_SPACE_(sizeof(int) * BW_MAX_FDS)];
	struct k_iovec iov = { m, sizeof *m };
	struct k_msghdr mh = { .iov = &iov, .iovlen = 1, .control = ctrl, .controllen = sizeof ctrl };
	*nfds = 0;
	long n = TAWC_RAW(TAWC_SYS_recvmsg, sock, (long)&mh, MSG_CMSG_CLOEXEC_ | flags, 0, 0, 0);
	if (n <= 0 || mh.controllen < sizeof(struct k_cmsghdr)) return n;
	struct k_cmsghdr *cm = (struct k_cmsghdr *)ctrl;
	if (cm->level == SOL_SOCKET_ && cm->type == SCM_RIGHTS_) {
		int k = (int)((cm->len - CMSG_LEN_(0)) / sizeof(int));
		const int *in = (const int *)CMSG_DATA_(cm);
		for (int i = 0; i < k && i < BW_MAX_FDS; i++) {
			if (*nfds < max) fds[(*nfds)++] = in[i];
			else tawc_close(in[i]);
		}
	}
	return n;
}

/* Our pid, cached beside the owner in the MADV_WIPEONFORK page: a fork
 * child reads 0 and asks again. Saves a getpid per binder ioctl. */
static long self_pid(void)
{
	if (!g_owner) return tawc_getpid();
	long pid = g_owner[1];
	if (!pid) g_owner[1] = pid = tawc_getpid();
	return pid;
}

static int32_t guest_pid(void)
{
	long pid = self_pid();
	return pid == tawcroot_namespace.init_pid ? 1 : (int32_t)pid;
}

static struct binder_slot *find_binder(int fd)
{
	if (fd < TAWCROOT_RESERVED_FD_BASE) return 0;
	for (int i = 0; i < MAX_BINDERS; i++)
		if (__atomic_load_n(&g_binders[i].fd, __ATOMIC_ACQUIRE) == fd) return &g_binders[i];
	return 0;
}

/* ---- open --------------------------------------------------------------- */

/* "binder" / "hwbinder" / "vndbinder" for a binder device path, else 0. */
static const char *device_name(const char *path)
{
	const char *name;
	if (tawc_starts_with(path, "/dev/binderfs/")) name = path + 14;
	else if (tawc_starts_with(path, "/dev/")) name = path + 5;
	else return 0;
	if (tawc_streq(name, "binder") || tawc_streq(name, "hwbinder") ||
	    tawc_streq(name, "vndbinder"))
		return name;
	return 0;
}

static const struct tawcroot_bind *binderfs_bind(void)
{
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		if (b->active && tawc_streq(b->dst, "dev/binderfs")) return b;
	}
	return 0;
}

long tawcroot_binder_open(int dirfd, const char *path, int flags)
{
	(void)dirfd;
	const char *name = device_name(path);
	if (!name) return TAWC_ENOSYS;
	const struct tawcroot_bind *b = binderfs_bind();
	if (!b) return TAWC_ENOSYS;

	struct k_sockaddr_un sa = { .family = AF_UNIX_ };
	size_t n = 0;
	int fits = tawc_str_append(sa.path, sizeof sa.path, &n, b->src) == 0 &&
		   tawc_str_append(sa.path, sizeof sa.path, &n, "/") == 0 &&
		   tawc_str_append(sa.path, sizeof sa.path, &n, name) == 0;

	long sock = TAWC_RAW(TAWC_SYS_socket, AF_UNIX_, SOCK_SEQPACKET_ | SOCK_CLOEXEC_, 0, 0, 0, 0);
	if (sock < 0) return sock;
	long rv = -1;
	if (fits) {
		rv = TAWC_RAW(TAWC_SYS_connect, sock, (long)&sa, sizeof sa, 0, 0, 0);
		if (rv < 0 && n + 1 < sizeof sa.path) {
			/* binderd --abstract: the same name in the abstract namespace. */
			memmove(sa.path + 1, sa.path, n + 1);
			sa.path[0] = 0;
			rv = TAWC_RAW(TAWC_SYS_connect, sock, (long)&sa,
				      offsetof(struct k_sockaddr_un, path) + 1 + n, 0, 0, 0);
		}
	} else if (tawc_proc_fd_path(sa.path, sizeof sa.path, b->src_fd, name) > 0) {
		/* A source dir too long for sun_path (app data dirs): through
		 * the bind's own directory fd. */
		rv = TAWC_RAW(TAWC_SYS_connect, sock, (long)&sa, sizeof sa, 0, 0, 0);
	}
	if (rv < 0) {
		tawc_close((int)sock);
		return TAWC_ENOENT;
	}
	long mem = new_memfd("tawc-binder", BW_BUFFER_SIZE);
	if (mem < 0) {
		tawc_close((int)sock);
		return mem;
	}
	uint8_t *alias = map_shared((int)mem, BW_BUFFER_SIZE, PROT_READ_ | PROT_WRITE_);
	struct bw_msg m = {
		.magic = BW_MAGIC, .type = BW_HELLO,
		.host_pid = (int32_t)tawc_getpid(), .guest_pid = guest_pid(),
		.euid = tawcroot_identity_euid(),
	};
	int memfd = (int)mem;
	rv = alias ? send_msg((int)sock, &m, &memfd, 1) : TAWC_ENOMEM;
	if (!rv) {
		/* Bound the handshake so a wedged binderd cannot hang open(). */
		struct { long sec, usec; } tv = { 5, 0 };
		TAWC_RAW(TAWC_SYS_setsockopt, sock, SOL_SOCKET_, SO_RCVTIMEO_, (long)&tv, sizeof tv, 0);
		int fds[1], nfds;
		struct bw_msg ack;
		long got = recv_msg((int)sock, &ack, fds, &nfds, 1, 0);
		for (int i = 0; i < nfds; i++) tawc_close(fds[i]);
		struct { long sec, usec; } zero = { 0, 0 };
		TAWC_RAW(TAWC_SYS_setsockopt, sock, SOL_SOCKET_, SO_RCVTIMEO_, (long)&zero, sizeof zero, 0);
		if (got != (long)sizeof ack || ack.magic != BW_MAGIC) rv = TAWC_EIO;
		else rv = ack.status;
	}
	long guest_fd = rv ? rv : TAWC_RAW(TAWC_SYS_fcntl, sock,
					   (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC_ : F_DUPFD_,
					   TAWCROOT_RESERVED_FD_BASE, 0, 0, 0);
	tawc_close((int)sock);
	long buf_fd = guest_fd < 0 ? guest_fd : reserve(mem);
	if (guest_fd < 0 || buf_fd < 0) {
		if (alias) tawc_munmap(alias, BW_BUFFER_SIZE);
		if (guest_fd >= 0) tawc_close((int)guest_fd);
		else tawc_close((int)mem);
		return guest_fd < 0 ? guest_fd : buf_fd;
	}

	lock();
	if (!g_owner || !*g_owner) owner_setup();
	struct binder_slot *s = 0;
	for (int i = 0; i < MAX_BINDERS && !s; i++)
		if (!g_binders[i].fd) s = &g_binders[i];
	if (s) {
		s->buf_fd = (int)buf_fd;
		s->nonblock = (flags & O_NONBLOCK) != 0;
		s->owner = tawc_getpid();
		s->map_base = s->map_size = 0;
		s->alias = alias;
		__atomic_store_n(&s->fd, (int)guest_fd, __ATOMIC_RELEASE);
	}
	unlock();
	if (!s) {
		tawc_munmap(alias, BW_BUFFER_SIZE);
		unreserve((int)buf_fd);
		tawc_close((int)guest_fd);
		return TAWC_EMFILE;
	}
	return guest_fd;
}

/* ---- threads ------------------------------------------------------------ */

static void free_thread_locked(struct thread_slot *t)
{
	if (t->stage) tawc_munmap(t->stage, BW_STAGE_SIZE);
	unreserve(t->sock);
	unreserve(t->sigfd);
	t->stage = 0;
	t->sock = t->sigfd = -1;
	t->binder = 0;
}

static struct thread_slot *get_thread(struct binder_slot *s)
{
	int index = (int)(s - g_binders) + 1;
	int tid = (int)TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	for (int i = 0; i < MAX_THREADS; i++) {
		struct thread_slot *t = &g_threads[i];
		if (__atomic_load_n(&t->binder, __ATOMIC_ACQUIRE) == index && t->tid == tid) return t;
	}

	/* New thread. Reclaim slots of threads that exited without
	 * BINDER_THREAD_EXIT (or belong to the parent, after a fork). */
	long pid = tawc_getpid();
	struct thread_slot *slot = 0;
	lock();
	for (int i = 0; i < MAX_THREADS; i++) {
		struct thread_slot *t = &g_threads[i];
		if (t->binder > 0 && TAWC_RAW(TAWC_SYS_tgkill, pid, t->tid, 0, 0, 0, 0) == TAWC_ESRCH)
			free_thread_locked(t);
		if (!t->binder && !slot) slot = t;
	}
	if (slot) slot->binder = -1;  /* claimed */
	unlock();
	if (!slot) return 0;

	int sv[2] = { -1, -1 };
	long mem = -1, sock = -1;
	uint8_t *stage = 0;
	long rv = TAWC_RAW(__NR_socketpair, AF_UNIX_, SOCK_SEQPACKET_ | SOCK_CLOEXEC_, 0, (long)sv, 0, 0);
	if (!rv) rv = mem = new_memfd("tawc-binder-stage", BW_STAGE_SIZE);
	if (rv >= 0 && !(stage = map_shared((int)mem, BW_STAGE_SIZE, PROT_READ_ | PROT_WRITE_)))
		rv = TAWC_ENOMEM;
	if (rv >= 0) {
		struct bw_msg m = { .magic = BW_MAGIC, .type = BW_NEW_THREAD, .tid = tid };
		int fds[2] = { sv[1], (int)mem };
		rv = send_msg(s->fd, &m, fds, 2);
	}
	if (sv[1] >= 0) tawc_close(sv[1]);
	if (mem >= 0) tawc_close((int)mem);
	if (rv >= 0) sock = reserve(sv[0]);
	else if (sv[0] >= 0) tawc_close(sv[0]);

	lock();
	if (sock < 0) {
		if (stage) tawc_munmap(stage, BW_STAGE_SIZE);
		slot->binder = 0;
		unlock();
		return 0;
	}
	slot->tid = tid;
	slot->sock = (int)sock;
	slot->sigfd = -1;
	slot->stage = stage;
	__atomic_store_n(&slot->binder, index, __ATOMIC_RELEASE);
	unlock();
	return slot;
}

static void drop_thread(struct binder_slot *s)
{
	int index = (int)(s - g_binders) + 1;
	int tid = (int)TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	lock();
	for (int i = 0; i < MAX_THREADS; i++)
		if (g_threads[i].binder == index && g_threads[i].tid == tid) free_thread_locked(&g_threads[i]);
	unlock();
}

void tawcroot_binder_close(int fd)
{
	struct binder_slot *s = find_binder(fd);
	if (!s || !own_tables()) return;
	int index = (int)(s - g_binders) + 1;
	lock();
	for (int i = 0; i < MAX_THREADS; i++)
		if (g_threads[i].binder == index) free_thread_locked(&g_threads[i]);
	if (s->alias) tawc_munmap(s->alias, BW_BUFFER_SIZE);
	unreserve(s->buf_fd);
	s->alias = 0;
	__atomic_store_n(&s->fd, 0, __ATOMIC_RELEASE);
	unlock();
}

/* ---- requests ------------------------------------------------------------ */

static void drain_poll(struct binder_slot *s)
{
	struct bw_msg m;
	int fds[2], nfds;
	for (;;) {
		long n = recv_msg(s->fd, &m, fds, &nfds, 2, MSG_DONTWAIT_);
		for (int i = 0; i < nfds; i++) tawc_close(fds[i]);
		if (n <= 0) break;
	}
}

static void fill_header(struct bw_request *rq, struct binder_slot *s, uint32_t kind)
{
	memset(rq, 0, sizeof *rq);
	rq->kind = kind;
	rq->guest_pid = guest_pid();
	rq->euid = tawcroot_identity_euid();
	rq->map_base = s->map_base;
	rq->map_size = s->map_size;
}

/* Send the request in t->stage and wait for BW_DONE; the fds that come
 * back replace the sent ones at SCRATCH_FDS. Returns 0 or -errno for
 * transport failures. A pending guest signal turns a blocked wait into
 * BW_CANCEL. */
static long round_trip(struct thread_slot *t, int nsend, int *nfds, ucontext_t *uc)
{
	int *fds = (int *)(t->stage + SCRATCH_FDS);
	struct bw_msg m = { .magic = BW_MAGIC, .type = BW_GO, .nfds = (uint32_t)nsend };
	long rv = send_msg(t->sock, &m, (const int *)(t->stage + SCRATCH_FDS), nsend);
	if (rv) return rv;
	int cancelled = 0;
	for (;;) {
		struct k_pollfd pf[2] = { { t->sock, POLLIN_, 0 }, { -1, POLLIN_, 0 } };
		if (uc && !cancelled) {
			uint64_t mask = ~*(uint64_t *)&uc->uc_sigmask;
			mask &= ~((1ull << (SIGSYS_ - 1)) | (1ull << (SIGKILL_ - 1)) | (1ull << (SIGSTOP_ - 1)));
			/* The mask rarely changes between calls */
			if (t->sigfd < 0 || mask != t->sigmask) {
				long sfd = TAWC_RAW(__NR_signalfd4, t->sigfd, (long)&mask, 8,
						    O_CLOEXEC | O_NONBLOCK, 0, 0);
				if (t->sigfd < 0 && sfd >= 0) {
					sfd = reserve(sfd);
					if (sfd >= 0) t->sigfd = (int)sfd;
				}
				if (sfd >= 0) t->sigmask = mask;
			}
			pf[1].fd = t->sigfd;
		}
		long n = TAWC_RAW(__NR_ppoll, (long)pf, pf[1].fd >= 0 ? 2 : 1, 0, 0, 8, 0);
		if (n < 0 && n != TAWC_EINTR) return n;
		if (pf[0].revents) break;
		if (pf[1].fd >= 0 && pf[1].revents && !cancelled) {
			struct bw_msg c = { .magic = BW_MAGIC, .type = BW_CANCEL };
			if (send_msg(t->sock, &c, 0, 0)) return TAWC_EIO;
			cancelled = 1;
		}
	}
	long n = recv_msg(t->sock, &m, fds, nfds, BW_MAX_FDS, 0);
	if (n != (long)sizeof m || m.magic != BW_MAGIC || m.type != BW_DONE) {
		for (int i = 0; i < *nfds; i++) tawc_close(fds[i]);
		*nfds = 0;
		return TAWC_EIO;
	}
	return 0;
}

static long copy_in(uint8_t *stage, uint64_t *pos, uint64_t guest, uint64_t len)
{
	if (len > REQ_LIMIT || REQ_LIMIT - *pos < BW_ALIGN8(len)) return TAWC_E2BIG;
	if (len && tawc_copy_from_guest(stage + *pos, len, (const void *)(uintptr_t)guest))
		return TAWC_EFAULT;
	*pos += BW_ALIGN8(len);
	return 0;
}

static size_t object_size(uint32_t type)
{
	switch (type) {
	case BINDER_TYPE_BINDER:
	case BINDER_TYPE_WEAK_BINDER:
	case BINDER_TYPE_HANDLE:
	case BINDER_TYPE_WEAK_HANDLE: return sizeof(struct flat_binder_object);
	case BINDER_TYPE_FD:          return sizeof(struct binder_fd_object);
	case BINDER_TYPE_FDA:         return sizeof(struct binder_fd_array_object);
	case BINDER_TYPE_PTR:         return sizeof(struct binder_buffer_object);
	default:                      return 0;
	}
}

struct ptr_copy { uint64_t index, pos, len; };

/* Stage one transaction: header, data, offsets, PTR buffers; collect
 * its fds. A section the shim cannot build is flagged BW_TX_BAD so
 * binderd fails just that transaction. */
static void stage_transaction(uint8_t *stage, uint64_t *pos, const struct binder_transaction_data *tr,
			      uint64_t sg_limit, int *nsend)
{
	uint64_t at = *pos;
	struct bw_tx_section sec = { .data_size = tr->data_size, .offsets_size = tr->offsets_size };
	if (REQ_LIMIT - at < sizeof sec) return;
	*pos += sizeof sec;
	uint64_t data_at = *pos;
	int nsend_start = *nsend;
	int nptrs = 0;
	struct ptr_copy *ptrs = (struct ptr_copy *)(stage + SCRATCH_PTRS);
	int *fds = (int *)(stage + SCRATCH_FDS);
	if (tr->offsets_size % 8 ||
	    copy_in(stage, pos, tr->data.ptr.buffer, tr->data_size) ||
	    copy_in(stage, pos, tr->data.ptr.offsets, tr->offsets_size))
		goto bad;
	const uint8_t *data = stage + data_at;
	const uint8_t *offsets = stage + data_at + BW_ALIGN8(tr->data_size);
	uint64_t sg_start = *pos;
	for (uint64_t i = 0; i < tr->offsets_size / 8; i++) {
		uint64_t off;
		uint32_t type;
		memcpy(&off, offsets + i * 8, 8);
		if (off > tr->data_size || tr->data_size - off < 4) goto bad;
		memcpy(&type, data + off, 4);
		size_t size = object_size(type);
		if (!size || tr->data_size - off < size) goto bad;
		if (type == BINDER_TYPE_FD) {
			struct binder_fd_object fo;
			memcpy(&fo, data + off, sizeof fo);
			if (*nsend >= BW_MAX_FDS) goto bad;
			fds[(*nsend)++] = (int)fo.fd;
		} else if (type == BINDER_TYPE_PTR) {
			struct binder_buffer_object bo;
			memcpy(&bo, data + off, sizeof bo);
			if (nptrs >= MAX_PTRS) goto bad;
			uint64_t p = *pos;
			if (copy_in(stage, pos, bo.buffer, bo.length) || *pos - sg_start > sg_limit) goto bad;
			ptrs[nptrs++] = (struct ptr_copy){ i, p, bo.length };
		} else if (type == BINDER_TYPE_FDA) {
			struct binder_fd_array_object fa;
			memcpy(&fa, data + off, sizeof fa);
			const struct ptr_copy *parent = 0;
			for (int k = 0; k < nptrs; k++)
				if (ptrs[k].index == fa.parent) parent = &ptrs[k];
			if (!parent || fa.parent_offset > parent->len ||
			    (parent->len - fa.parent_offset) / 4 < fa.num_fds ||
			    fa.num_fds > (uint64_t)(BW_MAX_FDS - *nsend))
				goto bad;
			for (uint64_t k = 0; k < fa.num_fds; k++) {
				uint32_t fd;
				memcpy(&fd, stage + parent->pos + fa.parent_offset + k * 4, 4);
				fds[(*nsend)++] = (int)fd;
			}
		}
	}
	sec.sg_size = *pos - sg_start;
	sec.nfds = (uint32_t)(*nsend - nsend_start);
	memcpy(stage + at, &sec, sizeof sec);
	return;
bad:
	*pos = at + sizeof sec;
	*nsend = nsend_start;
	sec.data_size = sec.offsets_size = sec.sg_size = 0;
	sec.flags = BW_TX_BAD;
	memcpy(stage + at, &sec, sizeof sec);
}

static long write_read(struct binder_slot *s, struct thread_slot *t, long arg, ucontext_t *uc)
{
	struct binder_write_read bwr;
	if (tawc_copy_from_guest(&bwr, sizeof bwr, (const void *)arg)) return TAWC_EFAULT;
	if (bwr.write_consumed > bwr.write_size || bwr.read_consumed > bwr.read_size) return TAWC_EINVAL;

	uint8_t *stage = t->stage;
	struct bw_request *rq = (struct bw_request *)stage;
	fill_header(rq, s, BW_REQ_WRITE_READ);
	if (s->nonblock) rq->flags |= BW_F_NONBLOCK;
	if (!bwr.read_consumed) rq->flags |= BW_F_READ_START;
	rq->write_size = bwr.write_size - bwr.write_consumed;
	rq->read_size = bwr.read_size - bwr.read_consumed;
	uint64_t pos = sizeof *rq;
	if (copy_in(stage, &pos, bwr.write_buffer + bwr.write_consumed, rq->write_size)) return TAWC_EFAULT;

	/* Stage each transaction's payload after the BC bytes. */
	int nsend = 0;
	const uint8_t *bc = stage + sizeof *rq;
	for (uint64_t at = 0; rq->write_size - at >= 4;) {
		uint32_t cmd;
		memcpy(&cmd, bc + at, 4);
		uint64_t len = _IOC_SIZE(cmd);
		if (rq->write_size - at - 4 < len) break;
		if (cmd == BC_TRANSACTION || cmd == BC_REPLY ||
		    cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG) {
			struct binder_transaction_data_sg tsg = {0};
			memcpy(&tsg, bc + at + 4, len < sizeof tsg ? len : sizeof tsg);
			uint64_t sg_limit = (cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG) ? tsg.buffers_size : 0;
			stage_transaction(stage, &pos, &tsg.transaction_data, sg_limit, &nsend);
		}
		at += 4 + len;
	}
	rq->total = pos;

	drain_poll(s);
	int nfds = 0;
	long rv = round_trip(t, nsend, &nfds, uc);
	if (rv) return rv;
	const int *fds = (const int *)(stage + SCRATCH_FDS);

	const struct bw_response *r = (const struct bw_response *)(stage + BW_STAGE_REQ);
	const uint8_t *br = stage + BW_STAGE_REQ + sizeof *r;
	const struct bw_fixup *fx = (const struct bw_fixup *)(br + BW_ALIGN8(r->read_len));
	for (uint32_t i = 0; i < r->nfixups && i < BW_MAX_FDS; i++) {
		if (fx[i].fd_index >= (uint32_t)nfds || fx[i].offset > BW_BUFFER_SIZE - 4) continue;
		uint32_t fd = (uint32_t)fds[fx[i].fd_index];
		memcpy(s->alias + fx[i].offset, &fd, 4);
	}
	long status = r->status;
	uint64_t read_len = r->read_len <= rq->read_size ? r->read_len : 0;
	if (read_len && tawc_copy_to_guest((void *)(uintptr_t)(bwr.read_buffer + bwr.read_consumed), br, read_len))
		status = TAWC_EFAULT;
	bwr.write_consumed += r->write_consumed <= rq->write_size ? r->write_consumed : rq->write_size;
	bwr.read_consumed += read_len;
	if (tawc_copy_to_guest((void *)arg, &bwr, sizeof bwr)) return TAWC_EFAULT;
	return status;
}

static long plain_ioctl(struct binder_slot *s, struct thread_slot *t, unsigned int cmd, long arg)
{
	struct bw_request *rq = (struct bw_request *)t->stage;
	fill_header(rq, s, BW_REQ_IOCTL);
	rq->ioctl_cmd = cmd;
	uint32_t size = _IOC_SIZE(cmd);
	if (size > BW_IOCTL_ARG) return TAWC_EINVAL;
	rq->ioctl_len = size;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && size &&
	    tawc_copy_from_guest(rq->ioctl_arg, size, (const void *)arg))
		return TAWC_EFAULT;
	rq->total = sizeof *rq;
	int nfds = 0;
	long rv = round_trip(t, 0, &nfds, 0);
	for (int i = 0; i < nfds; i++) tawc_close(((int *)(t->stage + SCRATCH_FDS))[i]);
	if (rv) return rv;
	const struct bw_response *r = (const struct bw_response *)(t->stage + BW_STAGE_REQ);
	if (!r->status && (_IOC_DIR(cmd) & _IOC_READ) && r->ioctl_len && r->ioctl_len <= size &&
	    tawc_copy_to_guest((void *)arg, r->ioctl_out, r->ioctl_len))
		return TAWC_EFAULT;
	return r->status;
}

long tawcroot_binder_ioctl(int fd, unsigned int cmd, long arg, ucontext_t *uc)
{
	if (_IOC_TYPE(cmd) != 'b') return TAWC_ENOSYS;
	struct binder_slot *s = find_binder(fd);
	if (!s) return TAWC_ENOSYS;
	/* A forked child shares the fd but not the binder process. */
	if (s->owner != self_pid()) return TAWC_EINVAL;
	if (cmd == BINDER_VERSION) {
		struct binder_version v = { BINDER_CURRENT_PROTOCOL_VERSION };
		return tawc_copy_to_guest((void *)arg, &v, sizeof v) ? TAWC_EFAULT : 0;
	}
	if (cmd == BINDER_THREAD_EXIT) {
		drop_thread(s);
		return 0;
	}
	struct thread_slot *t = get_thread(s);
	if (!t) return TAWC_ENOMEM;
	if (cmd == BINDER_WRITE_READ) return write_read(s, t, arg, uc);
	return plain_ioctl(s, t, cmd, arg);
}

/* ---- mmap --------------------------------------------------------------- */

static long handle_mmap(const tawcroot_syscall_args *args, ucontext_t *uc)
{
	(void)uc;
	struct binder_slot *s = find_binder((int)args->e);
	if (!s)
		return TAWC_RAW(TAWC_SYS_mmap, args->a, args->b, args->c, args->d, args->e, args->f);
	/* The kernel's binder_mmap: read-only, one buffer of at most 4 MiB. */
	if (args->c & PROT_WRITE_) return TAWC_EPERM;
	if (args->f || !args->b || (uint64_t)args->b > BW_BUFFER_SIZE) return TAWC_EINVAL;
	if (s->map_size) return TAWC_EBUSY;
	/* Shared, whatever was asked: binderd's writes must show through. */
	long flags = (args->d & ~MAP_TYPE_) | MAP_SHARED_;
	long p = TAWC_RAW(TAWC_SYS_mmap, args->a, args->b, args->c, flags, s->buf_fd, 0);
	if (p < 0 && p > -4096) return p;
	s->map_base = (uint64_t)p;
	s->map_size = (uint64_t)args->b;
	return p;
}

void tawcroot_binder_register(void)
{
	tawcroot_dispatch_install(TAWC_SYS_mmap, handle_mmap);
}
