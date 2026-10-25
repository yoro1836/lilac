/* `tawcroot --init --fake-net`: the emulated lo + eth0 over rtnetlink,
 * SIOC* ioctls, /sys/class/net and /proc/sys/net, as netd and IpClient
 * drive them; multicast changes reach a listener as kernel messages
 * with credentials, the state is shared with children, and glibc's
 * getifaddrs sees it.
 *
 * Exit 42 on success; every other code names the failed step. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned seq = 1;

static int nl_socket(unsigned groups)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) return -1;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = groups };
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) return -1;
	return fd;
}

static int request(int fd, int type, int flags, const void *body, size_t len)
{
	char buf[256] __attribute__((aligned(4)));
	memset(buf, 0, sizeof buf);
	struct nlmsghdr *h = (struct nlmsghdr *)buf;
	h->nlmsg_len = NLMSG_LENGTH(len);
	h->nlmsg_type = (unsigned short)type;
	h->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | flags);
	h->nlmsg_seq = seq++;
	memcpy(NLMSG_DATA(h), body, len);
	struct sockaddr_nl k = { .nl_family = AF_NETLINK };
	return sendto(fd, buf, h->nlmsg_len, 0, (struct sockaddr *)&k, sizeof k) == (ssize_t)h->nlmsg_len ? 0 : -1;
}

/* One datagram; checks it claims the kernel as its source. */
static ssize_t receive(int fd, char *buf, size_t cap, unsigned *groups, int *cred)
{
	struct sockaddr_nl from;
	struct iovec iov = { buf, cap };
	char control[64];
	struct msghdr mh = { &from, sizeof from, &iov, 1, control, sizeof control, 0 };
	ssize_t n = recvmsg(fd, &mh, 0);
	if (n < 0 || mh.msg_namelen != sizeof from || from.nl_family != AF_NETLINK || from.nl_pid != 0)
		return -1;
	if (groups) *groups = from.nl_groups;
	if (cred) {
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		*cred = c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS &&
			((struct ucred *)CMSG_DATA(c))->uid == 0;
	}
	return n;
}

/* Ack status of the last request, or 1 when none came. */
static int ack(int fd)
{
	char buf[4096] __attribute__((aligned(4)));
	ssize_t n = receive(fd, buf, sizeof buf, 0, 0);
	if (n < 0) return 1;
	struct nlmsghdr *h = (struct nlmsghdr *)buf;
	if (!NLMSG_OK(h, (size_t)n) || h->nlmsg_type != NLMSG_ERROR) return 1;
	return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
}

static const char *ifname(struct nlmsghdr *h)
{
	struct ifinfomsg *ifi = NLMSG_DATA(h);
	int len = (int)IFLA_PAYLOAD(h);
	for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, len); a = RTA_NEXT(a, len))
		if (a->rta_type == IFLA_IFNAME) return RTA_DATA(a);
	return "";
}

/* Dump links: 1 if lo is index 1 and eth0 index 2, ending in DONE. */
static int dump_links(int fd)
{
	struct rtgenmsg g = { AF_UNSPEC };
	if (request(fd, RTM_GETLINK, NLM_F_DUMP, &g, sizeof g)) return 0;
	int lo = 0, eth = 0;
	for (;;) {
		char buf[8192] __attribute__((aligned(4)));
		ssize_t n = receive(fd, buf, sizeof buf, 0, 0);
		if (n <= 0) return 0;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type == NLMSG_DONE) return lo && eth;
			if (h->nlmsg_type != RTM_NEWLINK) return 0;
			struct ifinfomsg *ifi = NLMSG_DATA(h);
			if (!strcmp(ifname(h), "lo") && ifi->ifi_index == 1) lo = 1;
			if (!strcmp(ifname(h), "eth0") && ifi->ifi_index == 2) eth = 1;
		}
	}
}

/* Wait for a `type` multicast on `fd` in group bit `bit`. */
static int heard(int fd, int type, unsigned bit)
{
	for (int i = 0; i < 8; ++i) {
		char buf[4096] __attribute__((aligned(4)));
		unsigned groups = 0;
		int cred = 0;
		ssize_t n = receive(fd, buf, sizeof buf, &groups, &cred);
		if (n <= 0 || !cred) return 0;
		struct nlmsghdr *h = (struct nlmsghdr *)buf;
		if (NLMSG_OK(h, (size_t)n) && h->nlmsg_type == type && groups == bit) return 1;
	}
	return 0;
}

static int addr_req(int fd, int type, const char *ip, int prefix)
{
	struct {
		struct ifaddrmsg ifa;
		struct rtattr a;
		struct in_addr in;
	} body;
	memset(&body, 0, sizeof body);
	body.ifa.ifa_family = AF_INET;
	body.ifa.ifa_prefixlen = (unsigned char)prefix;
	body.ifa.ifa_index = 2;
	body.a.rta_type = IFA_LOCAL;
	body.a.rta_len = RTA_LENGTH(4);
	inet_pton(AF_INET, ip, &body.in);
	if (request(fd, type, NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE, &body, sizeof body)) return 1;
	return ack(fd);
}

static int if_ioctl(int fd, unsigned long cmd, struct ifreq *r)
{
	return ioctl(fd, cmd, r);
}

static int eth_addr(int fd, char *out)
{
	struct ifreq r;
	memset(&r, 0, sizeof r);
	strcpy(r.ifr_name, "eth0");
	if (if_ioctl(fd, SIOCGIFADDR, &r)) return -1;
	inet_ntop(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, out, 16);
	return 0;
}

int main(void)
{
	DIR *d = opendir("/sys/class/net");
	int lo = 0, eth = 0;
	for (struct dirent *e; d && (e = readdir(d));) {
		if (!strcmp(e->d_name, "lo")) lo = 1;
		if (!strcmp(e->d_name, "eth0")) eth = 1;
	}
	if (!d || !lo || !eth) return 1;
	closedir(d);

	int nl = nl_socket(0);
	struct sockaddr_nl me;
	socklen_t ml = sizeof me;
	if (nl < 0 || getsockname(nl, (struct sockaddr *)&me, &ml) || me.nl_family != AF_NETLINK || !me.nl_pid)
		return 2;
	if (!dump_links(nl)) return 3;

	int mon = nl_socket(RTMGRP_LINK);
	int on = 1;
	struct timeval tv = { 2, 0 };
	if (mon < 0 || setsockopt(mon, SOL_SOCKET, SO_PASSCRED, &on, sizeof on) ||
	    setsockopt(mon, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) ||
	    setsockopt(nl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv))
		return 4;
	int group = RTNLGRP_IPV4_IFADDR;
	if (setsockopt(mon, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof group)) return 4;

	int inet = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct ifreq r;
	memset(&r, 0, sizeof r);
	strcpy(r.ifr_name, "eth0");
	if (inet < 0 || if_ioctl(inet, SIOCGIFINDEX, &r) || r.ifr_ifindex != 2) return 5;
	if (if_ioctl(inet, SIOCGIFFLAGS, &r) || (r.ifr_flags & IFF_UP)) return 5;
	r.ifr_flags |= IFF_UP;
	if (if_ioctl(inet, SIOCSIFFLAGS, &r) || if_ioctl(inet, SIOCGIFFLAGS, &r) ||
	    (r.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
		return 6;
	if (!heard(mon, RTM_NEWLINK, RTMGRP_LINK)) return 7;

	if (addr_req(nl, RTM_NEWADDR, "10.0.2.15", 24) != 0) return 8;
	if (!heard(mon, RTM_NEWADDR, RTMGRP_IPV4_IFADDR)) return 9;
	char ip[16];
	if (eth_addr(inet, ip) || strcmp(ip, "10.0.2.15")) return 10;
	if (if_ioctl(inet, SIOCGIFNETMASK, &r) ||
	    ((struct sockaddr_in *)&r.ifr_netmask)->sin_addr.s_addr != htonl(0xffffff00))
		return 11;

	struct ifaddrs *all, *x;
	int seen = 0;
	if (getifaddrs(&all)) return 12;
	for (x = all; x; x = x->ifa_next)
		if (x->ifa_addr && x->ifa_addr->sa_family == AF_INET && !strcmp(x->ifa_name, "eth0") &&
		    ((struct sockaddr_in *)x->ifa_addr)->sin_addr.s_addr == htonl(0x0a00020f))
			seen = 1;
	freeifaddrs(all);
	if (!seen) return 12;

	struct rtmsg rt = { .rtm_family = AF_INET, .rtm_table = RT_TABLE_MAIN, .rtm_type = RTN_UNICAST };
	if (request(nl, RTM_NEWROUTE, NLM_F_ACK | NLM_F_CREATE, &rt, sizeof rt) || ack(nl) != 0) return 13;

	int mark = 42;
	if (setsockopt(inet, SOL_SOCKET, SO_MARK, &mark, sizeof mark)) return 14;
	if (setsockopt(inet, SOL_SOCKET, SO_BINDTODEVICE, "eth0", 5)) return 15;
	struct sockaddr_in local = { .sin_family = AF_INET };
	inet_pton(AF_INET, "10.0.2.15", &local.sin_addr);
	if (bind(inet, (struct sockaddr *)&local, sizeof local)) return 16;

	int f = open("/proc/sys/net/ipv6/conf/eth0/disable_ipv6", O_WRONLY | O_TRUNC);
	if (f < 0 || write(f, "0\n", 2) != 2) return 17;
	close(f);
	f = open("/proc/sys/net/ipv6/conf/eth0/use_tempaddr", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (f < 0 || write(f, "2\n", 2) != 2) return 17;
	close(f);

	pid_t c = fork();
	if (c == 0) {
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		char cip[16];
		_exit(s < 0 || eth_addr(s, cip) || strcmp(cip, "10.0.2.15") ? 1 : 0);
	}
	int st;
	if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st)) return 18;

	/* Java's NetlinkSocket: plain write() and read(). */
	struct {
		struct nlmsghdr h;
		struct ifinfomsg ifi;
		struct rtattr a;
		char name[8];
	} q;
	memset(&q, 0, sizeof q);
	q.h.nlmsg_len = sizeof q;
	q.h.nlmsg_type = RTM_GETLINK;
	q.h.nlmsg_flags = NLM_F_REQUEST;
	q.h.nlmsg_seq = seq++;
	q.a.rta_type = IFLA_IFNAME;
	q.a.rta_len = RTA_LENGTH(5);
	strcpy(q.name, "eth0");
	char buf[4096] __attribute__((aligned(4)));
	if (write(nl, &q, sizeof q) != sizeof q) return 19;
	ssize_t n = read(nl, buf, sizeof buf);
	struct nlmsghdr *h = (struct nlmsghdr *)buf;
	if (n <= 0 || !NLMSG_OK(h, (size_t)n) || h->nlmsg_type != RTM_NEWLINK ||
	    h->nlmsg_seq != q.h.nlmsg_seq || h->nlmsg_pid != me.nl_pid ||
	    !(((struct ifinfomsg *)NLMSG_DATA(h))->ifi_flags & IFF_RUNNING))
		return 19;

	if (addr_req(nl, RTM_DELADDR, "10.0.2.15", 24) != 0) return 20;
	if (!heard(mon, RTM_DELADDR, RTMGRP_IPV4_IFADDR)) return 20;
	if (eth_addr(inet, ip) == 0) return 21;
	return 42;
}
