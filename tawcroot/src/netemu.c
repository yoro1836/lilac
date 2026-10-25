/* `--fake-net`: the guest sees a loopback and one wired link, eth0,
 * configured over rtnetlink and SIOC* ioctls as on a real kernel, while
 * its traffic goes out through the app's own sockets. An app can't read
 * or change the host's links (NETLINK_ROUTE binds and dumps are denied),
 * and Android's netd / EthernetTracker / IpClient need exactly that to
 * bring up a network, so without this ConnectivityService has none.
 *
 * NETLINK_ROUTE sockets are AF_UNIX datagram stubs above the reserved fd
 * floor, bound to "\0tawcrtnl.<init>.<portid>". A request written to one
 * is answered in the writing process against the session's link and
 * address table (<store>/net.state, flock'd), the replies queued on the
 * stub itself; changes also go to every stub in the matching multicast
 * group. Each datagram starts with a group word that the receive side
 * strips into the sockaddr_nl it fabricates: kernel source (nl_pid 0)
 * plus SCM_CREDENTIALS, which uevent_kernel_recv (netd) insists on.
 *
 * Routes, rules, neighbours and qdiscs are acknowledged and dropped:
 * the host routes. /sys/class/net and /proc/sys/net are small fake
 * trees for netd's interface list and per-interface knobs. */
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <asm/unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "cgroup.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "filter.h"
#include "io.h"
#include "blocking.h"
#include "linkstore.h"
#include "namespace.h"
#include "netemu.h"
#include "path.h"
#include "path_scratch.h"
#include "ports.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#define NE_UNIX 1
#define NE_INET 2
#define NE_INET6 10
#define NE_NETLINK 16
#define NE_DGRAM 2
#define NE_CLOEXEC 02000000
#define NE_MSG_CTRUNC 0x8
#define NE_MSG_DONTWAIT 0x40
#define NE_SOL_SOCKET 1
#define NE_SOL_NETLINK 270
#define NE_SO_BINDTODEVICE 25
#define NE_SO_MARK 36
#define NE_SO_DOMAIN 39
#define NE_SO_BINDTOIFINDEX 62
#define NE_SCM_CREDENTIALS 2

#define NE_IFF_UP 0x1
#define NE_IFF_BROADCAST 0x2
#define NE_IFF_LOOPBACK 0x8
#define NE_IFF_RUNNING 0x40
#define NE_IFF_MULTICAST 0x1000
#define NE_IFF_LOWER_UP 0x10000
/* What SIOCSIFFLAGS / RTM_SETLINK may change: UP, NOARP, PROMISC,
 * ALLMULTI, MULTICAST. */
#define NE_IFF_SETTABLE 0x1381u
#define NE_ARPHRD_ETHER 1
#define NE_ARPHRD_LOOPBACK 772
#define NE_OPER_DOWN 2
#define NE_OPER_UP 6

#define NE_MAGIC 0x4e455431u
#define NE_LINKS 2
#define NE_ADDRS 32
#define NE_SUBS 64
#define NE_MAX_IOV 16
#define NE_OUT_CAP TAWCROOT_PATH_SCRATCH_SIZE

struct ne_link { int32_t index; uint32_t flags, mtu; uint16_t type; uint8_t mac[6]; char name[16]; };
struct ne_addr { int32_t index; uint8_t family, prefix, scope, used; uint32_t flags; uint8_t addr[16]; };
struct ne_sub { uint32_t port, groups; };
struct ne_state {
	uint32_t magic, next_port;
	struct ne_link link[NE_LINKS];
	struct ne_addr addr[NE_ADDRS];
	struct ne_sub sub[NE_SUBS];
};
struct ne_map { long fd; struct ne_state *s; };
struct ne_un { uint16_t family; char path[108]; };
struct ne_nl { uint16_t family, pad; uint32_t pid, groups; };
struct ne_iov { uint64_t base, len; };
struct ne_mh {
	uint64_t name;
	uint32_t namelen, pad;
	uint64_t iov, iovlen, control, controllen;
	int32_t flags;
	uint32_t pad2;
};
struct ne_ifreq {
	char name[16];
	union {
		struct { uint16_t family; uint8_t data[14]; } sa;
		int16_t flags;
		int32_t ival;
		uint8_t raw[24];
	} u;
};
_Static_assert(sizeof(struct ne_ifreq) == 40, "struct ifreq is 40 bytes");

static const uint8_t eth_mac[6] = { 0x02, 0x00, 0x00, 0x57, 0x44, 0x01 };

static int enabled(void)
{
	return (tawcroot_namespace.session & TAWC_NS_FAKE_NET) != 0;
}

static void defaults(struct ne_state *s)
{
	memset(s, 0, sizeof *s);
	s->magic = NE_MAGIC;
	struct ne_link *lo = &s->link[0], *eth = &s->link[1];
	lo->index = 1;
	lo->flags = NE_IFF_UP | NE_IFF_LOOPBACK | NE_IFF_RUNNING | NE_IFF_LOWER_UP;
	lo->mtu = 65536;
	lo->type = NE_ARPHRD_LOOPBACK;
	memcpy(lo->name, "lo", 3);
	eth->index = TAWCROOT_NETEMU_ETH_INDEX;
	eth->flags = NE_IFF_BROADCAST | NE_IFF_MULTICAST;
	eth->mtu = 1500;
	eth->type = NE_ARPHRD_ETHER;
	memcpy(eth->mac, eth_mac, 6);
	memcpy(eth->name, "eth0", 5);
	struct ne_addr *a = &s->addr[0];
	a->index = 1; a->family = NE_INET; a->prefix = 8; a->scope = RT_SCOPE_HOST;
	a->used = 1; a->flags = IFA_F_PERMANENT;
	a->addr[0] = 127; a->addr[3] = 1;
	a = &s->addr[1];
	a->index = 1; a->family = NE_INET6; a->prefix = 128; a->scope = RT_SCOPE_HOST;
	a->used = 1; a->flags = IFA_F_PERMANENT;
	a->addr[15] = 1;
}

static long store_file(char *out, const char *name)
{
	size_t n = 0;
	if (!tawcroot_store_host_path_len) return TAWC_ENOENT;
	if (tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) ||
	    tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, name))
		return TAWC_ENAMETOOLONG;
	return 0;
}

static long state_open(struct ne_map *m, int reset)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	long fd = store_file(path, "/net.state");
	if (fd < 0) return fd;
	fd = tawc_openat(AT_FDCWD, path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return fd;
	long e = TAWC_RAW(__NR_flock, fd, 2 /*LOCK_EX*/, 0, 0, 0, 0);
	if (!e) e = TAWC_RAW(TAWC_SYS_ftruncate, fd, sizeof(struct ne_state), 0, 0, 0, 0);
	if (e < 0) { tawc_close((int)fd); return e; }
	long p = tawc_mmap(0, sizeof(struct ne_state), PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
	if (p < 0 && p >= -4095) { tawc_close((int)fd); return p; }
	m->fd = fd;
	m->s = (struct ne_state *)p;
	if (reset || m->s->magic != NE_MAGIC) defaults(m->s);
	return 0;
}

static void state_close(struct ne_map *m)
{
	tawc_munmap(m->s, sizeof(struct ne_state));
	tawc_close((int)m->fd);
}

static size_t stub_prefix(char *out, size_t cap)
{
	size_t n = 0;
	(void)tawc_str_append(out, cap, &n, "tawcrtnl.");
	(void)tawc_str_append_dec(out, cap, &n, tawcroot_namespace.init_pid);
	(void)tawc_str_append(out, cap, &n, ".");
	return n;
}

static long stub_addr(struct ne_un *un, uint32_t port)
{
	memset(un, 0, sizeof *un);
	un->family = NE_UNIX;
	size_t n = 1 + stub_prefix(un->path + 1, sizeof un->path - 1);
	(void)tawc_str_append_dec(un->path, sizeof un->path, &n, (long)port);
	return (long)(sizeof(uint16_t) + n);
}

/* The port id of a netlink stub, or 0 when `fd` is not one. */
static uint32_t stub_port(long fd)
{
	if (!enabled() || fd < TAWCROOT_RESERVED_FD_BASE) return 0;
	struct ne_un un;
	uint32_t len = sizeof un;
	if (TAWC_RAW(TAWC_SYS_getsockname, fd, (long)&un, (long)&len, 0, 0, 0) < 0 ||
	    un.family != NE_UNIX || len <= sizeof(uint16_t) + 1 || un.path[0])
		return 0;
	char pre[48];
	size_t n = stub_prefix(pre, sizeof pre);
	size_t end = len - sizeof(uint16_t);
	if (end <= 1 + n || memcmp(un.path + 1, pre, n)) return 0;
	uint64_t port = 0;
	for (size_t i = 1 + n; i < end; ++i) {
		char c = un.path[i];
		if (c < '0' || c > '9') return 0;
		port = port * 10 + (uint64_t)(c - '0');
	}
	return (uint32_t)port;
}

static struct ne_link *link_by(struct ne_state *s, int index, const char *name)
{
	for (int i = 0; i < NE_LINKS; ++i) {
		struct ne_link *l = &s->link[i];
		if (index ? l->index == index : (name && tawc_streq(l->name, name))) return l;
	}
	return 0;
}

static struct ne_link *link_of(struct ne_state *s, const struct ne_addr *a)
{
	return link_by(s, a->index, 0);
}

/* ---- message building ---- */

struct ne_msg { _Alignas(8) char b[512]; size_t n; };

static void msg_start(struct ne_msg *m, uint16_t type, uint16_t flags, uint32_t seq,
		      uint32_t pid, const void *fixed, size_t flen)
{
	memset(m->b, 0, sizeof m->b);
	struct nlmsghdr *h = (struct nlmsghdr *)m->b;
	h->nlmsg_type = type;
	h->nlmsg_flags = flags;
	h->nlmsg_seq = seq;
	h->nlmsg_pid = pid;
	memcpy(m->b + NLMSG_HDRLEN, fixed, flen);
	m->n = NLMSG_HDRLEN + NLMSG_ALIGN(flen);
	h->nlmsg_len = (uint32_t)m->n;
}

static void attr(struct ne_msg *m, uint16_t type, const void *data, size_t len)
{
	struct rtattr *a = (struct rtattr *)(m->b + m->n);
	a->rta_type = type;
	a->rta_len = (unsigned short)RTA_LENGTH(len);
	memcpy(RTA_DATA(a), data, len);
	m->n += RTA_ALIGN(a->rta_len);
	((struct nlmsghdr *)m->b)->nlmsg_len = (uint32_t)m->n;
}

static void attr32(struct ne_msg *m, uint16_t type, uint32_t v) { attr(m, type, &v, 4); }
static void attr8(struct ne_msg *m, uint16_t type, uint8_t v) { attr(m, type, &v, 1); }

static void link_msg(struct ne_msg *m, const struct ne_link *l, uint16_t flags,
		     uint32_t seq, uint32_t pid, uint32_t change)
{
	struct ifinfomsg ifi;
	memset(&ifi, 0, sizeof ifi);
	ifi.ifi_type = l->type;
	ifi.ifi_index = l->index;
	ifi.ifi_flags = l->flags;
	ifi.ifi_change = change;
	msg_start(m, RTM_NEWLINK, flags, seq, pid, &ifi, sizeof ifi);
	attr(m, IFLA_IFNAME, l->name, tawc_strlen(l->name) + 1);
	attr(m, IFLA_ADDRESS, l->mac, 6);
	static const uint8_t ones[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t zeros[6];
	attr(m, IFLA_BROADCAST, l->type == NE_ARPHRD_ETHER ? ones : zeros, 6);
	attr32(m, IFLA_MTU, l->mtu);
	attr32(m, IFLA_TXQLEN, 1000);
	attr8(m, IFLA_OPERSTATE, l->type == NE_ARPHRD_LOOPBACK ? 0 :
	      (l->flags & NE_IFF_RUNNING) ? NE_OPER_UP : NE_OPER_DOWN);
	attr8(m, IFLA_LINKMODE, 0);
	attr8(m, IFLA_CARRIER, 1);
}

static size_t addr_len(uint8_t family) { return family == NE_INET ? 4 : 16; }

static void addr_msg(struct ne_msg *m, struct ne_state *s, const struct ne_addr *a,
		     uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid)
{
	struct ifaddrmsg ifa;
	memset(&ifa, 0, sizeof ifa);
	ifa.ifa_family = a->family;
	ifa.ifa_prefixlen = a->prefix;
	ifa.ifa_flags = (uint8_t)a->flags;
	ifa.ifa_scope = a->scope;
	ifa.ifa_index = (uint32_t)a->index;
	msg_start(m, type, flags, seq, pid, &ifa, sizeof ifa);
	attr(m, IFA_ADDRESS, a->addr, addr_len(a->family));
	if (a->family == NE_INET) {
		attr(m, IFA_LOCAL, a->addr, 4);
		if (a->prefix < 31) {
			uint8_t b[4];
			uint32_t host = a->prefix ? 0xffffffffu >> a->prefix : 0xffffffffu;
			for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(a->addr[i] | (host >> (24 - 8 * i)));
			attr(m, IFA_BROADCAST, b, 4);
		}
		const struct ne_link *l = link_of(s, a);
		if (l) attr(m, IFA_LABEL, l->name, tawc_strlen(l->name) + 1);
	}
	struct ifa_cacheinfo ci = { 0xffffffffu, 0xffffffffu, 0, 0 };
	attr(m, IFA_CACHEINFO, &ci, sizeof ci);
	attr32(m, IFA_FLAGS, a->flags);
}

/* ---- delivery ---- */

/* Queue `iov` (group word first) on the stub named by `port`, from `fd`
 * (any unbound AF_UNIX datagram socket will do). */
static long deliver(long fd, uint32_t port, struct ne_iov *iov, int n)
{
	struct ne_un un;
	long ul = stub_addr(&un, port);
	struct ne_mh mh;
	memset(&mh, 0, sizeof mh);
	mh.name = (uint64_t)(uintptr_t)&un;
	mh.namelen = (uint32_t)ul;
	mh.iov = (uint64_t)(uintptr_t)iov;
	mh.iovlen = (uint64_t)n;
	return TAWC_RAW(TAWC_SYS_sendmsg, fd, (long)&mh, NE_MSG_DONTWAIT, 0, 0, 0);
}

/* Multicast `m` to every stub in group `group` (1-based). Gone stubs
 * drop out of the table. */
static void notify(struct ne_state *s, int group, const struct ne_msg *m)
{
	uint32_t bit = 1u << (group - 1);
	long fd = -1;
	for (int i = 0; i < NE_SUBS; ++i) {
		struct ne_sub *sub = &s->sub[i];
		if (!sub->port || !(sub->groups & bit)) continue;
		if (fd < 0) {
			fd = TAWC_RAW(TAWC_SYS_socket, NE_UNIX, NE_DGRAM | NE_CLOEXEC, 0, 0, 0, 0);
			if (fd < 0) return;
		}
		struct ne_iov iov[2] = {
			{ (uint64_t)(uintptr_t)&bit, 4 },
			{ (uint64_t)(uintptr_t)m->b, m->n },
		};
		long r = deliver(fd, sub->port, iov, 2);
		if (r == TAWC_ECONNREFUSED || r == TAWC_ENOENT) sub->port = 0;
	}
	if (fd >= 0) tawc_close((int)fd);
}

static void notify_link(struct ne_state *s, const struct ne_link *l, uint32_t seq, uint32_t pid)
{
	struct ne_msg m;
	link_msg(&m, l, 0, seq, pid, 0xffffffffu);
	notify(s, RTNLGRP_LINK, &m);
}

static void notify_addr(struct ne_state *s, const struct ne_addr *a, uint16_t type,
			uint32_t seq, uint32_t pid)
{
	struct ne_msg m;
	addr_msg(&m, s, a, type, 0, seq, pid);
	notify(s, a->family == NE_INET ? RTNLGRP_IPV4_IFADDR : RTNLGRP_IPV6_IFADDR, &m);
}

/* Replies to one writer, flushed as datagrams of up to NE_OUT_CAP. */
struct ne_out { long fd; uint32_t port; char *buf; size_t len; };

static void out_flush(struct ne_out *o)
{
	if (o->len <= 4) return;
	memset(o->buf, 0, 4);
	struct ne_iov iov = { (uint64_t)(uintptr_t)o->buf, o->len };
	(void)deliver(o->fd, o->port, &iov, 1);
	o->len = 4;
}

static void out_put(struct ne_out *o, const void *msg, size_t len)
{
	if (o->len + len > NE_OUT_CAP) out_flush(o);
	if (o->len + len > NE_OUT_CAP) return;
	memcpy(o->buf + o->len, msg, len);
	o->len += len;
}

static void out_ack(struct ne_out *o, const struct nlmsghdr *h, long err)
{
	struct { struct nlmsghdr h; struct nlmsgerr e; } a;
	memset(&a, 0, sizeof a);
	a.h.nlmsg_len = sizeof a;
	a.h.nlmsg_type = NLMSG_ERROR;
	a.h.nlmsg_seq = h->nlmsg_seq;
	a.h.nlmsg_pid = o->port;
	a.e.error = (int)err;
	a.e.msg = *h;
	out_put(o, &a, sizeof a);
}

static void out_done(struct ne_out *o, const struct nlmsghdr *h)
{
	struct { struct nlmsghdr h; int32_t v; } d;
	memset(&d, 0, sizeof d);
	d.h.nlmsg_len = sizeof d;
	d.h.nlmsg_type = NLMSG_DONE;
	d.h.nlmsg_flags = NLM_F_MULTI;
	d.h.nlmsg_seq = h->nlmsg_seq;
	d.h.nlmsg_pid = o->port;
	out_put(o, &d, sizeof d);
}

/* ---- link and address changes (rtnetlink and ioctl) ---- */

static void set_flags(struct ne_state *s, struct ne_link *l, uint32_t want, uint32_t seq, uint32_t pid)
{
	uint32_t f = (l->flags & ~NE_IFF_SETTABLE) | (want & NE_IFF_SETTABLE);
	if (f & NE_IFF_UP) f |= NE_IFF_RUNNING | NE_IFF_LOWER_UP;
	else f &= ~(uint32_t)(NE_IFF_RUNNING | NE_IFF_LOWER_UP);
	if (f == l->flags) return;
	l->flags = f;
	notify_link(s, l, seq, pid);
}

static struct ne_addr *addr_find(struct ne_state *s, int index, uint8_t family, const uint8_t *addr)
{
	for (int i = 0; i < NE_ADDRS; ++i) {
		struct ne_addr *a = &s->addr[i];
		if (a->used && a->index == index && a->family == family &&
		    (!addr || !memcmp(a->addr, addr, addr_len(family))))
			return a;
	}
	return 0;
}

static long addr_add(struct ne_state *s, int index, uint8_t family, const uint8_t *addr,
		     uint8_t prefix, uint32_t flags, uint16_t nlflags, uint32_t seq, uint32_t pid)
{
	size_t n = addr_len(family);
	int zero = 1;
	for (size_t i = 0; i < n; ++i) if (addr[i]) zero = 0;
	if (zero) return 0;
	struct ne_addr *a = addr_find(s, index, family, addr);
	if (a && ((nlflags & NLM_F_EXCL) || !(nlflags & NLM_F_REPLACE))) return TAWC_EEXIST;
	for (int i = 0; !a && i < NE_ADDRS; ++i)
		if (!s->addr[i].used) a = &s->addr[i];
	if (!a) return TAWC_ENOSPC;
	memset(a, 0, sizeof *a);
	a->index = index;
	a->family = family;
	a->prefix = prefix;
	a->used = 1;
	memcpy(a->addr, addr, n);
	if (family == NE_INET) a->scope = addr[0] == 127 ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
	else a->scope = (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) ? RT_SCOPE_LINK : RT_SCOPE_UNIVERSE;
	/* No DAD: an address is usable as soon as it is added. */
	a->flags = (flags & ~(uint32_t)(IFA_F_TENTATIVE | IFA_F_DADFAILED | IFA_F_OPTIMISTIC)) |
		   IFA_F_PERMANENT;
	notify_addr(s, a, RTM_NEWADDR, seq, pid);
	return 0;
}

static void addr_del(struct ne_state *s, struct ne_addr *a, uint32_t seq, uint32_t pid)
{
	notify_addr(s, a, RTM_DELADDR, seq, pid);
	a->used = 0;
}

/* ---- rtnetlink requests ---- */

static const struct rtattr *find_attr(const struct nlmsghdr *h, size_t fixed, uint16_t type)
{
	size_t off = NLMSG_HDRLEN + NLMSG_ALIGN(fixed);
	while (off + sizeof(struct rtattr) <= h->nlmsg_len) {
		const struct rtattr *a = (const struct rtattr *)((const char *)h + off);
		if (a->rta_len < sizeof(struct rtattr) || off + a->rta_len > h->nlmsg_len) break;
		if (a->rta_type == type) return a;
		off += RTA_ALIGN(a->rta_len);
	}
	return 0;
}

static const void *payload(const struct nlmsghdr *h, size_t len)
{
	return h->nlmsg_len >= NLMSG_LENGTH(len) ? NLMSG_DATA(h) : 0;
}

static void ifname_attr(const struct nlmsghdr *h, char name[16])
{
	memset(name, 0, 16);
	const struct rtattr *a = find_attr(h, sizeof(struct ifinfomsg), IFLA_IFNAME);
	if (!a) return;
	size_t n = RTA_PAYLOAD(a);
	memcpy(name, RTA_DATA(a), n < 15 ? n : 15);
}

static long rt_getlink(struct ne_state *s, const struct nlmsghdr *h, struct ne_out *o)
{
	struct ne_msg m;
	if (h->nlmsg_flags & NLM_F_DUMP) {
		for (int i = 0; i < NE_LINKS; ++i) {
			link_msg(&m, &s->link[i], NLM_F_MULTI, h->nlmsg_seq, o->port, 0);
			out_put(o, m.b, m.n);
		}
		out_done(o, h);
		return 1;
	}
	const struct ifinfomsg *ifi = payload(h, sizeof *ifi);
	char name[16];
	ifname_attr(h, name);
	struct ne_link *l = link_by(s, ifi ? ifi->ifi_index : 0, name);
	if (!l) return TAWC_ENODEV;
	link_msg(&m, l, 0, h->nlmsg_seq, o->port, 0);
	out_put(o, m.b, m.n);
	return 0;
}

static long rt_setlink(struct ne_state *s, const struct nlmsghdr *h, uint32_t pid)
{
	const struct ifinfomsg *ifi = payload(h, sizeof *ifi);
	if (!ifi) return TAWC_EINVAL;
	char name[16];
	ifname_attr(h, name);
	struct ne_link *l = link_by(s, ifi->ifi_index, name);
	if (!l) return (h->nlmsg_flags & NLM_F_CREATE) ? TAWC_EOPNOTSUPP : TAWC_ENODEV;
	const struct rtattr *mtu = find_attr(h, sizeof *ifi, IFLA_MTU);
	if (mtu && RTA_PAYLOAD(mtu) >= 4) {
		uint32_t v;
		memcpy(&v, RTA_DATA(mtu), 4);
		if (v < 68 || v > 65536) return TAWC_EINVAL;
		if (v != l->mtu) {
			l->mtu = v;
			notify_link(s, l, h->nlmsg_seq, pid);
		}
	}
	/* ifi_change == 0 means "all bits", but only if any are given. */
	if (ifi->ifi_flags || ifi->ifi_change) {
		uint32_t change = ifi->ifi_change ? ifi->ifi_change : 0xffffffffu;
		set_flags(s, l, (ifi->ifi_flags & change) | (l->flags & ~change), h->nlmsg_seq, pid);
	}
	return 0;
}

static long rt_addr(struct ne_state *s, const struct nlmsghdr *h, uint32_t pid, int add)
{
	const struct ifaddrmsg *ifa = payload(h, sizeof *ifa);
	if (!ifa) return TAWC_EINVAL;
	if (ifa->ifa_family != NE_INET && ifa->ifa_family != NE_INET6) return TAWC_EOPNOTSUPP;
	if (!link_by(s, (int)ifa->ifa_index, 0)) return TAWC_ENODEV;
	size_t n = addr_len(ifa->ifa_family);
	const struct rtattr *a = find_attr(h, sizeof *ifa, IFA_LOCAL);
	if (!a) a = find_attr(h, sizeof *ifa, IFA_ADDRESS);
	if (a && RTA_PAYLOAD(a) < n) return TAWC_EINVAL;
	const uint8_t *addr = a ? (const uint8_t *)RTA_DATA(a) : 0;
	if (add) {
		if (!addr) return TAWC_EINVAL;
		uint32_t flags = ifa->ifa_flags;
		const struct rtattr *f = find_attr(h, sizeof *ifa, IFA_FLAGS);
		if (f && RTA_PAYLOAD(f) >= 4) memcpy(&flags, RTA_DATA(f), 4);
		return addr_add(s, (int)ifa->ifa_index, ifa->ifa_family, addr, ifa->ifa_prefixlen,
				flags, h->nlmsg_flags, h->nlmsg_seq, pid);
	}
	struct ne_addr *hit = addr_find(s, (int)ifa->ifa_index, ifa->ifa_family, addr);
	if (!hit) return TAWC_EADDRNOTAVAIL;
	addr_del(s, hit, h->nlmsg_seq, pid);
	return 0;
}

static void rt_dump_addrs(struct ne_state *s, const struct nlmsghdr *h, struct ne_out *o)
{
	const uint8_t *fam = payload(h, 1);
	struct ne_msg m;
	for (int i = 0; i < NE_ADDRS; ++i) {
		const struct ne_addr *a = &s->addr[i];
		if (!a->used || (fam && *fam && *fam != a->family)) continue;
		addr_msg(&m, s, a, RTM_NEWADDR, NLM_F_MULTI, h->nlmsg_seq, o->port);
		out_put(o, m.b, m.n);
	}
	out_done(o, h);
}

static void rt_request(struct ne_state *s, const struct nlmsghdr *h, struct ne_out *o)
{
	uint16_t t = h->nlmsg_type;
	if (t < RTM_BASE) return;  /* NOOP, ERROR, DONE, OVERRUN */
	int dump = (h->nlmsg_flags & NLM_F_DUMP) != 0;
	long err = 0;
	switch (t) {
	case RTM_GETLINK:
		err = rt_getlink(s, h, o);
		if (err > 0) return;
		break;
	case RTM_NEWLINK:
	case RTM_SETLINK:
		err = rt_setlink(s, h, o->port);
		break;
	case RTM_DELLINK:
		err = TAWC_EOPNOTSUPP;
		break;
	case RTM_GETADDR:
		if (dump) { rt_dump_addrs(s, h, o); return; }
		err = TAWC_EOPNOTSUPP;
		break;
	case RTM_NEWADDR:
	case RTM_DELADDR:
		err = rt_addr(s, h, o->port, t == RTM_NEWADDR);
		break;
	default:
		if (t > RTM_MAX) {
			err = TAWC_EOPNOTSUPP;
		} else if (((t - RTM_BASE) & 3) == 2) {  /* RTM_GET*: nothing is kept */
			if (dump) { out_done(o, h); return; }
			err = t == RTM_GETROUTE ? TAWC_ENETUNREACH : TAWC_ENOENT;
		}
		/* NEW/DEL/SET of routes, rules, neighbours, qdiscs...: the
		 * host routes, so these are accepted and dropped. */
	}
	if (err || (h->nlmsg_flags & NLM_F_ACK)) out_ack(o, h, err);
}

/* ---- stub socket I/O ---- */

static long gather(struct ne_iov *iov, uint64_t giov, uint64_t n)
{
	if (n > NE_MAX_IOV) return TAWC_EMSGSIZE;
	if (n && tawc_copy_from_guest(iov, (size_t)n * sizeof *iov, (const void *)(uintptr_t)giov) < 0)
		return TAWC_EFAULT;
	return (long)n;
}

static long nl_write(long fd, uint32_t port, const struct ne_iov *iov, int n)
{
	uint64_t total = 0;
	for (int i = 0; i < n; ++i) total += iov[i].len;
	if (total > TAWCROOT_PATH_SCRATCH_SIZE) return TAWC_EMSGSIZE;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *in = scratch->buf[0];
	size_t off = 0;
	for (int i = 0; i < n; ++i) {
		if (iov[i].len && tawc_copy_from_guest(in + off, (size_t)iov[i].len,
						       (const void *)(uintptr_t)iov[i].base) < 0)
			return TAWC_EFAULT;
		off += (size_t)iov[i].len;
	}
	struct ne_map map;
	long e = state_open(&map, 0);
	if (e < 0) return e;
	struct ne_out o = { fd, port, scratch->buf[1], 4 };
	for (off = 0; off + NLMSG_HDRLEN <= total;) {
		const struct nlmsghdr *h = (const struct nlmsghdr *)(in + off);
		if (h->nlmsg_len < NLMSG_HDRLEN || off + h->nlmsg_len > total) break;
		rt_request(map.s, h, &o);
		off += NLMSG_ALIGN(h->nlmsg_len);
	}
	out_flush(&o);
	state_close(&map);
	return (long)total;
}

/* Receive one datagram into the guest's buffers, presenting it as from
 * the kernel. `gm` (a local copy of the guest's msghdr) gets name,
 * control and flags filled for recvmsg; recvfrom passes its address
 * out-params instead. */
static long nl_recv(long fd, const struct ne_iov *uiov, int n, long flags,
		    struct ne_mh *gm, uint64_t gname, uint64_t gnamelen, ucontext_t *uc)
{
	if (!(flags & NE_MSG_DONTWAIT)) {
		long w = tawcroot_wait_fd((int)fd, 1 /*POLLIN*/, uc);
		if (w) return w;
	}
	struct ne_iov iov[1 + NE_MAX_IOV];
	uint32_t group = 0;
	iov[0].base = (uint64_t)(uintptr_t)&group;
	iov[0].len = 4;
	for (int i = 0; i < n; ++i) iov[1 + i] = uiov[i];
	struct ne_mh k;
	memset(&k, 0, sizeof k);
	k.iov = (uint64_t)(uintptr_t)iov;
	k.iovlen = (uint64_t)(1 + n);
	long r = TAWC_RAW(TAWC_SYS_recvmsg, fd, (long)&k, flags, 0, 0, 0);
	if (r < 0) return r;
	r = r >= 4 ? r - 4 : 0;
	struct ne_nl nl = { NE_NETLINK, 0, 0, group };
	if (gm) {
		gname = gm->name;
		if (gname && gm->namelen) {
			uint32_t c = gm->namelen < sizeof nl ? gm->namelen : (uint32_t)sizeof nl;
			if (tawc_copy_to_guest((void *)(uintptr_t)gname, &nl, c) < 0) return TAWC_EFAULT;
		}
		gm->namelen = sizeof nl;
		gm->flags = k.flags & ~NE_MSG_CTRUNC;
		/* struct cmsghdr (len, level, type) + struct ucred: all zero
		 * ids, as from the kernel. */
		struct { uint64_t len; int32_t level, type; uint32_t cred[3]; uint32_t pad; } cm = {
			16 + 12, NE_SOL_SOCKET, NE_SCM_CREDENTIALS, { 0, 0, 0 }, 0
		};
		if (gm->control && gm->controllen >= sizeof cm) {
			if (tawc_copy_to_guest((void *)(uintptr_t)gm->control, &cm, sizeof cm) < 0)
				return TAWC_EFAULT;
			gm->controllen = sizeof cm;
		} else {
			if (gm->controllen) gm->flags |= NE_MSG_CTRUNC;
			gm->controllen = 0;
		}
		return r;
	}
	if (gname && gnamelen) {
		uint32_t cap;
		if (tawc_copy_from_guest(&cap, sizeof cap, (const void *)(uintptr_t)gnamelen) < 0)
			return TAWC_EFAULT;
		uint32_t c = cap < sizeof nl ? cap : (uint32_t)sizeof nl;
		uint32_t full = sizeof nl;
		if ((c && tawc_copy_to_guest((void *)(uintptr_t)gname, &nl, c) < 0) ||
		    tawc_copy_to_guest((void *)(uintptr_t)gnamelen, &full, sizeof full) < 0)
			return TAWC_EFAULT;
	}
	return r;
}

static void set_groups(uint32_t port, uint32_t groups, int how /* 0 set, 1 add, 2 drop */)
{
	struct ne_map m;
	if (state_open(&m, 0) < 0) return;
	struct ne_sub *hit = 0, *spare = 0;
	for (int i = 0; i < NE_SUBS; ++i) {
		struct ne_sub *s = &m.s->sub[i];
		if (s->port == port) hit = s;
		else if (!s->port && !spare) spare = s;
	}
	if (!hit && spare) { hit = spare; hit->port = port; hit->groups = 0; }
	if (hit) {
		hit->groups = how == 0 ? groups : how == 1 ? hit->groups | groups : hit->groups & ~groups;
		if (!hit->groups) hit->port = 0;
	}
	state_close(&m);
}

static uint32_t get_groups(uint32_t port)
{
	struct ne_map m;
	uint32_t g = 0;
	if (state_open(&m, 0) < 0) return 0;
	for (int i = 0; i < NE_SUBS; ++i)
		if (m.s->sub[i].port == port) g = m.s->sub[i].groups;
	state_close(&m);
	return g;
}

long tawcroot_netemu_socket(long domain, long type, long proto)
{
	if (!enabled() || domain != NE_NETLINK || proto != NETLINK_ROUTE) return TAWC_ENOSYS;
	long fd = TAWC_RAW(TAWC_SYS_socket, NE_UNIX, (type & ~0xfL) | NE_DGRAM, 0, 0, 0, 0);
	if (fd < 0) return fd;
	struct ne_map m;
	long e = state_open(&m, 0);
	if (e < 0) { tawc_close((int)fd); return e; }
	/* Like the kernel: the first socket gets the process id, later
	 * ones a distinct high id. */
	uint32_t port = (uint32_t)tawc_getpid();
	for (int i = 0; i < 64; ++i) {
		struct ne_un un;
		long len = stub_addr(&un, port);
		e = TAWC_RAW(TAWC_SYS_bind, fd, (long)&un, len, 0, 0, 0);
		if (e != TAWC_EADDRINUSE) break;
		port = 0x80000000u | (++m.s->next_port & 0x7fffffffu);
	}
	/* A reused id must not inherit a dead socket's groups. */
	for (int i = 0; !e && i < NE_SUBS; ++i)
		if (m.s->sub[i].port == port) m.s->sub[i].port = 0;
	state_close(&m);
	if (e < 0) { tawc_close((int)fd); return e; }
	long hi = tawc_fcntl((int)fd, (type & NE_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD,
			     TAWCROOT_RESERVED_FD_BASE);
	tawc_close((int)fd);
	return hi;
}

/* bind() to the fake eth0 address: the host has no such address, so
 * bind the wildcard instead. */
static long bind_fake_addr(const tawcroot_syscall_args *a)
{
	unsigned char sa[16];
	if (!a->b || a->c < 8 || a->c > 128 ||
	    tawc_copy_from_guest(sa, 8, (const void *)(uintptr_t)a->b) < 0 ||
	    sa[0] != NE_INET || sa[1] || sa[4] == 0 || sa[4] == 127)
		return TAWC_ENOSYS;
	struct ne_map m;
	if (state_open(&m, 0) < 0) return TAWC_ENOSYS;
	int hit = 0;
	for (int i = 0; i < NE_ADDRS; ++i) {
		const struct ne_addr *x = &m.s->addr[i];
		if (x->used && x->family == NE_INET && x->index != 1 && !memcmp(x->addr, sa + 4, 4)) hit = 1;
	}
	state_close(&m);
	if (!hit) return TAWC_ENOSYS;
	unsigned char copy[128];
	if (tawc_copy_from_guest(copy, (size_t)a->c, (const void *)(uintptr_t)a->b) < 0) return TAWC_EFAULT;
	memset(copy + 4, 0, 4);
	(void)tawcroot_ports_rewrite(copy, a->c, TAWC_PORTS_BIND);
	return TAWC_RAW(TAWC_SYS_bind, a->a, (long)copy, a->c, 0, 0, 0);
}

static long put_name(uint64_t gaddr, uint64_t glen, const struct ne_nl *nl)
{
	uint32_t cap;
	if (!gaddr || !glen) return TAWC_EFAULT;
	if (tawc_copy_from_guest(&cap, sizeof cap, (const void *)(uintptr_t)glen) < 0) return TAWC_EFAULT;
	if ((int32_t)cap < 0) return TAWC_EINVAL;
	uint32_t c = cap < sizeof *nl ? cap : (uint32_t)sizeof *nl, full = sizeof *nl;
	if ((c && tawc_copy_to_guest((void *)(uintptr_t)gaddr, nl, c) < 0) ||
	    tawc_copy_to_guest((void *)(uintptr_t)glen, &full, sizeof full) < 0)
		return TAWC_EFAULT;
	return 0;
}

long tawcroot_netemu_call(const tawcroot_syscall_args *a, ucontext_t *uc)
{
	if (!enabled()) return TAWC_ENOSYS;
	if (a->nr == TAWC_SYS_bind) {
		long r = bind_fake_addr(a);
		if (r != TAWC_ENOSYS) return r;
	}
	uint32_t port = stub_port(a->a);
	if (!port) return TAWC_ENOSYS;
	struct ne_iov iov[NE_MAX_IOV];
	struct ne_mh mh;
	long n;
	switch (a->nr) {
	case TAWC_SYS_bind: {
		struct ne_nl nl;
		if (!a->b || a->c < (long)sizeof nl) return TAWC_EINVAL;
		if (tawc_copy_from_guest(&nl, sizeof nl, (const void *)(uintptr_t)a->b) < 0) return TAWC_EFAULT;
		if (nl.family != NE_NETLINK) return TAWC_EINVAL;
		set_groups(port, nl.groups, 0);
		return 0;
	}
	case TAWC_SYS_connect:
		return 0;
	case TAWC_SYS_getsockname:
	case TAWC_SYS_getpeername: {
		struct ne_nl nl = { NE_NETLINK, 0, 0, 0 };
		if (a->nr == TAWC_SYS_getsockname) { nl.pid = port; nl.groups = get_groups(port); }
		return put_name((uint64_t)a->b, (uint64_t)a->c, &nl);
	}
	case TAWC_SYS_getsockopt: {
		int32_t v;
		if ((int)a->b == NE_SOL_NETLINK) v = 0;
		else if ((int)a->b == NE_SOL_SOCKET && (int)a->c == NE_SO_DOMAIN) v = NE_NETLINK;
		else return TAWC_ENOSYS;
		uint32_t four = 4;
		if (!a->d || !a->e ||
		    tawc_copy_to_guest((void *)(uintptr_t)a->d, &v, 4) < 0 ||
		    tawc_copy_to_guest((void *)(uintptr_t)a->e, &four, 4) < 0)
			return TAWC_EFAULT;
		return 0;
	}
	case TAWC_SYS_write:
	case TAWC_SYS_sendto:
		iov[0].base = (uint64_t)a->b;
		iov[0].len = (uint64_t)a->c;
		return nl_write(a->a, port, iov, 1);
	case TAWC_SYS_sendmsg:
		if (!a->b || tawc_copy_from_guest(&mh, sizeof mh, (const void *)(uintptr_t)a->b) < 0)
			return TAWC_EFAULT;
		n = gather(iov, mh.iov, mh.iovlen);
		if (n < 0) return n;
		return nl_write(a->a, port, iov, (int)n);
	case TAWC_SYS_read:
		iov[0].base = (uint64_t)a->b;
		iov[0].len = (uint64_t)a->c;
		return nl_recv(a->a, iov, 1, 0, 0, 0, 0, uc);
	case TAWC_SYS_recvfrom:
		iov[0].base = (uint64_t)a->b;
		iov[0].len = (uint64_t)a->c;
		return nl_recv(a->a, iov, 1, a->d, 0, (uint64_t)a->e, (uint64_t)a->f, uc);
	case TAWC_SYS_recvmsg: {
		if (!a->b || tawc_copy_from_guest(&mh, sizeof mh, (const void *)(uintptr_t)a->b) < 0)
			return TAWC_EFAULT;
		n = gather(iov, mh.iov, mh.iovlen);
		if (n < 0) return n;
		long r = nl_recv(a->a, iov, (int)n, a->c, &mh, 0, 0, uc);
		if (r >= 0 && tawc_copy_to_guest((void *)(uintptr_t)a->b, &mh, sizeof mh) < 0)
			return TAWC_EFAULT;
		return r;
	}
	}
	return TAWC_ENOSYS;
}

long tawcroot_netemu_read(const tawcroot_syscall_args *a, ucontext_t *uc)
{
	long r = tawcroot_netemu_call(a, uc);
	if (r != TAWC_ENOSYS) return r;
	if ((r = tawcroot_wait_fd((int)a->a, 1 /*POLLIN*/, uc))) return r;
	return TAWC_RAW(TAWC_SYS_read, a->a, a->b, a->c, 0, 0, 0);
}

long tawcroot_netemu_setsockopt(const tawcroot_syscall_args *a, ucontext_t *uc)
{
	(void)uc;
	int level = (int)a->b, opt = (int)a->c;
	uint32_t port = stub_port(a->a);
	if (port) {
		if (level == NE_SOL_NETLINK) {
			uint32_t g;
			if ((opt == NETLINK_ADD_MEMBERSHIP || opt == NETLINK_DROP_MEMBERSHIP) &&
			    a->d && a->e >= 4 &&
			    tawc_copy_from_guest(&g, 4, (const void *)(uintptr_t)a->d) == 0 &&
			    g >= 1 && g <= 32)
				set_groups(port, 1u << (g - 1), opt == NETLINK_ADD_MEMBERSHIP ? 1 : 2);
			return 0;
		}
		/* Socket filters, forced buffer sizes...: what an AF_UNIX
		 * socket refuses would have worked on netlink. */
		long r = TAWC_RAW(TAWC_SYS_setsockopt, a->a, a->b, a->c, a->d, a->e, 0);
		return r < 0 ? 0 : r;
	}
	if (enabled() && level == NE_SOL_SOCKET) {
		/* netd marks every socket with its network; there is one. */
		if (opt == NE_SO_MARK) return 0;
		if (opt == NE_SO_BINDTODEVICE || opt == NE_SO_BINDTOIFINDEX) {
			char dev[16];
			memset(dev, 0, sizeof dev);
			size_t len = a->e < 15 ? (size_t)a->e : 15;
			if (a->d && len && tawc_copy_from_guest(dev, len, (const void *)(uintptr_t)a->d) < 0)
				return TAWC_EFAULT;
			int32_t index;
			memcpy(&index, dev, 4);
			if (opt == NE_SO_BINDTODEVICE ? tawc_streq(dev, "eth0")
						     : index == TAWCROOT_NETEMU_ETH_INDEX)
				return 0;
			long r = TAWC_RAW(TAWC_SYS_setsockopt, a->a, a->b, a->c, a->d, a->e, 0);
			return r < 0 ? 0 : r;
		}
	}
	return TAWC_RAW(TAWC_SYS_setsockopt, a->a, a->b, a->c, a->d, a->e, 0);
}

/* ---- SIOC* ioctls ---- */

#define NE_SIOCGIFNAME 0x8910
#define NE_SIOCGIFCONF 0x8912
#define NE_SIOCGIFFLAGS 0x8913
#define NE_SIOCSIFFLAGS 0x8914
#define NE_SIOCGIFADDR 0x8915
#define NE_SIOCSIFADDR 0x8916
#define NE_SIOCGIFDSTADDR 0x8917
#define NE_SIOCGIFBRDADDR 0x8919
#define NE_SIOCGIFNETMASK 0x891b
#define NE_SIOCSIFNETMASK 0x891c
#define NE_SIOCGIFMETRIC 0x891d
#define NE_SIOCGIFMTU 0x8921
#define NE_SIOCSIFMTU 0x8922
#define NE_SIOCGIFHWADDR 0x8927
#define NE_SIOCGIFINDEX 0x8933
#define NE_SIOCDIFADDR 0x8936
#define NE_SIOCGIFTXQLEN 0x8942
#define NE_SIOCETHTOOL 0x8946
#define NE_SIOCWANDEV 0x894a

static void put_in(struct ne_ifreq *r, const uint8_t addr[4])
{
	memset(&r->u, 0, sizeof r->u);
	r->u.sa.family = NE_INET;
	memcpy(r->u.sa.data + 2, addr, 4);
}

static long ifconf(struct ne_state *s, long arg)
{
	struct { int32_t len, pad; uint64_t buf; } c;
	if (!arg || tawc_copy_from_guest(&c, sizeof c, (const void *)(uintptr_t)arg) < 0) return TAWC_EFAULT;
	int32_t cap = c.buf ? c.len / (int32_t)sizeof(struct ne_ifreq) : 0x7fffffff, out = 0;
	for (int i = 0; i < NE_LINKS && out < cap; ++i) {
		const struct ne_addr *a = addr_find(s, s->link[i].index, NE_INET, 0);
		if (!a) continue;
		if (c.buf) {
			struct ne_ifreq r;
			memset(&r, 0, sizeof r);
			memcpy(r.name, s->link[i].name, sizeof r.name);
			put_in(&r, a->addr);
			if (tawc_copy_to_guest((void *)(uintptr_t)(c.buf + (uint64_t)out * sizeof r), &r, sizeof r) < 0)
				return TAWC_EFAULT;
		}
		++out;
	}
	c.len = out * (int32_t)sizeof(struct ne_ifreq);
	return tawc_copy_to_guest((void *)(uintptr_t)arg, &c, sizeof c) < 0 ? TAWC_EFAULT : 0;
}

static uint8_t mask_prefix(const uint8_t m[4])
{
	uint32_t v = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
	uint8_t p = 0;
	while (p < 32 && (v & (0x80000000u >> p))) ++p;
	return p;
}

static long ifreq_op(struct ne_state *s, int fd, unsigned cmd, struct ne_ifreq *r)
{
	struct ne_link *l = cmd == NE_SIOCGIFNAME ? link_by(s, r->u.ival, 0) : link_by(s, 0, r->name);
	if (!l) return TAWC_ENODEV;
	struct ne_addr *v4 = addr_find(s, l->index, NE_INET, 0);
	const uint8_t *in = r->u.sa.data + 2;
	switch (cmd) {
	case NE_SIOCGIFNAME:
		memcpy(r->name, l->name, sizeof r->name);
		return 0;
	case NE_SIOCGIFINDEX:
		r->u.ival = l->index;
		return 0;
	case NE_SIOCGIFFLAGS:
		r->u.flags = (int16_t)l->flags;
		return 0;
	case NE_SIOCSIFFLAGS:
		set_flags(s, l, (l->flags & ~0xffffu) | (uint16_t)r->u.flags, 0, 0);
		return 0;
	case NE_SIOCGIFMTU:
		r->u.ival = (int32_t)l->mtu;
		return 0;
	case NE_SIOCSIFMTU:
		if (r->u.ival < 68 || r->u.ival > 65536) return TAWC_EINVAL;
		if ((uint32_t)r->u.ival != l->mtu) {
			l->mtu = (uint32_t)r->u.ival;
			notify_link(s, l, 0, 0);
		}
		return 0;
	case NE_SIOCGIFHWADDR:
		memset(&r->u, 0, sizeof r->u);
		r->u.sa.family = l->type;
		memcpy(r->u.sa.data, l->mac, 6);
		return 0;
	case NE_SIOCGIFTXQLEN:
		r->u.ival = 1000;
		return 0;
	case NE_SIOCGIFMETRIC:
		r->u.ival = 0;
		return 0;
	case NE_SIOCGIFADDR:
	case NE_SIOCGIFNETMASK:
	case NE_SIOCGIFBRDADDR:
	case NE_SIOCGIFDSTADDR: {
		if (!v4) return TAWC_EADDRNOTAVAIL;
		uint8_t b[4];
		uint32_t mask = v4->prefix ? 0xffffffffu << (32 - v4->prefix) : 0;
		for (int i = 0; i < 4; ++i) {
			uint8_t m = (uint8_t)(mask >> (24 - 8 * i));
			b[i] = cmd == NE_SIOCGIFNETMASK ? m :
			       cmd == NE_SIOCGIFBRDADDR ? (uint8_t)(v4->addr[i] | ~m) : v4->addr[i];
		}
		put_in(r, b);
		return 0;
	}
	case NE_SIOCSIFADDR:
	case NE_SIOCDIFADDR: {
		int32_t domain = 0;
		uint32_t dl = sizeof domain;
		(void)TAWC_RAW(TAWC_SYS_getsockopt, fd, NE_SOL_SOCKET, NE_SO_DOMAIN, (long)&domain, (long)&dl, 0);
		if (domain == NE_INET6 || r->u.sa.family != NE_INET) return TAWC_EOPNOTSUPP;
		if (cmd == NE_SIOCDIFADDR) return TAWC_EOPNOTSUPP;
		static const uint8_t zero[4];
		if (!memcmp(in, zero, 4)) {
			/* 0.0.0.0 clears the link's IPv4 addresses (netd's setCfg). */
			for (struct ne_addr *a; (a = addr_find(s, l->index, NE_INET, 0));) addr_del(s, a, 0, 0);
			return 0;
		}
		uint8_t prefix = v4 ? v4->prefix : 24;
		if (v4 && !memcmp(v4->addr, in, 4)) return 0;
		if (v4) addr_del(s, v4, 0, 0);
		return addr_add(s, l->index, NE_INET, in, prefix, 0, NLM_F_CREATE, 0, 0);
	}
	case NE_SIOCSIFNETMASK: {
		if (!v4) return TAWC_EADDRNOTAVAIL;
		uint8_t p = mask_prefix(in);
		if (p != v4->prefix) {
			v4->prefix = p;
			notify_addr(s, v4, RTM_NEWADDR, 0, 0);
		}
		return 0;
	}
	}
	return TAWC_EINVAL;
}

long tawcroot_netemu_ioctl(int fd, unsigned cmd, long arg)
{
	if (!enabled() || cmd < NE_SIOCGIFNAME || cmd > NE_SIOCWANDEV) return TAWC_ENOSYS;
	if (cmd >= NE_SIOCETHTOOL) return TAWC_EOPNOTSUPP;  /* ethtool, MII, WAN */
	struct ne_map m;
	struct ne_ifreq r;
	if (cmd != NE_SIOCGIFCONF &&
	    (!arg || tawc_copy_from_guest(&r, sizeof r, (const void *)(uintptr_t)arg) < 0))
		return TAWC_EFAULT;
	long e = state_open(&m, 0);
	if (e < 0) return e;
	if (cmd == NE_SIOCGIFCONF) {
		e = ifconf(m.s, arg);
	} else {
		r.name[15] = 0;
		e = ifreq_op(m.s, fd, cmd, &r);
		if (!e && tawc_copy_to_guest((void *)(uintptr_t)arg, &r, sizeof r) < 0) e = TAWC_EFAULT;
	}
	state_close(&m);
	return e;
}

/* ---- setup ---- */

/* Write `value` + "\n" to `rel` under `root`, making its directories. */
static long put(int root, const char *rel, const char *value)
{
	char path[160];
	size_t n = 0;
	if (tawc_str_append(path, sizeof path, &n, rel)) return TAWC_ENAMETOOLONG;
	for (size_t i = 0; i < n; ++i) {
		if (path[i] != '/') continue;
		path[i] = 0;
		long e = TAWC_RAW(TAWC_SYS_mkdirat, root, (long)path, 0755, 0, 0, 0);
		path[i] = '/';
		if (e < 0 && e != TAWC_EEXIST) return e;
	}
	long fd = tawc_openat(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0) return fd;
	size_t len = tawc_strlen(value);
	long w = tawc_write((int)fd, value, len);
	if (w >= 0) w = tawc_write((int)fd, "\n", 1);
	tawc_close((int)fd);
	return w < 0 ? w : 0;
}

static long put_in_dir(int root, const char *dir, const char *name, const char *value)
{
	char rel[160];
	size_t n = 0;
	if (tawc_str_append(rel, sizeof rel, &n, dir) || tawc_str_append(rel, sizeof rel, &n, "/") ||
	    tawc_str_append(rel, sizeof rel, &n, name))
		return TAWC_ENAMETOOLONG;
	return put(root, rel, value);
}

static long put_list(int root, const char *dir, const char *const *kv)
{
	for (; kv[0]; kv += 2) {
		long e = put_in_dir(root, dir, kv[0], kv[1]);
		if (e < 0) return e;
	}
	return 0;
}

static const char *const sys_lo[] = {
	"ifindex", "1", "iflink", "1", "address", "00:00:00:00:00:00",
	"broadcast", "00:00:00:00:00:00", "mtu", "65536", "flags", "0x9",
	"operstate", "unknown", "carrier", "1", "type", "772", "addr_len", "6",
	"tx_queue_len", "1000", "dev_id", "0x0", "dev_port", "0", "addr_assign_type", "0",
	"uevent", "INTERFACE=lo\nIFINDEX=1", 0
};
static const char *const sys_eth[] = {
	"ifindex", "2", "iflink", "2", "address", "02:00:00:57:44:01",
	"broadcast", "ff:ff:ff:ff:ff:ff", "mtu", "1500", "flags", "0x1003",
	"operstate", "up", "carrier", "1", "type", "1", "addr_len", "6",
	"tx_queue_len", "1000", "dev_id", "0x0", "dev_port", "0", "addr_assign_type", "3",
	"speed", "1000", "duplex", "full", "uevent", "INTERFACE=eth0\nIFINDEX=2", 0
};
static const char *const conf4[] = {
	"forwarding", "0", "mc_forwarding", "0", "rp_filter", "0", "accept_redirects", "1",
	"send_redirects", "1", "accept_local", "0", "arp_ignore", "0", "arp_announce", "0",
	"proxy_arp", "0", "route_localnet", "0", 0
};
static const char *const conf6[] = {
	"disable_ipv6", "0", "accept_ra", "1", "accept_ra_defrtr", "1", "accept_ra_pinfo", "1",
	"accept_ra_rt_info_min_plen", "0", "accept_ra_rt_info_max_plen", "0", "accept_ra_mtu", "1",
	"accept_dad", "1", "dad_transmits", "1", "autoconf", "1", "forwarding", "0",
	"hop_limit", "64", "mtu", "1500", "use_tempaddr", "0", "addr_gen_mode", "0",
	"router_solicitations", "-1", "use_optimistic", "0", "optimistic_dad", "0",
	"proxy_ndp", "0", "keep_addr_on_down", "0", 0
};
static const char *const neigh[] = {
	"base_reachable_time_ms", "30000", "retrans_time_ms", "1000", "ucast_solicit", "3",
	"mcast_solicit", "3", "mcast_resolicit", "0", "app_solicit", "0",
	"delay_first_probe_time", "5", "gc_stale_time", "60", 0
};
static const char *const proc_top[] = {
	"ipv4/ip_forward", "0", "ipv4/ip_local_port_range", "32768\t60999",
	"ipv4/ping_group_range", "0\t2147483647", "ipv4/tcp_syncookies", "1",
	"ipv6/conf/lo/mtu", "65536", "core/somaxconn", "4096", 0
};

static long populate(int root)
{
	static const char *const ifs[] = { "all", "default", "lo", "eth0" };
	long e = put_list(root, "sys/lo", sys_lo);
	if (!e) e = put_list(root, "sys/eth0", sys_eth);
	char dir[64];
	for (int i = 0; !e && i < 4; ++i) {
		static const char *const roots[] = { "proc/ipv4/conf/", "proc/ipv6/conf/",
						     "proc/ipv4/neigh/", "proc/ipv6/neigh/" };
		for (int k = 0; !e && k < 4; ++k) {
			if (k >= 2 && i == 0) continue;  /* neigh has no "all" */
			size_t n = 0;
			(void)tawc_str_append(dir, sizeof dir, &n, roots[k]);
			(void)tawc_str_append(dir, sizeof dir, &n, ifs[i]);
			e = put_list(root, dir, k == 0 ? conf4 : k == 1 ? conf6 : neigh);
		}
	}
	for (const char *const *kv = proc_top; !e && kv[0]; kv += 2)
		e = put_in_dir(root, "proc", kv[0], kv[1]);
	return e;
}

long tawcroot_netemu_setup(void)
{
	struct tawc_namespace *ns = &tawcroot_namespace;
	if (!(ns->session & TAWC_NS_INIT_SESSION)) return TAWC_EINVAL;
	struct ne_map m;
	long e = state_open(&m, 1);
	if (e < 0) return e;
	state_close(&m);

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	e = store_file(path, "/net");
	if (e < 0) return e;
	e = TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)path, 0755, 0, 0, 0);
	if (e < 0 && e != TAWC_EEXIST) return e;
	long dir = tawc_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (dir < 0) return dir;
	tawcroot_wipe_dir((int)dir);
	e = populate((int)dir);
	tawc_close((int)dir);
	if (e < 0) return e;
	char *sub = scratch->buf[1];
	if ((e = store_file(sub, "/net/sys")) < 0 ||
	    (e = tawcroot_path_add_bind(sub, "sys/class/net", 0)) < 0 ||
	    (e = store_file(sub, "/net/proc")) < 0 ||
	    (e = tawcroot_path_add_bind(sub, "proc/sys/net", 0)) < 0)
		return e;

	/* Stub reads and writes go by fd, above the floor; setsockopt
	 * everywhere (SO_MARK). */
	if ((e = tawcroot_install_fd_floor_filter(TAWC_SYS_read, 0)) < 0) return e;
	if (!(ns->session & TAWC_NS_CG_WRITE_TRAP)) {
		if ((e = tawcroot_install_fd_floor_filter(TAWC_SYS_write, 0)) < 0) return e;
		ns->session |= TAWC_NS_CG_WRITE_TRAP;
	}
	if (!(ns->session & TAWC_NS_RECV_TRAP)) {
		if ((e = tawcroot_install_fd_floor_filter(TAWC_SYS_recvfrom, 0)) < 0 ||
		    (e = tawcroot_install_fd_floor_filter(TAWC_SYS_recvmsg, 0)) < 0 ||
		    (e = tawcroot_install_fd_floor_filter(__NR_recvmmsg, 0)) < 0)
			return e;
		ns->session |= TAWC_NS_RECV_TRAP;
	}
	static const int lazy[] = { TAWC_SYS_setsockopt };
	if ((e = tawcroot_install_lazy_filter(lazy, 1)) < 0) return e;
	ns->session |= TAWC_NS_FAKE_NET;
	return 0;
}
