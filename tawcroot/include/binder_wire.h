/* Wire protocol between the in-handler binder shim (src/binder.c) and
 * binderd (binderd/binderd.c). See docs/rootless.md "Binder emulation".
 *
 * binderd listens on one AF_UNIX SOCK_SEQPACKET socket per context,
 * named after the device (`binder`, `hwbinder`, `vndbinder`) in the
 * directory the guest sees as /dev/binderfs.
 *
 * Process connection (the guest's binder fd):
 *   shim → binderd  BW_HELLO + SCM_RIGHTS[buffer memfd], then BW_NEW_THREAD
 *                   + SCM_RIGHTS[thread socket, staging memfd] per thread
 *   binderd → shim  BW_HELLO ack, then BW_POLL bytes while the process
 *                   has work, so poll/epoll on the binder fd works
 *
 * Thread connection (one per guest thread): the shim writes a request
 * into the staging memfd and sends BW_GO with the transaction fds;
 * binderd answers with BW_DONE, the response in the staging memfd and
 * the fds to install. A blocked read is abandoned with BW_CANCEL, which
 * binderd answers with exactly one BW_DONE (status -EINTR unless the
 * real answer was already on its way).
 *
 * Shared by freestanding (shim) and libc (binderd) code: fixed-width
 * types only.
 */

#pragma once

#include <stdint.h>

#define BW_MAGIC   0x56424e44u  /* 'VBND' */
#define BW_VERSION 1

/* The receive buffer the guest mmaps. The kernel caps it at 4 MiB too. */
#define BW_BUFFER_SIZE (4u << 20)

/* Staging memfd per thread connection: request area, then response. */
#define BW_STAGE_REQ   (2u << 20)
#define BW_STAGE_RESP  (256u << 10)
#define BW_STAGE_SIZE  (BW_STAGE_REQ + BW_STAGE_RESP)

/* Fds per BW_GO / BW_DONE message, well under SCM_MAX_FD. */
#define BW_MAX_FDS 64

/* Ioctl argument bytes carried inline (largest: flat_binder_object). */
#define BW_IOCTL_ARG 64

enum bw_type {
	BW_HELLO = 1,
	BW_NEW_THREAD,
	BW_POLL,
	BW_GO,
	BW_DONE,
	BW_CANCEL,
};

enum bw_request_kind {
	BW_REQ_WRITE_READ = 1,
	BW_REQ_IOCTL,
};

#define BW_F_NONBLOCK   1u
#define BW_F_READ_START 2u  /* bwr.read_consumed == 0: lead with BR_NOOP */

/* Control message, every direction. */
struct bw_msg {
	uint32_t magic;
	uint32_t type;
	int32_t  status;     /* BW_HELLO ack */
	int32_t  host_pid;   /* BW_HELLO */
	int32_t  guest_pid;  /* BW_HELLO */
	uint32_t euid;       /* BW_HELLO */
	int32_t  tid;        /* BW_NEW_THREAD */
	uint32_t nfds;       /* fds riding along in SCM_RIGHTS */
};

/* Staging offset 0. */
struct bw_request {
	uint32_t kind;
	uint32_t flags;
	int32_t  guest_pid;
	uint32_t euid;
	uint64_t map_base;      /* guest mapping of the buffer, 0 = none yet */
	uint64_t map_size;
	uint64_t write_size;    /* BC bytes right after this header */
	uint64_t read_size;     /* room for BR bytes */
	uint32_t ioctl_cmd;
	uint32_t ioctl_len;
	uint8_t  ioctl_arg[BW_IOCTL_ARG];
	uint64_t total;         /* bytes used in the request area */
};

/* After the BC bytes, one section per BC_TRANSACTION/BC_REPLY(_SG), in
 * order, 8-byte aligned: header, data, offsets, then the BINDER_TYPE_PTR
 * buffers in object order. Fds follow in object order (FD: one, FDA:
 * num_fds) in the BW_GO message. */
struct bw_tx_section {
	uint64_t data_size;
	uint64_t offsets_size;
	uint64_t sg_size;
	uint32_t nfds;
	uint32_t flags;         /* BW_TX_BAD: shim could not copy, fail it */
};

#define BW_TX_BAD 1u

/* Staging offset BW_STAGE_REQ. */
struct bw_response {
	int32_t  status;        /* 0 or -errno */
	uint32_t nfixups;
	uint64_t write_consumed;
	uint64_t read_len;      /* BR bytes right after this header */
	uint32_t ioctl_len;
	uint8_t  ioctl_out[BW_IOCTL_ARG];
	uint32_t pad;
};

/* After the BR bytes (8-aligned): write fd number `fd_index` of the
 * BW_DONE fds as a u32 at `offset` in the buffer memfd. */
struct bw_fixup {
	uint64_t offset;
	uint32_t fd_index;
	uint32_t pad;
};

#define BW_ALIGN8(x) (((x) + 7u) & ~(uint64_t)7u)
