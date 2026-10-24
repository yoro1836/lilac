/* static_binder_check: two guest processes talking over the emulated
 * /dev/binder (binderd), with raw binder ioctls the way libbinder uses
 * them.
 *
 * Parent: context manager (BINDER_SET_CONTEXT_MGR_EXT, security
 *   context), waits on the binder fd with epoll, serves:
 *     code 3, oneway, scatter-gather: a PTR buffer holding an fd array
 *       (FDA) — checks the copy and writes to the received fd
 *     code 1, sync: data + BINDER object + FD — writes to the fd, takes
 *       a strong ref and a death notification on the binder, calls it
 *       back (nested, must reach the child's waiting thread), replies
 *   then waits for BR_DEAD_BINDER once the child exits.
 * Child: sends both, answers the nested call, checks the reply and
 *   what arrived on its pipes.
 *
 * `static_binder_check dead MARKER`: the parent dies while the child
 * serves its nested call. The child's reply to the nested call may or
 * may not land, but its own call must end in BR_DEAD_REPLY; it then
 * creates MARKER (the parent's exit status is not the result).
 *
 * Exit 42 on success, otherwise the failing step (1..49).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/android/binder.h>

#define MAP_SIZE (1024 * 1024 - 8192)
#define FAIL(n) do { fprintf(stderr, "static_binder_check: step %d (%s:%d) errno=%d\n", n, __FILE__, __LINE__, errno); _exit(n); } while (0)

struct conn {
	int fd;
	uint8_t *map;
	uint8_t out[1024];
	size_t out_len;
	uint8_t in[1024];
	size_t in_len, in_pos;
};

static void open_binder(struct conn *c, int step)
{
	memset(c, 0, sizeof *c);
	c->fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
	if (c->fd < 0) FAIL(step);
	struct binder_version v;
	if (ioctl(c->fd, BINDER_VERSION, &v) < 0 || v.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) FAIL(step);
	c->map = mmap(NULL, MAP_SIZE, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, c->fd, 0);
	if (c->map == MAP_FAILED) FAIL(step);
	uint32_t max = 0;
	if (ioctl(c->fd, BINDER_SET_MAX_THREADS, &max) < 0) FAIL(step);
}

static void put32(struct conn *c, uint32_t v) { memcpy(c->out + c->out_len, &v, 4); c->out_len += 4; }
static void put64(struct conn *c, uint64_t v) { memcpy(c->out + c->out_len, &v, 8); c->out_len += 8; }
static void putbytes(struct conn *c, const void *p, size_t n) { memcpy(c->out + c->out_len, p, n); c->out_len += n; }

/* One BINDER_WRITE_READ: flush `out`, read into `in` when asked. */
static void talk(struct conn *c, int do_read, int step)
{
	struct binder_write_read bwr = {
		.write_size = c->out_len, .write_buffer = (uintptr_t)c->out,
		.read_size = do_read ? sizeof c->in : 0, .read_buffer = (uintptr_t)c->in,
	};
	int rv;
	do rv = ioctl(c->fd, BINDER_WRITE_READ, &bwr);
	while (rv < 0 && errno == EINTR);
	if (rv < 0 || bwr.write_consumed != c->out_len) FAIL(step);
	c->out_len = 0;
	/* A write-only call must not drop commands not yet consumed. */
	if (do_read) {
		c->in_len = bwr.read_consumed;
		c->in_pos = 0;
	}
}

/* Next BR command, reading more when the buffer is drained. */
static uint32_t next_cmd(struct conn *c, void *arg, int step)
{
	while (c->in_pos >= c->in_len) talk(c, 1, step);
	uint32_t cmd;
	memcpy(&cmd, c->in + c->in_pos, 4);
	size_t size = _IOC_SIZE(cmd);
	if (c->in_len - c->in_pos - 4 < size) FAIL(step);
	if (arg) memcpy(arg, c->in + c->in_pos + 4, size);
	c->in_pos += 4 + size;
	return cmd;
}

/* Answer refcount requests for our own nodes; returns other commands. */
static uint32_t next_real(struct conn *c, void *arg, int step)
{
	for (;;) {
		uint8_t buf[sizeof(struct binder_transaction_data_secctx)];
		uint32_t cmd = next_cmd(c, buf, step);
		if (cmd == BR_NOOP || cmd == BR_SPAWN_LOOPER) continue;
		if (cmd == BR_INCREFS || cmd == BR_ACQUIRE) {
			struct binder_ptr_cookie pc;
			memcpy(&pc, buf, sizeof pc);
			put32(c, cmd == BR_INCREFS ? BC_INCREFS_DONE : BC_ACQUIRE_DONE);
			putbytes(c, &pc, sizeof pc);
			talk(c, 0, step);
			continue;
		}
		if (cmd == BR_RELEASE || cmd == BR_DECREFS) continue;
		if (arg) memcpy(arg, buf, _IOC_SIZE(cmd));
		return cmd;
	}
}

static void send_txn(struct conn *c, uint32_t cmd, uint32_t handle, uint32_t code, uint32_t flags,
		     const void *data, size_t data_size, const uint64_t *offs, size_t n_offs, uint64_t sg_size)
{
	struct binder_transaction_data_sg t = {0};
	t.transaction_data.target.handle = handle;
	t.transaction_data.code = code;
	t.transaction_data.flags = flags;
	t.transaction_data.data_size = data_size;
	t.transaction_data.offsets_size = n_offs * 8;
	t.transaction_data.data.ptr.buffer = (uintptr_t)data;
	t.transaction_data.data.ptr.offsets = (uintptr_t)offs;
	t.buffers_size = sg_size;
	put32(c, cmd);
	putbytes(c, &t, _IOC_SIZE(cmd));
}

static void free_buf(struct conn *c, uint64_t ptr)
{
	put32(c, BC_FREE_BUFFER);
	put64(c, ptr);
}

static int child(void)
{
	int pipe_a[2], pipe_b[2];
	if (pipe(pipe_a) || pipe(pipe_b)) FAIL(20);
	struct conn c;
	open_binder(&c, 21);

	/* Oneway scatter-gather: a PTR buffer holding one fd slot plus
	 * text, and an FDA pointing at the slot. */
	struct { uint32_t fd; char text[12]; } sg = { (uint32_t)pipe_b[1], "sg-payload" };
	struct {
		char head[8];
		struct binder_buffer_object ptr;
		struct binder_fd_array_object fda;
	} d3 = { .head = "sgtest" };
	d3.ptr.hdr.type = BINDER_TYPE_PTR;
	d3.ptr.buffer = (uintptr_t)&sg;
	d3.ptr.length = sizeof sg;
	d3.fda.hdr.type = BINDER_TYPE_FDA;
	d3.fda.num_fds = 1;
	d3.fda.parent = 0;
	d3.fda.parent_offset = 0;
	uint64_t offs3[2] = { 8, 8 + sizeof d3.ptr };
	send_txn(&c, BC_TRANSACTION_SG, 0, 3, TF_ONE_WAY, &d3, sizeof d3, offs3, 2, 16);
	talk(&c, 0, 22);
	if (next_real(&c, NULL, 23) != BR_TRANSACTION_COMPLETE) FAIL(23);

	/* Sync call carrying our own binder and a pipe fd. */
	struct {
		char head[8];
		struct flat_binder_object obj;
		struct binder_fd_object fdo;
	} d1 = { .head = "ping" };
	d1.obj.hdr.type = BINDER_TYPE_BINDER;
	d1.obj.binder = 0xB0;
	d1.obj.cookie = 0xB1;
	d1.fdo.hdr.type = BINDER_TYPE_FD;
	d1.fdo.fd = (uint32_t)pipe_a[1];
	uint64_t offs1[2] = { 8, 8 + sizeof d1.obj };
	send_txn(&c, BC_TRANSACTION, 0, 1, TF_ACCEPT_FDS, &d1, sizeof d1, offs1, 2, 0);
	talk(&c, 1, 24);

	int nested = 0;
	for (;;) {
		struct binder_transaction_data tr;
		uint32_t cmd = next_real(&c, &tr, 25);
		if (cmd == BR_TRANSACTION_COMPLETE) continue;
		if (cmd == BR_TRANSACTION) {
			/* The parent's callback, delivered to this blocked thread. */
			if (tr.code != 2 || tr.target.ptr != 0xB0 || tr.cookie != 0xB1) FAIL(26);
			if (tr.data_size != 7 || memcmp((void *)(uintptr_t)tr.data.ptr.buffer, "nested", 7)) FAIL(27);
			free_buf(&c, tr.data.ptr.buffer);
			send_txn(&c, BC_REPLY, 0, 0, 0, "nested-ok", 10, NULL, 0, 0);
			talk(&c, 0, 28);
			nested = 1;
			continue;
		}
		if (cmd != BR_REPLY) FAIL(29);
		if (!nested) FAIL(30);
		if (tr.data_size != 5 || memcmp((void *)(uintptr_t)tr.data.ptr.buffer, "pong", 5)) FAIL(31);
		free_buf(&c, tr.data.ptr.buffer);
		talk(&c, 0, 32);
		break;
	}
	char got[16] = {0};
	close(pipe_a[1]);
	close(pipe_b[1]);
	if (read(pipe_a[0], got, sizeof got) != 6 || strcmp(got, "hello")) FAIL(33);
	memset(got, 0, sizeof got);
	if (read(pipe_b[0], got, sizeof got) != 3 || strcmp(got, "sg")) FAIL(34);
	return 42;
}

/* Child side of `dead`: call the manager, serve the nested call, then
 * expect our own call to fail. */
static int dead_child(const char *marker)
{
	struct conn c;
	open_binder(&c, 43);
	struct { char head[8]; struct flat_binder_object obj; } d = { .head = "ping" };
	d.obj.hdr.type = BINDER_TYPE_BINDER;
	d.obj.binder = 0xC0;
	d.obj.cookie = 0xC1;
	uint64_t offs[1] = { 8 };
	send_txn(&c, BC_TRANSACTION, 0, 1, 0, &d, sizeof d, offs, 1, 0);
	talk(&c, 1, 44);
	struct binder_transaction_data tr;
	uint32_t cmd;
	while ((cmd = next_real(&c, &tr, 45)) == BR_TRANSACTION_COMPLETE) {}
	if (cmd != BR_TRANSACTION || tr.code != 2) FAIL(45);
	free_buf(&c, tr.data.ptr.buffer);
	send_txn(&c, BC_REPLY, 0, 0, 0, "late", 5, NULL, 0, 0);
	talk(&c, 1, 46);
	/* Our reply: delivered (TC) or its sender is gone (DEAD_REPLY). */
	cmd = next_real(&c, NULL, 47);
	if (cmd != BR_TRANSACTION_COMPLETE && cmd != BR_DEAD_REPLY) FAIL(47);
	/* Our own call. */
	while ((cmd = next_real(&c, NULL, 48)) == BR_TRANSACTION_COMPLETE) {}
	if (cmd != BR_DEAD_REPLY) FAIL(48);
	int fd = open(marker, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0) FAIL(49);
	close(fd);
	return 42;
}

/* Parent side of `dead`: take the call, call back, die. */
static int dead_parent(const char *marker)
{
	struct conn c;
	open_binder(&c, 1);
	if (ioctl(c.fd, BINDER_SET_CONTEXT_MGR, &(int){0}) < 0) FAIL(2);
	pid_t pid = fork();
	if (pid < 0) FAIL(3);
	if (pid == 0) _exit(dead_child(marker));
	put32(&c, BC_ENTER_LOOPER);
	talk(&c, 1, 4);
	struct binder_transaction_data tr;
	uint32_t cmd;
	while ((cmd = next_real(&c, &tr, 7)) == BR_TRANSACTION_COMPLETE) {}
	if (cmd != BR_TRANSACTION || tr.code != 1) FAIL(8);
	struct flat_binder_object fo;
	memcpy(&fo, (const uint8_t *)(uintptr_t)tr.data.ptr.buffer + 8, sizeof fo);
	if (fo.hdr.type != BINDER_TYPE_HANDLE) FAIL(9);
	send_txn(&c, BC_TRANSACTION, fo.handle, 2, 0, "nested", 7, NULL, 0, 0);
	talk(&c, 0, 10);
	/* Give the child time to take the nested call, then die with both
	 * transactions open. */
	usleep(200 * 1000);
	_exit(42);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "dead")) return dead_parent(argv[2]);
	struct conn c;
	open_binder(&c, 1);
	struct flat_binder_object mgr = {
		.flags = FLAT_BINDER_FLAG_TXN_SECURITY_CTX | FLAT_BINDER_FLAG_ACCEPTS_FDS,
		.binder = 0xA0, .cookie = 0xA1,
	};
	if (ioctl(c.fd, BINDER_SET_CONTEXT_MGR_EXT, &mgr) < 0) FAIL(2);

	pid_t pid = fork();
	if (pid < 0) FAIL(3);
	if (pid == 0) _exit(child());

	put32(&c, BC_ENTER_LOOPER);
	talk(&c, 0, 4);
	/* servicemanager style: wait on the binder fd before reading. */
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN };
	if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev) < 0) FAIL(5);
	if (epoll_wait(ep, &ev, 1, 10000) != 1) FAIL(6);

	int saw_oneway = 0;
	uint32_t handle = 0;
	for (;;) {
		struct binder_transaction_data_secctx ts;
		struct binder_transaction_data *tr = &ts.transaction_data;
		uint32_t cmd = next_real(&c, &ts, 7);
		if (cmd == BR_TRANSACTION_COMPLETE) continue;
		if (cmd != BR_TRANSACTION_SEC_CTX) FAIL(8);
		if (tr->target.ptr != 0xA0 || tr->cookie != 0xA1) FAIL(9);
		if (!ts.secctx || strncmp((const char *)(uintptr_t)ts.secctx, "u:r:", 4)) FAIL(10);
		const uint8_t *data = (const uint8_t *)(uintptr_t)tr->data.ptr.buffer;
		const uint64_t *offs = (const uint64_t *)(uintptr_t)tr->data.ptr.offsets;
		if (tr->code == 3) {
			if (!(tr->flags & TF_ONE_WAY) || tr->sender_pid != 0 || tr->offsets_size != 16) FAIL(11);
			struct binder_buffer_object bo;
			memcpy(&bo, data + offs[0], sizeof bo);
			if (bo.hdr.type != BINDER_TYPE_PTR || bo.length != 16) FAIL(12);
			/* The copy lives in our buffer, not the child's memory. */
			if (bo.buffer < (uintptr_t)c.map || bo.buffer >= (uintptr_t)c.map + MAP_SIZE) FAIL(13);
			const uint8_t *sgp = (const uint8_t *)(uintptr_t)bo.buffer;
			if (strcmp((const char *)sgp + 4, "sg-payload")) FAIL(14);
			uint32_t fd;
			memcpy(&fd, sgp, 4);
			if (write((int)fd, "sg", 3) != 3) FAIL(15);
			close((int)fd);
			free_buf(&c, tr->data.ptr.buffer);
			talk(&c, 0, 16);
			saw_oneway = 1;
			continue;
		}
		if (tr->code != 1 || !saw_oneway) FAIL(17);
		if (tr->sender_pid <= 0 || memcmp(data, "ping", 5) || tr->offsets_size != 16) FAIL(18);
		struct flat_binder_object fo;
		struct binder_fd_object fdo;
		memcpy(&fo, data + offs[0], sizeof fo);
		memcpy(&fdo, data + offs[1], sizeof fdo);
		if (fo.hdr.type != BINDER_TYPE_HANDLE || fo.handle == 0 || fdo.hdr.type != BINDER_TYPE_FD) FAIL(19);
		if (write((int)fdo.fd, "hello", 6) != 6) FAIL(35);
		close((int)fdo.fd);
		handle = fo.handle;
		put32(&c, BC_ACQUIRE);
		put32(&c, handle);
		put32(&c, BC_REQUEST_DEATH_NOTIFICATION);
		put32(&c, handle);
		put64(&c, 0xDEAD);
		/* Call back into the child while it waits for our reply. */
		send_txn(&c, BC_TRANSACTION, handle, 2, 0, "nested", 7, NULL, 0, 0);
		talk(&c, 1, 36);
		for (;;) {
			struct binder_transaction_data rr;
			uint32_t rc = next_real(&c, &rr, 37);
			if (rc == BR_TRANSACTION_COMPLETE) continue;
			if (rc != BR_REPLY || rr.data_size != 10 ||
			    memcmp((void *)(uintptr_t)rr.data.ptr.buffer, "nested-ok", 10)) FAIL(37);
			free_buf(&c, rr.data.ptr.buffer);
			break;
		}
		free_buf(&c, tr->data.ptr.buffer);
		send_txn(&c, BC_REPLY, 0, 0, 0, "pong", 5, NULL, 0, 0);
		talk(&c, 0, 38);
		break;
	}

	int status = 0;
	if (waitpid(pid, &status, 0) != pid) FAIL(39);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
		fprintf(stderr, "static_binder_check: child exited %d\n", WEXITSTATUS(status));
		FAIL(40);
	}
	for (;;) {
		uint64_t cookie;
		uint32_t cmd = next_real(&c, &cookie, 41);
		if (cmd == BR_TRANSACTION_COMPLETE) continue;
		if (cmd != BR_DEAD_BINDER || cookie != 0xDEAD) FAIL(41);
		put32(&c, BC_DEAD_BINDER_DONE);
		put64(&c, cookie);
		put32(&c, BC_RELEASE);
		put32(&c, handle);
		talk(&c, 0, 41);
		break;
	}
	return 42;
}
