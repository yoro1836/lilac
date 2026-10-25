/* binderd: the binder driver in userspace, for tawcroot guests.
 *
 * One process serves the binder, hwbinder and vndbinder contexts of one
 * instance. Guest processes reach it through the in-handler shim
 * (src/binder.c) over the protocol in include/binder_wire.h.
 *
 * Semantics follow drivers/android/binder.c: node/ref reference counting
 * with BR_INCREFS/ACQUIRE/RELEASE/DECREFS, transaction stacks with
 * nested-call routing, oneway queues per node, death notifications,
 * BINDER_TYPE_FD/FDA/PTR objects and BR_SPAWN_LOOPER. Not implemented:
 * priority inheritance, freezing, oneway spam detection, async space
 * accounting.
 *
 * Single-threaded, epoll driven.
 * Usage: binderd [-v] [--abstract] [--secctx CTX] DIR
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/android/binder.h>

#include "binder_wire.h"

static int g_verbose;
/* Listen on abstract sockets named like the socket paths, for hosts
 * that forbid socket files in DIR (adb shell in /data/local/tmp). */
static int g_abstract;
static const char *g_secctx = "u:r:untrusted_app:s0";

static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("binderd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (g_verbose) logf_(__VA_ARGS__); } while (0)

static void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n, size);
	if (!p) { LOG("out of memory"); abort(); }
	return p;
}

/* ---- lists ------------------------------------------------------------ */

struct list { struct list *prev, *next; };

#define container_of(p, T, m) ((T *)((char *)(p) - offsetof(T, m)))

static void list_init(struct list *l) { l->prev = l->next = l; }
static bool list_empty(const struct list *l) { return l->next == l; }

static void list_add_tail(struct list *head, struct list *e)
{
	e->prev = head->prev;
	e->next = head;
	head->prev->next = e;
	head->prev = e;
}

static void list_add_head(struct list *head, struct list *e)
{
	e->next = head->next;
	e->prev = head;
	head->next->prev = e;
	head->next = e;
}

static void list_del(struct list *e)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
	list_init(e);
}

static struct list *list_pop(struct list *head)
{
	if (list_empty(head)) return NULL;
	struct list *e = head->next;
	list_del(e);
	return e;
}

/* ---- objects ---------------------------------------------------------- */

enum work_type {
	W_TXN = 1,
	W_COMPLETE,
	W_NODE,
	W_DEAD_BINDER,
	W_DEAD_BINDER_AND_CLEAR,
	W_CLEAR_DEATH,
	W_ERROR,
};

struct work {
	struct list entry;
	int type;
	uint32_t cmd;           /* W_ERROR */
};

struct ctx;
struct proc;
struct thread;
struct node;
struct ref;
struct buffer;

enum conn_kind { C_LISTEN = 1, C_HELLO, C_PROC, C_THREAD };

struct conn {
	int kind;
	int fd;
	void *obj;
};

struct ctx {
	const char *name;
	struct conn listen;
	struct node *mgr;
};

struct proc {
	struct list entry;          /* g_procs */
	struct ctx *ctx;
	struct conn conn;
	int host_pid, guest_pid;
	uint32_t euid;
	uint8_t *buf;               /* our RW mapping of the buffer memfd */
	uint64_t map_base, map_size;
	struct list buffers;        /* sorted by offset */
	struct list threads;
	struct list todo;
	struct list nodes;
	struct list delivered_death;
	struct ref **refs;          /* sorted by desc */
	size_t nrefs, cap_refs;
	uint32_t max_threads, requested_threads, requested_threads_started;
	bool poll_signaled;
	bool dead;
};

enum {
	LOOPER_REGISTERED = 1,
	LOOPER_ENTERED = 2,
	LOOPER_EXITED = 4,
	LOOPER_INVALID = 8,
};

struct thread {
	struct list entry;          /* proc->threads */
	struct proc *proc;
	struct conn conn;
	int tid;
	uint8_t *stage;
	uint32_t looper;
	struct txn *stack;
	struct list todo;
	bool process_todo;          /* todo holds non-deferred work */
	bool need_return;           /* first request returns even without work */
	/* A blocked BINDER_WRITE_READ waiting for work. */
	bool waiting;
	uint32_t wait_flags;
	uint64_t wait_read_size;
	uint64_t wait_write_consumed;
};

struct node {
	struct list entry;          /* proc->nodes, or g_dead_nodes */
	struct proc *proc;          /* NULL once the owner died */
	uint64_t ptr, cookie;
	int internal_strong, local_strong, local_weak, tmp_refs;
	bool has_strong, pending_strong, has_weak, pending_weak;
	struct list refs;
	struct work work;
	bool has_async;
	struct list async_todo;
	bool accept_fds, secctx;
	uint32_t id;
};

struct death {
	struct work work;
	struct ref *ref;            /* NULL once cleared or freed */
	struct proc *proc;
	uint64_t cookie;
};

struct ref {
	struct list node_entry;     /* node->refs */
	struct proc *proc;
	struct node *node;
	uint32_t desc;
	int strong, weak;
	struct death *death;
};

struct txn;

struct buffer {
	struct list entry;          /* proc->buffers */
	uint64_t off, size;
	uint64_t data_size, offsets_size;
	struct txn *txn;
	struct node *target_node;
	bool async;
	bool delivered;             /* user may BC_FREE_BUFFER it */
};

struct txn {
	struct work work;
	struct thread *from;        /* sync sender, NULL for oneway/replies/dead */
	struct txn *from_parent;
	struct txn *to_parent;
	struct proc *to_proc;
	struct thread *to_thread;
	struct buffer *buf;
	uint32_t code, flags;
	int32_t sender_pid;
	uint32_t sender_euid;
	bool is_reply;
	bool secctx;
	uint32_t failed;            /* receiver gave up while buried in from's stack */
	uint64_t secctx_off;        /* offset of the context string in buf */
	int nfds;
	int fds[BW_MAX_FDS];
	uint64_t fd_off[BW_MAX_FDS];
};

static struct ctx g_ctx[] = {
	{ .name = "binder" },
	{ .name = "hwbinder" },
	{ .name = "vndbinder" },
};
#define N_CTX (sizeof g_ctx / sizeof g_ctx[0])

static int g_epoll = -1;
static struct list g_procs;
static struct list g_dead_nodes;
static uint32_t g_node_ids;

static void wake_thread(struct thread *t);
static void wake_proc(struct proc *p);
static void free_txn(struct txn *t);
static void release_buffer(struct proc *p, struct buffer *b, size_t n_objects);
static void free_buffer(struct proc *p, struct buffer *b);

static bool wait_for_proc_work(const struct thread *t)
{
	return !t->stack && list_empty(&t->todo) &&
	       (t->looper & (LOOPER_REGISTERED | LOOPER_ENTERED));
}

/* The kernel's binder_has_work: deferred work alone does not wake. */
static bool has_work(const struct thread *t)
{
	return t->process_todo || t->need_return ||
	       (wait_for_proc_work(t) && !list_empty(&t->proc->todo));
}

static void enqueue_thread(struct thread *t, struct work *w, bool deferred)
{
	list_add_tail(&t->todo, &w->entry);
	if (!deferred) t->process_todo = true;
}

static void enqueue_proc(struct proc *p, struct work *w)
{
	list_add_tail(&p->todo, &w->entry);
}

static struct work *new_work(int type, uint32_t cmd)
{
	struct work *w = xcalloc(1, sizeof *w);
	list_init(&w->entry);
	w->type = type;
	w->cmd = cmd;
	return w;
}

static void queue_error(struct thread *t, uint32_t cmd)
{
	enqueue_thread(t, new_work(W_ERROR, cmd), false);
	wake_thread(t);
}

/* ---- poll readiness ---------------------------------------------------- */

static bool proc_has_poll_work(const struct proc *p)
{
	if (!list_empty(&p->todo)) return true;
	for (struct list *e = p->threads.next; e != &p->threads; e = e->next) {
		const struct thread *t = container_of(e, struct thread, entry);
		if (!t->waiting && t->process_todo) return true;
	}
	return false;
}

static void poll_check(struct proc *p)
{
	if (p->dead || p->poll_signaled || !proc_has_poll_work(p)) return;
	struct bw_msg m = { .magic = BW_MAGIC, .type = BW_POLL };
	if (send(p->conn.fd, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof m)
		p->poll_signaled = true;
}

/* ---- nodes and refs ----------------------------------------------------- */

static struct node *find_node(struct proc *p, uint64_t ptr)
{
	for (struct list *e = p->nodes.next; e != &p->nodes; e = e->next) {
		struct node *n = container_of(e, struct node, entry);
		if (n->ptr == ptr) return n;
	}
	return NULL;
}

static struct node *new_node(struct proc *p, uint64_t ptr, uint64_t cookie, uint32_t flags)
{
	struct node *n = xcalloc(1, sizeof *n);
	n->proc = p;
	n->ptr = ptr;
	n->cookie = cookie;
	n->accept_fds = flags & FLAT_BINDER_FLAG_ACCEPTS_FDS;
	n->secctx = flags & FLAT_BINDER_FLAG_TXN_SECURITY_CTX;
	n->id = ++g_node_ids;
	list_init(&n->refs);
	list_init(&n->async_todo);
	list_init(&n->work.entry);
	n->work.type = W_NODE;
	list_add_tail(&p->nodes, &n->entry);
	return n;
}

static void free_node(struct node *n)
{
	list_del(&n->work.entry);
	list_del(&n->entry);
	while (!list_empty(&n->async_todo)) {
		struct work *w = container_of(list_pop(&n->async_todo), struct work, entry);
		free_txn(container_of(w, struct txn, work));
	}
	free(n);
}

/* Queue the node's BR_INCREFS/ACQUIRE/RELEASE/DECREFS work. A sender's
 * own new node goes to the sending thread, deferred, as the kernel does. */
static void queue_node_work(struct node *n, struct thread *target)
{
	if (!n->proc || !list_empty(&n->work.entry)) return;
	if (target && target->proc == n->proc) {
		enqueue_thread(target, &n->work, true);
	} else {
		enqueue_proc(n->proc, &n->work);
		wake_proc(n->proc);
	}
}

static void inc_node(struct node *n, bool strong, bool internal, struct thread *target)
{
	if (strong) {
		if (internal) n->internal_strong++;
		else n->local_strong++;
		if (!n->has_strong) queue_node_work(n, target);
	} else {
		if (!internal) n->local_weak++;
		if (!n->has_weak) queue_node_work(n, target);
	}
}

/* Returns true when the node was freed. */
static bool dec_node(struct node *n, bool strong, bool internal)
{
	if (strong) {
		if (internal) n->internal_strong--;
		else n->local_strong--;
		if (n->local_strong || n->internal_strong) return false;
	} else {
		if (!internal) n->local_weak--;
		if (n->local_weak || n->tmp_refs || !list_empty(&n->refs)) return false;
	}
	if (n->proc && (n->has_strong || n->has_weak)) {
		queue_node_work(n, NULL);
		return false;
	}
	if (list_empty(&n->refs) && !n->local_strong && !n->local_weak && !n->tmp_refs) {
		free_node(n);
		return true;
	}
	return false;
}

static struct ref *find_ref_desc(struct proc *p, uint32_t desc)
{
	size_t lo = 0, hi = p->nrefs;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (p->refs[mid]->desc < desc) lo = mid + 1;
		else hi = mid;
	}
	return lo < p->nrefs && p->refs[lo]->desc == desc ? p->refs[lo] : NULL;
}

static struct ref *find_ref_node(struct proc *p, struct node *n)
{
	for (struct list *e = n->refs.next; e != &n->refs; e = e->next) {
		struct ref *r = container_of(e, struct ref, node_entry);
		if (r->proc == p) return r;
	}
	return NULL;
}

static struct ref *get_ref_for_node(struct proc *p, struct node *n)
{
	struct ref *r = find_ref_node(p, n);
	if (r) return r;
	r = xcalloc(1, sizeof *r);
	r->proc = p;
	r->node = n;
	r->desc = n == p->ctx->mgr ? 0 : 1;
	size_t at = 0;
	for (; at < p->nrefs; at++) {
		if (p->refs[at]->desc > r->desc) break;
		if (p->refs[at]->desc == r->desc) r->desc++;
	}
	if (p->nrefs == p->cap_refs) {
		p->cap_refs = p->cap_refs ? p->cap_refs * 2 : 16;
		p->refs = realloc(p->refs, p->cap_refs * sizeof *p->refs);
		if (!p->refs) { LOG("out of memory"); abort(); }
	}
	memmove(p->refs + at + 1, p->refs + at, (p->nrefs - at) * sizeof *p->refs);
	p->refs[at] = r;
	p->nrefs++;
	list_add_tail(&n->refs, &r->node_entry);
	return r;
}

static void inc_ref(struct ref *r, bool strong, struct thread *target)
{
	if (strong) {
		if (!r->strong) inc_node(r->node, true, true, target);
		r->strong++;
	} else {
		if (!r->weak) inc_node(r->node, false, true, target);
		r->weak++;
	}
}

static void free_death(struct death *d)
{
	list_del(&d->work.entry);
	free(d);
}

static void cleanup_ref(struct ref *r)
{
	struct proc *p = r->proc;
	for (size_t i = 0; i < p->nrefs; i++) {
		if (p->refs[i] != r) continue;
		memmove(p->refs + i, p->refs + i + 1, (p->nrefs - i - 1) * sizeof *p->refs);
		p->nrefs--;
		break;
	}
	struct node *n = r->node;
	list_del(&r->node_entry);
	/* The strong drop can wake the owner, whose read then runs the
	 * node work right away and would free a node with no refs left:
	 * pin it until the weak drop. */
	n->tmp_refs++;
	if (r->strong) {
		r->strong = 0;
		dec_node(n, true, true);
	}
	n->tmp_refs--;
	dec_node(n, false, true);
	if (r->death) free_death(r->death);
	free(r);
}

/* Returns false for an invalid decrement. */
static bool dec_ref(struct ref *r, bool strong)
{
	if (strong) {
		if (!r->strong) return false;
		r->strong--;
		if (!r->strong && dec_node(r->node, true, true)) {
			/* Cannot happen while the ref is still on node->refs. */
			r->node = NULL;
		}
	} else {
		if (!r->weak) return false;
		r->weak--;
	}
	if (!r->strong && !r->weak) cleanup_ref(r);
	return true;
}

/* ---- buffer allocation in the target's memfd ---------------------------- */

static struct buffer *alloc_buffer(struct proc *p, uint64_t size)
{
	if (!p->map_size) return NULL;
	size = size < 8 ? 8 : BW_ALIGN8(size);
	uint64_t at = 0;
	struct list *before = &p->buffers;
	for (struct list *e = p->buffers.next; e != &p->buffers; e = e->next) {
		struct buffer *b = container_of(e, struct buffer, entry);
		if (b->off - at >= size) { before = e; goto found; }
		at = b->off + b->size;
	}
	if (p->map_size < at || p->map_size - at < size) return NULL;
	before = &p->buffers;
found:;
	struct buffer *b = xcalloc(1, sizeof *b);
	b->off = at;
	b->size = size;
	/* Insert before `before` (the list head means append). */
	b->entry.next = before;
	b->entry.prev = before->prev;
	before->prev->next = &b->entry;
	before->prev = &b->entry;
	return b;
}

static void free_buffer(struct proc *p, struct buffer *b)
{
	(void)p;
	if (b->txn) b->txn->buf = NULL;
	list_del(&b->entry);
	struct node *n = b->target_node;
	free(b);
	if (n) {
		n->tmp_refs--;
		dec_node(n, false, true);
	}
}

static struct buffer *find_buffer(struct proc *p, uint64_t guest_addr)
{
	if (guest_addr < p->map_base) return NULL;
	uint64_t off = guest_addr - p->map_base;
	for (struct list *e = p->buffers.next; e != &p->buffers; e = e->next) {
		struct buffer *b = container_of(e, struct buffer, entry);
		if (b->off == off) return b;
	}
	return NULL;
}

/* ---- transactions ------------------------------------------------------- */

static void free_txn(struct txn *t)
{
	list_del(&t->work.entry);
	if (t->buf) {
		t->buf->txn = NULL;
		/* Never delivered: the target's buffer goes with it. */
		if (!t->buf->delivered && t->to_proc) {
			release_buffer(t->to_proc, t->buf, (size_t)-1);
			free_buffer(t->to_proc, t->buf);
		}
	}
	for (int i = 0; i < t->nfds; i++) close(t->fds[i]);
	free(t);
}

/* A sync transaction is referenced by its sender (t->from, the sender's
 * stack) and, once delivered, its receiver (t->to_thread). It is freed
 * when both have let go. */

/* Fail the sender's outgoing transactions whose receivers gave up while
 * the sender was serving a nested call on top of them. */
static void unwind_failed(struct thread *th)
{
	while (th->stack && th->stack->from == th && th->stack->failed) {
		struct txn *t = th->stack;
		th->stack = t->from_parent;
		t->from = NULL;
		queue_error(th, t->failed);
		free_txn(t);
	}
}

/* The receiver side gives up on `t` (dead receiver, failed reply): hand
 * `cmd` to the waiting sender, the kernel's binder_send_failed_reply. */
static void send_failed_reply(struct txn *t, uint32_t cmd)
{
	t->to_thread = NULL;
	struct thread *from = t->from;
	if (!from) {
		free_txn(t);
	} else if (from->stack == t) {
		from->stack = t->from_parent;
		t->from = NULL;
		queue_error(from, cmd);
		free_txn(t);
		unwind_failed(from);
	} else {
		t->failed = cmd;
	}
}

/* Object sizes by type; 0 for unknown types. */
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

/* Offset of object `i` in b's data, or UINT64_MAX when invalid. */
static uint64_t object_at(struct proc *p, struct buffer *b, size_t i, uint32_t *type)
{
	uint64_t off;
	memcpy(&off, p->buf + b->off + BW_ALIGN8(b->data_size) + i * 8, 8);
	if (off % 4 || off > b->data_size || b->data_size - off < sizeof(struct binder_object_header))
		return UINT64_MAX;
	memcpy(type, p->buf + b->off + off, 4);
	size_t size = object_size(*type);
	if (!size || b->data_size - off < size) return UINT64_MAX;
	return off;
}

/* Drop the references objects [0, n) of a target-form buffer hold. */
static void release_buffer(struct proc *p, struct buffer *b, size_t n)
{
	size_t count = b->offsets_size / 8;
	if (n > count) n = count;
	for (size_t i = 0; i < n; i++) {
		uint32_t type;
		uint64_t off = object_at(p, b, i, &type);
		if (off == UINT64_MAX) continue;
		struct flat_binder_object fo;
		memcpy(&fo, p->buf + b->off + off, sizeof fo);
		if (type == BINDER_TYPE_BINDER || type == BINDER_TYPE_WEAK_BINDER) {
			struct node *node = find_node(p, fo.binder);
			if (node) dec_node(node, type == BINDER_TYPE_BINDER, false);
		} else if (type == BINDER_TYPE_HANDLE || type == BINDER_TYPE_WEAK_HANDLE) {
			struct ref *r = find_ref_desc(p, fo.handle);
			if (r) dec_ref(r, type == BINDER_TYPE_HANDLE);
		}
	}
}

/* Where the request's tx sections and fds are read from. */
struct req_cursor {
	const uint8_t *base;        /* staging */
	uint64_t pos, end;
	const int *fds;
	int nfds, next_fd;
};

struct ptr_slot { uint64_t off, len; };  /* target memfd offset of a PTR copy */

static int take_fd(struct req_cursor *c, int *budget)
{
	if (!*budget || c->next_fd >= c->nfds) return -1;
	(*budget)--;
	return c->fds[c->next_fd++];
}

/* Copy the section into the target buffer and translate its objects.
 * Returns 0 or the BR_* error for the sender. */
static uint32_t translate(struct thread *sender, struct proc *tp, struct txn *t,
			  const struct bw_tx_section *sec, const uint8_t *data,
			  const uint8_t *offsets, const uint8_t *sg,
			  struct req_cursor *c, bool allow_fds)
{
	struct proc *sp = sender->proc;
	struct buffer *b = t->buf;
	uint8_t *dst = tp->buf + b->off;
	memcpy(dst, data, sec->data_size);
	memcpy(dst + BW_ALIGN8(sec->data_size), offsets, sec->offsets_size);
	uint64_t sg_start = BW_ALIGN8(sec->data_size) + BW_ALIGN8(sec->offsets_size);
	uint64_t sg_pos = 0, sg_in = 0;
	int fd_budget = (int)sec->nfds;
	size_t count = sec->offsets_size / 8;
	struct ptr_slot *slots = count ? xcalloc(count, sizeof *slots) : NULL;
	uint32_t err = 0;
	size_t i = 0;
	for (; i < count; i++) {
		uint32_t type;
		uint64_t off = object_at(tp, b, i, &type);
		if (off == UINT64_MAX) { err = BR_FAILED_REPLY; break; }
		uint8_t *obj = dst + off;
		if (type == BINDER_TYPE_BINDER || type == BINDER_TYPE_WEAK_BINDER) {
			struct flat_binder_object fo;
			memcpy(&fo, obj, sizeof fo);
			struct node *n = find_node(sp, fo.binder);
			if (!n) n = new_node(sp, fo.binder, fo.cookie, fo.flags);
			else if (n->cookie != fo.cookie) { err = BR_FAILED_REPLY; break; }
			struct ref *r = get_ref_for_node(tp, n);
			inc_ref(r, type == BINDER_TYPE_BINDER, sender);
			fo.hdr.type = type == BINDER_TYPE_BINDER ? BINDER_TYPE_HANDLE : BINDER_TYPE_WEAK_HANDLE;
			fo.binder = 0;
			fo.handle = r->desc;
			fo.cookie = 0;
			memcpy(obj, &fo, sizeof fo);
		} else if (type == BINDER_TYPE_HANDLE || type == BINDER_TYPE_WEAK_HANDLE) {
			struct flat_binder_object fo;
			memcpy(&fo, obj, sizeof fo);
			struct ref *sr = find_ref_desc(sp, fo.handle);
			if (!sr) { err = BR_FAILED_REPLY; break; }
			struct node *n = sr->node;
			bool strong = type == BINDER_TYPE_HANDLE;
			if (n->proc == tp) {
				fo.hdr.type = strong ? BINDER_TYPE_BINDER : BINDER_TYPE_WEAK_BINDER;
				fo.binder = n->ptr;
				fo.cookie = n->cookie;
				inc_node(n, strong, false, NULL);
			} else {
				struct ref *r = get_ref_for_node(tp, n);
				inc_ref(r, strong, NULL);
				fo.binder = 0;
				fo.handle = r->desc;
				fo.cookie = 0;
			}
			memcpy(obj, &fo, sizeof fo);
		} else if (type == BINDER_TYPE_FD) {
			int fd = take_fd(c, &fd_budget);
			if (fd < 0 || !allow_fds || t->nfds >= BW_MAX_FDS) {
				if (fd >= 0) close(fd);
				err = BR_FAILED_REPLY;
				break;
			}
			t->fds[t->nfds] = fd;
			t->fd_off[t->nfds++] = b->off + off + offsetof(struct binder_fd_object, fd);
		} else if (type == BINDER_TYPE_FDA) {
			struct binder_fd_array_object fa;
			memcpy(&fa, obj, sizeof fa);
			if (fa.parent >= i || !slots[fa.parent].len || fa.parent_offset % 4 ||
			    fa.num_fds > BW_MAX_FDS ||
			    fa.parent_offset > slots[fa.parent].len ||
			    (slots[fa.parent].len - fa.parent_offset) / 4 < fa.num_fds ||
			    !allow_fds) {
				err = BR_FAILED_REPLY;
				break;
			}
			for (uint64_t k = 0; k < fa.num_fds; k++) {
				int fd = take_fd(c, &fd_budget);
				if (fd < 0 || t->nfds >= BW_MAX_FDS) {
					if (fd >= 0) close(fd);
					err = BR_FAILED_REPLY;
					break;
				}
				t->fds[t->nfds] = fd;
				t->fd_off[t->nfds++] = slots[fa.parent].off + fa.parent_offset + k * 4;
			}
			if (err) break;
		} else if (type == BINDER_TYPE_PTR) {
			struct binder_buffer_object bo;
			memcpy(&bo, obj, sizeof bo);
			uint64_t len = BW_ALIGN8(bo.length);
			if (bo.length > sec->sg_size - sg_in || len > b->size - sg_start - sg_pos) {
				err = BR_FAILED_REPLY;
				break;
			}
			uint64_t at = b->off + sg_start + sg_pos;
			memcpy(tp->buf + at, sg + sg_in, bo.length);
			sg_in += BW_ALIGN8(bo.length);
			sg_pos += len;
			slots[i].off = at;
			slots[i].len = bo.length;
			bo.buffer = tp->map_base + at;
			if (bo.flags & BINDER_BUFFER_FLAG_HAS_PARENT) {
				if (bo.parent >= i || !slots[bo.parent].len ||
				    bo.parent_offset > slots[bo.parent].len ||
				    slots[bo.parent].len - bo.parent_offset < 8) {
					err = BR_FAILED_REPLY;
					break;
				}
				memcpy(tp->buf + slots[bo.parent].off + bo.parent_offset, &bo.buffer, 8);
			}
			memcpy(obj, &bo, sizeof bo);
		} else {
			err = BR_FAILED_REPLY;
			break;
		}
	}
	free(slots);
	if (err) release_buffer(tp, b, i);
	/* Fds the objects did not claim. */
	while (fd_budget > 0) {
		int fd = take_fd(c, &fd_budget);
		if (fd >= 0) close(fd);
		else break;
	}
	return err;
}

static void handle_transaction(struct thread *th, const struct binder_transaction_data *tr,
			       bool is_reply, struct req_cursor *c)
{
	struct proc *p = th->proc;
	/* Pull this transaction's section out of the request first, so a
	 * failure below still leaves the cursor at the next one. */
	struct bw_tx_section sec = { .flags = BW_TX_BAD };
	const uint8_t *data = NULL, *offsets = NULL, *sg = NULL;
	if (c->end - c->pos >= sizeof sec) {
		memcpy(&sec, c->base + c->pos, sizeof sec);
		uint64_t need = sizeof sec + BW_ALIGN8(sec.data_size) +
				BW_ALIGN8(sec.offsets_size) + BW_ALIGN8(sec.sg_size);
		if (need > c->end - c->pos || sec.offsets_size % 8) {
			sec.flags |= BW_TX_BAD;
			c->pos = c->end;
		} else {
			data = c->base + c->pos + sizeof sec;
			offsets = data + BW_ALIGN8(sec.data_size);
			sg = offsets + BW_ALIGN8(sec.offsets_size);
			c->pos += need;
		}
	}
	int fd_budget = (int)sec.nfds;
	uint32_t err = 0;
	struct txn *in_reply_to = NULL;
	struct thread *target_thread = NULL;
	struct proc *tp = NULL;
	struct node *target_node = NULL;

	if (sec.flags & BW_TX_BAD) { err = BR_FAILED_REPLY; goto fail; }

	if (is_reply) {
		in_reply_to = th->stack;
		if (!in_reply_to || in_reply_to->to_thread != th) { err = BR_FAILED_REPLY; in_reply_to = NULL; goto fail; }
		th->stack = in_reply_to->to_parent;
		target_thread = in_reply_to->from;
		if (!target_thread) { err = BR_DEAD_REPLY; goto fail; }
		if (target_thread->stack != in_reply_to) { err = BR_FAILED_REPLY; goto fail; }
		tp = target_thread->proc;
	} else {
		if (tr->target.handle == 0) {
			target_node = p->ctx->mgr;
		} else {
			struct ref *r = find_ref_desc(p, tr->target.handle);
			target_node = r ? r->node : NULL;
		}
		if (!target_node) { err = BR_FAILED_REPLY; goto fail; }
		tp = target_node->proc;
		if (!tp) { err = BR_DEAD_REPLY; goto fail; }
		if (!(tr->flags & TF_ONE_WAY) && th->stack) {
			struct txn *tmp = th->stack;
			if (tmp->to_thread != th) { err = BR_FAILED_REPLY; goto fail; }
			for (; tmp; tmp = tmp->from_parent) {
				if (tmp->from && tmp->from->proc == tp) { target_thread = tmp->from; break; }
			}
		}
	}

	bool secctx = !is_reply && target_node && target_node->secctx;
	size_t secctx_len = secctx ? strlen(g_secctx) + 1 : 0;
	uint64_t size = BW_ALIGN8(sec.data_size) + BW_ALIGN8(sec.offsets_size) +
			BW_ALIGN8(sec.sg_size) + BW_ALIGN8(secctx_len);
	struct buffer *b = alloc_buffer(tp, size);
	if (!b) { err = BR_FAILED_REPLY; goto fail; }
	b->data_size = sec.data_size;
	b->offsets_size = sec.offsets_size;
	/* The buffer keeps its target node alive until it is freed, as the
	 * kernel's ref on buffer->target_node does: BC_FREE_BUFFER of a
	 * oneway call and the delivery itself both look at the node. */
	b->target_node = target_node;
	if (target_node) target_node->tmp_refs++;
	b->async = !is_reply && (tr->flags & TF_ONE_WAY);

	struct txn *t = xcalloc(1, sizeof *t);
	list_init(&t->work.entry);
	t->work.type = W_TXN;
	t->buf = b;
	b->txn = t;
	t->to_proc = tp;
	t->code = tr->code;
	t->flags = tr->flags;
	t->is_reply = is_reply;
	t->sender_euid = p->euid;
	if (!is_reply && !(tr->flags & TF_ONE_WAY)) {
		t->from = th;
		t->sender_pid = p->guest_pid;
	}
	bool allow_fds = is_reply ? (in_reply_to->flags & TF_ACCEPT_FDS) : target_node->accept_fds;
	/* translate() consumes this transaction's fds from here on. */
	fd_budget = 0;
	err = translate(th, tp, t, &sec, data, offsets, sg, c, allow_fds);
	if (err) {
		t->buf = NULL;
		b->txn = NULL;
		free_buffer(tp, b);
		free_txn(t);
		goto fail;
	}
	if (secctx) {
		t->secctx = true;
		t->secctx_off = b->off + BW_ALIGN8(sec.data_size) + BW_ALIGN8(sec.offsets_size) +
				BW_ALIGN8(sec.sg_size);
		memcpy(tp->buf + t->secctx_off, g_secctx, secctx_len);
	}

	struct work *tc = new_work(W_COMPLETE, 0);
	if (is_reply) {
		enqueue_thread(th, tc, false);
		target_thread->stack = in_reply_to->from_parent;
		in_reply_to->from = NULL;
		in_reply_to->to_thread = NULL;
		enqueue_thread(target_thread, &t->work, false);
		free_txn(in_reply_to);
		unwind_failed(th);
		unwind_failed(target_thread);
		wake_thread(target_thread);
	} else if (!(tr->flags & TF_ONE_WAY)) {
		enqueue_thread(th, tc, true);
		t->from_parent = th->stack;
		th->stack = t;
		if (target_thread) {
			enqueue_thread(target_thread, &t->work, false);
			wake_thread(target_thread);
		} else {
			enqueue_proc(tp, &t->work);
			wake_proc(tp);
		}
	} else {
		enqueue_thread(th, tc, false);
		if (target_node->has_async) {
			list_add_tail(&target_node->async_todo, &t->work.entry);
		} else {
			target_node->has_async = true;
			enqueue_proc(tp, &t->work);
			wake_proc(tp);
		}
	}
	return;

fail:
	while (fd_budget > 0) {
		int fd = take_fd(c, &fd_budget);
		if (fd >= 0) close(fd);
		else break;
	}
	/* The replier popped it already; a dead sender just drops it. */
	if (in_reply_to) {
		send_failed_reply(in_reply_to, BR_FAILED_REPLY);
		unwind_failed(th);
	}
	DBG("transaction from %d failed: %s", p->guest_pid,
	    err == BR_DEAD_REPLY ? "dead reply" : "failed reply");
	queue_error(th, err);
}

/* ---- BC stream ---------------------------------------------------------- */

static void request_death(struct thread *th, uint32_t handle, uint64_t cookie)
{
	struct proc *p = th->proc;
	struct ref *r = find_ref_desc(p, handle);
	if (!r || r->death) return;
	struct death *d = xcalloc(1, sizeof *d);
	list_init(&d->work.entry);
	d->work.type = W_DEAD_BINDER;
	d->ref = r;
	d->proc = p;
	d->cookie = cookie;
	r->death = d;
	if (!r->node->proc) {
		if (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) {
			enqueue_thread(th, &d->work, false);
		} else {
			enqueue_proc(p, &d->work);
			wake_proc(p);
		}
	}
}

static void clear_death(struct thread *th, uint32_t handle, uint64_t cookie)
{
	struct proc *p = th->proc;
	struct ref *r = find_ref_desc(p, handle);
	if (!r || !r->death || r->death->cookie != cookie) return;
	struct death *d = r->death;
	r->death = NULL;
	d->ref = NULL;
	if (list_empty(&d->work.entry)) {
		d->work.type = W_CLEAR_DEATH;
		if (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) {
			enqueue_thread(th, &d->work, false);
		} else {
			enqueue_proc(p, &d->work);
			wake_proc(p);
		}
	} else {
		d->work.type = W_DEAD_BINDER_AND_CLEAR;
	}
}

static void dead_binder_done(struct thread *th, uint64_t cookie)
{
	struct proc *p = th->proc;
	for (struct list *e = p->delivered_death.next; e != &p->delivered_death; e = e->next) {
		struct death *d = container_of(e, struct death, work.entry);
		if (d->cookie != cookie) continue;
		list_del(&d->work.entry);
		if (d->work.type == W_DEAD_BINDER_AND_CLEAR) {
			d->work.type = W_CLEAR_DEATH;
			if (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) {
				enqueue_thread(th, &d->work, false);
			} else {
				enqueue_proc(p, &d->work);
				wake_proc(p);
			}
		}
		return;
	}
}

static void free_buffer_cmd(struct thread *th, uint64_t addr)
{
	struct proc *p = th->proc;
	struct buffer *b = find_buffer(p, addr);
	if (!b || !b->delivered) {
		DBG("BC_FREE_BUFFER of unknown buffer %#llx", (unsigned long long)addr);
		return;
	}
	if (b->txn) {
		b->txn->buf = NULL;
		b->txn = NULL;
	}
	if (b->async && b->target_node) {
		struct node *n = b->target_node;
		struct list *w = list_pop(&n->async_todo);
		if (!w) {
			n->has_async = false;
		} else if (n->proc) {
			list_add_tail(&n->proc->todo, w);
			wake_proc(n->proc);
		}
	}
	release_buffer(p, b, (size_t)-1);
	free_buffer(p, b);
}

static void node_done(struct thread *th, uint64_t ptr, uint64_t cookie, bool strong)
{
	struct node *n = find_node(th->proc, ptr);
	if (!n || n->cookie != cookie) return;
	if (strong ? !n->pending_strong : !n->pending_weak) return;
	if (strong) n->pending_strong = false;
	else n->pending_weak = false;
	dec_node(n, strong, false);
}

static void update_ref(struct thread *th, uint32_t cmd, uint32_t handle)
{
	struct proc *p = th->proc;
	struct ref *r = find_ref_desc(p, handle);
	bool strong = cmd == BC_ACQUIRE || cmd == BC_RELEASE;
	if (!r && handle == 0 && p->ctx->mgr && p->ctx->mgr->proc != p &&
	    (cmd == BC_INCREFS || cmd == BC_ACQUIRE))
		r = get_ref_for_node(p, p->ctx->mgr);
	if (!r) {
		DBG("refcount op on unknown handle %u", handle);
		return;
	}
	if (cmd == BC_INCREFS || cmd == BC_ACQUIRE) inc_ref(r, strong, NULL);
	else if (!dec_ref(r, strong)) DBG("refcount underflow on handle %u", handle);
}

/* Returns 0 or -errno; *consumed is how far it got. */
static int thread_write(struct thread *th, const uint8_t *buf, uint64_t size,
			uint64_t *consumed, struct req_cursor *c)
{
	struct proc *p = th->proc;
	uint64_t pos = 0;
	while (size - pos >= 4) {
		uint32_t cmd;
		memcpy(&cmd, buf + pos, 4);
		uint64_t len = _IOC_SIZE(cmd);
		if (size - pos - 4 < len) break;
		const uint8_t *arg = buf + pos + 4;
		switch (cmd) {
		case BC_INCREFS:
		case BC_ACQUIRE:
		case BC_RELEASE:
		case BC_DECREFS: {
			uint32_t handle;
			memcpy(&handle, arg, 4);
			update_ref(th, cmd, handle);
			break;
		}
		case BC_INCREFS_DONE:
		case BC_ACQUIRE_DONE: {
			struct binder_ptr_cookie pc;
			memcpy(&pc, arg, sizeof pc);
			node_done(th, pc.ptr, pc.cookie, cmd == BC_ACQUIRE_DONE);
			break;
		}
		case BC_FREE_BUFFER: {
			uint64_t addr;
			memcpy(&addr, arg, 8);
			free_buffer_cmd(th, addr);
			break;
		}
		case BC_TRANSACTION:
		case BC_REPLY:
		case BC_TRANSACTION_SG:
		case BC_REPLY_SG: {
			struct binder_transaction_data tr;
			memcpy(&tr, arg, sizeof tr);
			handle_transaction(th, &tr, cmd == BC_REPLY || cmd == BC_REPLY_SG, c);
			break;
		}
		case BC_REGISTER_LOOPER:
			if (th->looper & LOOPER_ENTERED) {
				th->looper |= LOOPER_INVALID;
			} else if (!p->requested_threads) {
				th->looper |= LOOPER_INVALID;
			} else {
				p->requested_threads--;
				p->requested_threads_started++;
			}
			th->looper |= LOOPER_REGISTERED;
			break;
		case BC_ENTER_LOOPER:
			if (th->looper & LOOPER_REGISTERED) th->looper |= LOOPER_INVALID;
			th->looper |= LOOPER_ENTERED;
			break;
		case BC_EXIT_LOOPER:
			th->looper |= LOOPER_EXITED;
			break;
		case BC_REQUEST_DEATH_NOTIFICATION:
		case BC_CLEAR_DEATH_NOTIFICATION: {
			uint32_t handle;
			uint64_t cookie;
			memcpy(&handle, arg, 4);
			memcpy(&cookie, arg + 4, 8);
			if (cmd == BC_REQUEST_DEATH_NOTIFICATION) request_death(th, handle, cookie);
			else clear_death(th, handle, cookie);
			break;
		}
		case BC_DEAD_BINDER_DONE: {
			uint64_t cookie;
			memcpy(&cookie, arg, 8);
			dead_binder_done(th, cookie);
			break;
		}
		default:
			LOG("unsupported BC command %#x from %d", cmd, p->guest_pid);
			*consumed = pos;
			return -EINVAL;
		}
		pos += 4 + len;
	}
	*consumed = pos;
	return 0;
}

/* ---- BR stream ---------------------------------------------------------- */

struct out {
	uint8_t *buf;
	uint64_t len, cap;
	int fds[BW_MAX_FDS];
	struct bw_fixup fixups[BW_MAX_FDS];
	int nfds;
};

static bool put(struct out *o, uint32_t cmd, const void *arg, size_t len)
{
	if (o->cap - o->len < 4 + len) return false;
	memcpy(o->buf + o->len, &cmd, 4);
	if (len) memcpy(o->buf + o->len + 4, arg, len);
	o->len += 4 + len;
	return true;
}

static bool put_node_cmds(struct out *o, struct node *n)
{
	bool strong = n->internal_strong || n->local_strong;
	bool weak = !list_empty(&n->refs) || n->local_weak || n->tmp_refs || strong;
	bool has_strong = n->has_strong, has_weak = n->has_weak;
	size_t need = 0;
	if (weak && !has_weak) need += 4 + sizeof(struct binder_ptr_cookie);
	if (strong && !has_strong) need += 4 + sizeof(struct binder_ptr_cookie);
	if (!strong && has_strong) need += 4 + sizeof(struct binder_ptr_cookie);
	if (!weak && has_weak) need += 4 + sizeof(struct binder_ptr_cookie);
	if (o->cap - o->len < need) return false;
	struct binder_ptr_cookie pc = { .ptr = n->ptr, .cookie = n->cookie };
	if (weak && !has_weak) {
		n->has_weak = n->pending_weak = true;
		n->local_weak++;
	}
	if (strong && !has_strong) {
		n->has_strong = n->pending_strong = true;
		n->local_strong++;
	}
	if (!strong && has_strong) n->has_strong = false;
	if (!weak && has_weak) n->has_weak = false;
	if (weak && !has_weak) put(o, BR_INCREFS, &pc, sizeof pc);
	if (strong && !has_strong) put(o, BR_ACQUIRE, &pc, sizeof pc);
	if (!strong && has_strong) put(o, BR_RELEASE, &pc, sizeof pc);
	if (!weak && has_weak) put(o, BR_DECREFS, &pc, sizeof pc);
	if (!weak && !strong) free_node(n);
	return true;
}

static bool put_txn(struct out *o, struct thread *th, struct txn *t)
{
	struct proc *p = th->proc;
	struct buffer *b = t->buf;
	size_t len = t->secctx ? sizeof(struct binder_transaction_data_secctx)
			       : sizeof(struct binder_transaction_data);
	if (o->cap - o->len < 4 + len) return false;
	struct binder_transaction_data_secctx td;
	memset(&td, 0, sizeof td);
	struct binder_transaction_data *tr = &td.transaction_data;
	if (!t->is_reply && b && b->target_node) {
		tr->target.ptr = b->target_node->ptr;
		tr->cookie = b->target_node->cookie;
	}
	tr->code = t->code;
	tr->flags = t->flags;
	tr->sender_pid = t->sender_pid;
	tr->sender_euid = t->sender_euid;
	if (b) {
		tr->data_size = b->data_size;
		tr->offsets_size = b->offsets_size;
		tr->data.ptr.buffer = p->map_base + b->off;
		tr->data.ptr.offsets = p->map_base + b->off + BW_ALIGN8(b->data_size);
		b->delivered = true;
	}
	if (t->secctx) td.secctx = p->map_base + t->secctx_off;
	put(o, t->is_reply ? BR_REPLY : (t->secctx ? BR_TRANSACTION_SEC_CTX : BR_TRANSACTION), &td, len);
	for (int i = 0; i < t->nfds; i++) {
		o->fds[o->nfds] = t->fds[i];
		o->fixups[o->nfds].offset = t->fd_off[i];
		o->fixups[o->nfds].fd_index = (uint32_t)o->nfds;
		o->nfds++;
	}
	t->nfds = 0;
	if (b) b->txn = NULL;
	t->buf = NULL;
	if (!t->is_reply && !(t->flags & TF_ONE_WAY)) {
		t->to_thread = th;
		t->to_parent = th->stack;
		th->stack = t;
	} else {
		free_txn(t);
	}
	return true;
}

/* Fill `o` from th's queues; the caller checked has_work(). */
static bool thread_read(struct thread *th, struct out *o, bool at_start)
{
	struct proc *p = th->proc;
	if (at_start) put(o, BR_NOOP, NULL, 0);
	uint64_t start_len = o->len;
	bool done = false;
	while (!done) {
		struct list *e = NULL;
		bool from_proc = false;
		if (!list_empty(&th->todo)) {
			e = th->todo.next;
		} else if (wait_for_proc_work(th) && !list_empty(&p->todo)) {
			e = p->todo.next;
			from_proc = true;
		} else {
			break;
		}
		struct work *w = container_of(e, struct work, entry);
		list_del(e);
		bool ok = true;
		switch (w->type) {
		case W_ERROR:
			ok = put(o, w->cmd, NULL, 0);
			if (ok) { free(w); done = true; }
			break;
		case W_COMPLETE:
			ok = put(o, BR_TRANSACTION_COMPLETE, NULL, 0);
			if (ok) free(w);
			break;
		case W_NODE:
			ok = put_node_cmds(o, container_of(w, struct node, work));
			break;
		case W_DEAD_BINDER:
		case W_DEAD_BINDER_AND_CLEAR:
		case W_CLEAR_DEATH: {
			struct death *d = container_of(w, struct death, work);
			uint32_t cmd = w->type == W_CLEAR_DEATH ? BR_CLEAR_DEATH_NOTIFICATION_DONE : BR_DEAD_BINDER;
			ok = put(o, cmd, &d->cookie, 8);
			if (!ok) break;
			if (w->type == W_CLEAR_DEATH) {
				free(d);
			} else {
				list_add_tail(&p->delivered_death, &d->work.entry);
				done = true;
			}
			break;
		}
		case W_TXN:
			ok = put_txn(o, th, container_of(w, struct txn, work));
			if (ok) done = true;
			break;
		}
		if (!ok) {
			list_add_head(from_proc ? &p->todo : &th->todo, e);
			done = true;
			break;
		}
	}
	if (list_empty(&th->todo)) th->process_todo = false;
	bool any = o->len > start_len;
	if (any && at_start && !p->requested_threads &&
	    p->requested_threads_started < p->max_threads &&
	    (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED))) {
		bool idle_waiting = false;
		for (struct list *t = p->threads.next; t != &p->threads; t = t->next) {
			struct thread *other = container_of(t, struct thread, entry);
			if (other != th && other->waiting && wait_for_proc_work(other)) idle_waiting = true;
		}
		if (!idle_waiting) {
			p->requested_threads++;
			uint32_t spawn = BR_SPAWN_LOOPER;
			memcpy(o->buf, &spawn, 4);
		}
	}
	return any;
}

/* ---- responses ---------------------------------------------------------- */

static void send_done(struct thread *th, struct out *o)
{
	struct msghdr mh = {0};
	struct bw_msg m = { .magic = BW_MAGIC, .type = BW_DONE, .nfds = (uint32_t)o->nfds };
	struct iovec iov = { &m, sizeof m };
	char ctrl[CMSG_SPACE(sizeof(int) * BW_MAX_FDS)];
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	if (o->nfds) {
		memset(ctrl, 0, sizeof ctrl);
		mh.msg_control = ctrl;
		mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)o->nfds);
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)o->nfds);
		memcpy(CMSG_DATA(cm), o->fds, sizeof(int) * (size_t)o->nfds);
	}
	if (sendmsg(th->conn.fd, &mh, MSG_NOSIGNAL) < 0) DBG("send to %d failed: %s", th->tid, strerror(errno));
	for (int i = 0; i < o->nfds; i++) close(o->fds[i]);
	o->nfds = 0;
}

static void out_init(struct out *o, struct thread *th, uint64_t read_size)
{
	uint8_t *resp = th->stage + BW_STAGE_REQ;
	o->buf = resp + sizeof(struct bw_response);
	uint64_t room = BW_STAGE_RESP - sizeof(struct bw_response) - sizeof o->fixups - 8;
	o->cap = read_size < room ? read_size : room;
	o->len = 0;
	o->nfds = 0;
}

static void respond(struct thread *th, struct out *o, int status, uint64_t write_consumed)
{
	struct bw_response r = {
		.status = status,
		.nfixups = (uint32_t)o->nfds,
		.write_consumed = write_consumed,
		.read_len = o->len,
	};
	uint8_t *resp = th->stage + BW_STAGE_REQ;
	memcpy(resp, &r, sizeof r);
	memcpy(resp + sizeof r + BW_ALIGN8(o->len), o->fixups, sizeof(struct bw_fixup) * (size_t)o->nfds);
	send_done(th, o);
	th->need_return = false;
}

static void wake_thread(struct thread *th)
{
	if (th->waiting) {
		static struct out o;
		out_init(&o, th, th->wait_read_size);
		if (has_work(th)) {
			thread_read(th, &o, th->wait_flags & BW_F_READ_START);
			th->waiting = false;
			respond(th, &o, 0, th->wait_write_consumed);
		}
	}
	poll_check(th->proc);
}

static void wake_proc(struct proc *p)
{
	for (struct list *e = p->threads.next; e != &p->threads; e = e->next) {
		struct thread *th = container_of(e, struct thread, entry);
		if (th->waiting && wait_for_proc_work(th)) {
			wake_thread(th);
			if (list_empty(&p->todo)) break;
		}
	}
	poll_check(p);
}

/* ---- ioctls ------------------------------------------------------------- */

static int do_ioctl(struct thread *th, const struct bw_request *rq, struct bw_response *r)
{
	struct proc *p = th->proc;
	switch (rq->ioctl_cmd) {
	case BINDER_SET_MAX_THREADS: {
		uint32_t n;
		memcpy(&n, rq->ioctl_arg, 4);
		p->max_threads = n;
		return 0;
	}
	case BINDER_SET_CONTEXT_MGR:
	case BINDER_SET_CONTEXT_MGR_EXT: {
		if (p->ctx->mgr) return -EBUSY;
		struct flat_binder_object fo = {0};
		if (rq->ioctl_cmd == BINDER_SET_CONTEXT_MGR_EXT) memcpy(&fo, rq->ioctl_arg, sizeof fo);
		struct node *n = new_node(p, fo.binder, fo.cookie, fo.flags);
		n->local_weak++;
		n->local_strong++;
		n->has_strong = n->has_weak = true;
		p->ctx->mgr = n;
		return 0;
	}
	case BINDER_SET_IDLE_TIMEOUT:
	case BINDER_SET_IDLE_PRIORITY:
	case BINDER_ENABLE_ONEWAY_SPAM_DETECTION:
		return 0;
	case BINDER_GET_NODE_INFO_FOR_REF: {
		struct binder_node_info_for_ref info;
		memcpy(&info, rq->ioctl_arg, sizeof info);
		if (!p->ctx->mgr || p->ctx->mgr->proc != p) return -EPERM;
		struct ref *ref = find_ref_desc(p, info.handle);
		if (!ref) return -EINVAL;
		info.strong_count = (uint32_t)(ref->node->local_strong + ref->node->internal_strong);
		info.weak_count = (uint32_t)ref->node->local_weak;
		for (struct list *e = ref->node->refs.next; e != &ref->node->refs; e = e->next)
			info.weak_count++;
		memcpy(r->ioctl_out, &info, sizeof info);
		r->ioctl_len = sizeof info;
		return 0;
	}
	case BINDER_GET_NODE_DEBUG_INFO: {
		struct binder_node_debug_info info;
		memcpy(&info, rq->ioctl_arg, sizeof info);
		uint64_t after = info.ptr;
		struct node *best = NULL;
		for (struct list *e = p->nodes.next; e != &p->nodes; e = e->next) {
			struct node *n = container_of(e, struct node, entry);
			if (n->ptr > after && (!best || n->ptr < best->ptr)) best = n;
		}
		memset(&info, 0, sizeof info);
		if (best) {
			info.ptr = best->ptr;
			info.cookie = best->cookie;
			info.has_strong_ref = best->has_strong;
			info.has_weak_ref = best->has_weak;
		}
		memcpy(r->ioctl_out, &info, sizeof info);
		r->ioctl_len = sizeof info;
		return 0;
	}
	case BINDER_GET_EXTENDED_ERROR: {
		struct binder_extended_error ee = { .command = BR_OK };
		memcpy(r->ioctl_out, &ee, sizeof ee);
		r->ioctl_len = sizeof ee;
		return 0;
	}
	default:
		DBG("unsupported ioctl %#x", rq->ioctl_cmd);
		return -EINVAL;
	}
}

/* ---- connections -------------------------------------------------------- */

static void epoll_add(struct conn *c)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
	if (epoll_ctl(g_epoll, EPOLL_CTL_ADD, c->fd, &ev) < 0) LOG("epoll_ctl: %s", strerror(errno));
}

static void conn_close(struct conn *c)
{
	if (c->fd < 0) return;
	epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
}

/* recvmsg with fds. Returns bytes, 0 on hangup, -1 on error. */
static ssize_t recv_fds(int fd, void *buf, size_t len, int *fds, int *nfds, int max)
{
	char ctrl[CMSG_SPACE(sizeof(int) * BW_MAX_FDS)];
	struct iovec iov = { buf, len };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctrl, .msg_controllen = sizeof ctrl };
	*nfds = 0;
	ssize_t n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
	if (n <= 0) return n;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
		int k = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		int *in = (int *)CMSG_DATA(cm);
		for (int i = 0; i < k; i++) {
			if (*nfds < max) fds[(*nfds)++] = in[i];
			else close(in[i]);
		}
	}
	return n;
}

static void close_fds(int *fds, int n)
{
	for (int i = 0; i < n; i++) close(fds[i]);
}

static void thread_release(struct thread *th)
{
	struct proc *p = th->proc;
	DBG("%s process %d: thread %d gone", p->ctx->name, p->guest_pid, th->tid);
	/* Incoming transactions we owe a reply to fail; outgoing ones lose
	 * their sender. */
	struct txn *t = th->stack;
	while (t) {
		struct txn *next;
		if (t->to_thread == th) {
			next = t->to_parent;
			send_failed_reply(t, BR_DEAD_REPLY);
		} else if (t->from == th) {
			next = t->from_parent;
			t->from = NULL;
			/* Undelivered, or its receiver already gave up. */
			if (!t->to_thread) free_txn(t);
		} else {
			break;
		}
		t = next;
	}
	th->stack = NULL;
	while (!list_empty(&th->todo)) {
		struct work *w = container_of(list_pop(&th->todo), struct work, entry);
		if (w->type == W_TXN) {
			struct txn *tx = container_of(w, struct txn, work);
			if (!tx->is_reply && !(tx->flags & TF_ONE_WAY)) send_failed_reply(tx, BR_DEAD_REPLY);
			else free_txn(tx);
		} else if (w->type == W_COMPLETE || w->type == W_ERROR) {
			free(w);
		} else {
			/* Node and death work belong to the process: hand it back. */
			enqueue_proc(p, w);
		}
	}
	list_del(&th->entry);
	conn_close(&th->conn);
	munmap(th->stage, BW_STAGE_SIZE);
	free(th);
	if (!p->dead) wake_proc(p);
}

static void proc_release(struct proc *p)
{
	p->dead = true;
	DBG("%s process %d died", p->ctx->name, p->guest_pid);
	conn_close(&p->conn);
	if (p->ctx->mgr && p->ctx->mgr->proc == p) p->ctx->mgr = NULL;
	/* Order matters: transactions and buffers still name this
	 * process's nodes and refs, so they go first. */
	while (!list_empty(&p->threads))
		thread_release(container_of(p->threads.next, struct thread, entry));
	while (!list_empty(&p->todo)) {
		struct work *w = container_of(list_pop(&p->todo), struct work, entry);
		if (w->type == W_TXN) {
			struct txn *tx = container_of(w, struct txn, work);
			if (!tx->is_reply && !(tx->flags & TF_ONE_WAY)) send_failed_reply(tx, BR_DEAD_REPLY);
			else free_txn(tx);
		} else if (w->type == W_DEAD_BINDER || w->type == W_DEAD_BINDER_AND_CLEAR || w->type == W_CLEAR_DEATH) {
			struct death *d = container_of(w, struct death, work);
			if (d->ref) d->ref->death = NULL;
			free(d);
		} else if (w->type != W_NODE) {
			free(w);
		}
	}
	while (!list_empty(&p->buffers)) {
		struct buffer *b = container_of(p->buffers.next, struct buffer, entry);
		if (b->txn) {
			b->txn->buf = NULL;
			b->txn = NULL;
		}
		release_buffer(p, b, (size_t)-1);
		free_buffer(p, b);
	}
	/* Nodes: notify death listeners, keep the node while refs remain. */
	while (!list_empty(&p->nodes)) {
		struct node *n = container_of(p->nodes.next, struct node, entry);
		list_del(&n->entry);
		list_del(&n->work.entry);
		n->proc = NULL;
		while (!list_empty(&n->async_todo)) {
			struct work *w = container_of(list_pop(&n->async_todo), struct work, entry);
			free_txn(container_of(w, struct txn, work));
		}
		if (list_empty(&n->refs) && !n->tmp_refs) {
			free(n);
			continue;
		}
		list_add_tail(&g_dead_nodes, &n->entry);
		for (struct list *e = n->refs.next; e != &n->refs; e = e->next) {
			struct ref *r = container_of(e, struct ref, node_entry);
			if (!r->death || !list_empty(&r->death->work.entry)) continue;
			r->death->work.type = W_DEAD_BINDER;
			enqueue_proc(r->proc, &r->death->work);
			wake_proc(r->proc);
		}
	}
	while (p->nrefs) cleanup_ref(p->refs[p->nrefs - 1]);
	while (!list_empty(&p->delivered_death)) {
		struct death *d = container_of(list_pop(&p->delivered_death), struct death, work.entry);
		if (d->ref) d->ref->death = NULL;
		free(d);
	}
	munmap(p->buf, BW_BUFFER_SIZE);
	list_del(&p->entry);
	free(p->refs);
	free(p);
}

static void on_listen(struct ctx *x)
{
	int fd = accept4(x->listen.fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (fd < 0) return;
	struct conn *c = xcalloc(1, sizeof *c);
	c->kind = C_HELLO;
	c->fd = fd;
	c->obj = x;
	epoll_add(c);
}

static void on_hello(struct conn *c)
{
	struct ctx *x = c->obj;
	struct bw_msg m;
	int fds[2], nfds;
	ssize_t n = recv_fds(c->fd, &m, sizeof m, fds, &nfds, 2);
	if (n < 0 && errno == EAGAIN) return;
	if (n != (ssize_t)sizeof m || m.magic != BW_MAGIC || m.type != BW_HELLO || nfds != 1) {
		close_fds(fds, nfds);
		conn_close(c);
		free(c);
		return;
	}
	void *buf = mmap(NULL, BW_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
	close(fds[0]);
	struct bw_msg ack = { .magic = BW_MAGIC, .type = BW_HELLO, .status = buf == MAP_FAILED ? -ENOMEM : 0 };
	(void)send(c->fd, &ack, sizeof ack, MSG_NOSIGNAL);
	if (buf == MAP_FAILED) {
		conn_close(c);
		free(c);
		return;
	}
	struct proc *p = xcalloc(1, sizeof *p);
	p->ctx = x;
	p->host_pid = m.host_pid;
	p->guest_pid = m.guest_pid;
	p->euid = m.euid;
	p->buf = buf;
	list_init(&p->buffers);
	list_init(&p->threads);
	list_init(&p->todo);
	list_init(&p->nodes);
	list_init(&p->delivered_death);
	list_add_tail(&g_procs, &p->entry);
	/* Reuse the accepted socket as the process connection. */
	epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, NULL);
	p->conn.kind = C_PROC;
	p->conn.fd = c->fd;
	p->conn.obj = p;
	free(c);
	epoll_add(&p->conn);
	DBG("%s process %d connected", x->name, p->guest_pid);
}

static void on_proc(struct proc *p)
{
	struct bw_msg m;
	int fds[2], nfds;
	ssize_t n = recv_fds(p->conn.fd, &m, sizeof m, fds, &nfds, 2);
	if (n < 0 && errno == EAGAIN) return;
	if (n <= 0) {
		close_fds(fds, nfds);
		proc_release(p);
		return;
	}
	if (n != (ssize_t)sizeof m || m.magic != BW_MAGIC || m.type != BW_NEW_THREAD || nfds != 2) {
		close_fds(fds, nfds);
		return;
	}
	void *stage = mmap(NULL, BW_STAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fds[1], 0);
	close(fds[1]);
	if (stage == MAP_FAILED) {
		close(fds[0]);
		return;
	}
	struct thread *th = xcalloc(1, sizeof *th);
	th->proc = p;
	th->tid = m.tid;
	th->stage = stage;
	th->need_return = true;
	list_init(&th->todo);
	th->conn.kind = C_THREAD;
	th->conn.fd = fds[0];
	th->conn.obj = th;
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	list_add_tail(&p->threads, &th->entry);
	epoll_add(&th->conn);
	DBG("%s process %d: thread %d", p->ctx->name, p->guest_pid, th->tid);
}

static void on_request(struct thread *th, const int *fds, int nfds)
{
	struct proc *p = th->proc;
	struct bw_request rq;
	memcpy(&rq, th->stage, sizeof rq);
	if (rq.total > BW_STAGE_REQ || rq.write_size > rq.total - sizeof rq) {
		close_fds((int *)fds, nfds);
		static struct out bad;
		out_init(&bad, th, 0);
		respond(th, &bad, -EINVAL, 0);
		return;
	}
	p->guest_pid = rq.guest_pid;
	p->euid = rq.euid;
	if (rq.map_size && !p->map_size && rq.map_size <= BW_BUFFER_SIZE) {
		p->map_base = rq.map_base;
		p->map_size = rq.map_size;
	}
	p->poll_signaled = false;

	static struct out o;
	out_init(&o, th, rq.kind == BW_REQ_WRITE_READ ? rq.read_size : 0);
	if (rq.kind == BW_REQ_IOCTL) {
		close_fds((int *)fds, nfds);
		struct bw_response r = {0};
		int status = do_ioctl(th, &rq, &r);
		uint8_t *resp = th->stage + BW_STAGE_REQ;
		r.status = status;
		memcpy(resp, &r, sizeof r);
		send_done(th, &o);
		th->need_return = false;
		poll_check(p);
		return;
	}

	struct req_cursor c = {
		.base = th->stage,
		.pos = sizeof rq + BW_ALIGN8(rq.write_size),
		.end = rq.total,
		.fds = fds,
		.nfds = nfds,
	};
	uint64_t consumed = 0;
	int status = thread_write(th, th->stage + sizeof rq, rq.write_size, &consumed, &c);
	for (int i = c.next_fd; i < nfds; i++) close(fds[i]);
	if (status || !rq.read_size) {
		respond(th, &o, status, consumed);
		poll_check(p);
		return;
	}
	if (has_work(th)) {
		thread_read(th, &o, rq.flags & BW_F_READ_START);
		respond(th, &o, 0, consumed);
	} else if (rq.flags & BW_F_NONBLOCK) {
		o.len = 0;
		respond(th, &o, -EAGAIN, consumed);
	} else {
		th->waiting = true;
		th->wait_flags = rq.flags;
		th->wait_read_size = rq.read_size;
		th->wait_write_consumed = consumed;
	}
	poll_check(p);
}

static void on_thread(struct thread *th)
{
	struct bw_msg m;
	int fds[BW_MAX_FDS], nfds;
	ssize_t n = recv_fds(th->conn.fd, &m, sizeof m, fds, &nfds, BW_MAX_FDS);
	if (n < 0 && errno == EAGAIN) return;
	if (n <= 0) {
		close_fds(fds, nfds);
		thread_release(th);
		return;
	}
	if (n != (ssize_t)sizeof m || m.magic != BW_MAGIC) {
		close_fds(fds, nfds);
		return;
	}
	if (m.type == BW_CANCEL) {
		close_fds(fds, nfds);
		if (th->waiting) {
			th->waiting = false;
			static struct out o;
			out_init(&o, th, 0);
			respond(th, &o, -EINTR, th->wait_write_consumed);
		}
		return;
	}
	if (m.type != BW_GO || th->waiting) {
		close_fds(fds, nfds);
		return;
	}
	on_request(th, fds, nfds);
}

/* ---- setup -------------------------------------------------------------- */

static int listen_at(const char *dir, const char *name)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char *name_at = sa.sun_path + (g_abstract ? 1 : 0);
	int n = snprintf(name_at, sizeof sa.sun_path - (g_abstract ? 1 : 0), "%s/%s", dir, name);
	if (n < 0 || (size_t)n >= sizeof sa.sun_path - (g_abstract ? 1 : 0)) {
		LOG("socket path too long: %s/%s", dir, name);
		return -1;
	}
	socklen_t len = sizeof sa;
	if (g_abstract) len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
	else unlink(sa.sun_path);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, len) < 0 || listen(fd, 64) < 0) {
		LOG("listen on %s%s: %s", g_abstract ? "@" : "", name_at, strerror(errno));
		if (fd >= 0) close(fd);
		return -1;
	}
	return fd;
}

/* binderfs feature files libbinder reads. */
static void write_features(const char *dir)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/features", dir);
	mkdir(path, 0755);
	static const struct { const char *name, *value; } features[] = {
		{ "oneway_spam_detection", "0\n" },
		{ "extended_error", "1\n" },
		{ "freeze_notification", "0\n" },
	};
	for (size_t i = 0; i < sizeof features / sizeof features[0]; i++) {
		snprintf(path, sizeof path, "%s/features/%s", dir, features[i].name);
		FILE *f = fopen(path, "w");
		if (!f) continue;
		fputs(features[i].value, f);
		fclose(f);
	}
}

static size_t list_len(const struct list *l)
{
	size_t n = 0;
	for (const struct list *e = l->next; e != l; e = e->next) n++;
	return n;
}

/* SIGUSR1: every live process, its threads and their transaction
 * stacks, for diagnosing a hung guest. */
static void dump_state(void)
{
	for (struct list *e = g_procs.next; e != &g_procs; e = e->next) {
		struct proc *p = container_of(e, struct proc, entry);
		LOG("%s pid %d (host %d)%s todo %zu threads %zu", p->ctx->name, p->guest_pid,
		    p->host_pid, p->dead ? " dead" : "", list_len(&p->todo), list_len(&p->threads));
		for (struct list *t = p->threads.next; t != &p->threads; t = t->next) {
			struct thread *th = container_of(t, struct thread, entry);
			LOG("  tid %d looper %#x%s todo %zu", th->tid, th->looper,
			    th->waiting ? " waiting" : "", list_len(&th->todo));
			for (struct txn *x = th->stack; x; x = x->to_thread == th ? x->to_parent : x->from_parent)
				LOG("    txn code %u flags %#x from %d to %d (pid %d)%s", x->code, x->flags,
				    x->from ? x->from->tid : 0, x->to_thread ? x->to_thread->tid : 0,
				    x->to_proc ? x->to_proc->guest_pid : 0, x->is_reply ? " reply" : "");
		}
	}
}

static volatile sig_atomic_t g_stop, g_dump;
static void on_signal(int sig) { (void)sig; g_stop = 1; }
static void on_dump(int sig) { (void)sig; g_dump = 1; }

int main(int argc, char **argv)
{
	const char *dir = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else if (!strcmp(argv[i], "--abstract")) g_abstract = 1;
		else if (!strcmp(argv[i], "--secctx") && i + 1 < argc) g_secctx = argv[++i];
		else if (!dir && argv[i][0] != '-') dir = argv[i];
		else {
			fprintf(stderr, "usage: binderd [-v] [--abstract] [--secctx CTX] DIR\n");
			return 2;
		}
	}
	if (!dir) {
		fprintf(stderr, "usage: binderd [-v] [--abstract] [--secctx CTX] DIR\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	struct sigaction da = { .sa_handler = on_dump };
	sigaction(SIGUSR1, &da, NULL);

	list_init(&g_procs);
	list_init(&g_dead_nodes);
	g_epoll = epoll_create1(EPOLL_CLOEXEC);
	if (g_epoll < 0) {
		LOG("epoll_create1: %s", strerror(errno));
		return 1;
	}
	mkdir(dir, 0755);
	for (size_t i = 0; i < N_CTX; i++) {
		g_ctx[i].listen.kind = C_LISTEN;
		g_ctx[i].listen.obj = &g_ctx[i];
		g_ctx[i].listen.fd = listen_at(dir, g_ctx[i].name);
		if (g_ctx[i].listen.fd < 0) return 1;
		epoll_add(&g_ctx[i].listen);
	}
	write_features(dir);
	/* Ready: whoever started us can connect now. */
	char ready[4096];
	snprintf(ready, sizeof ready, "%s/ready", dir);
	FILE *rf = fopen(ready, "w");
	if (rf) fclose(rf);

	while (!g_stop) {
		/* One at a time: a handler may free connections a later
		 * event in the same batch would name. */
		struct epoll_event ev[1];
		int n = epoll_wait(g_epoll, ev, 1, -1);
		if (g_dump) { g_dump = 0; dump_state(); }
		if (n < 0) {
			if (errno == EINTR) continue;
			LOG("epoll_wait: %s", strerror(errno));
			return 1;
		}
		for (int i = 0; i < n; i++) {
			struct conn *c = ev[i].data.ptr;
			switch (c->kind) {
			case C_LISTEN: on_listen(c->obj); break;
			case C_HELLO:  on_hello(c); break;
			case C_PROC:   on_proc(c->obj); break;
			case C_THREAD: on_thread(c->obj); break;
			}
		}
	}
	for (size_t i = 0; i < N_CTX; i++) {
		char path[4096];
		snprintf(path, sizeof path, "%s/%s", dir, g_ctx[i].name);
		if (!g_abstract) unlink(path);
	}
	unlink(ready);
	return 0;
}
