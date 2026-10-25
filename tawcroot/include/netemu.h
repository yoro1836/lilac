/* `--fake-net`: an emulated loopback + eth0 for init sessions (netemu.c). */
#pragma once
#include <ucontext.h>
#include "dispatch.h"

/* The fake eth0's index; lo is 1. */
#define TAWCROOT_NETEMU_ETH_INDEX 2

/* After --init: reset the link table, populate the fake /sys/class/net
 * and /proc/sys/net and bind them, and stack the traps netemu needs. */
long tawcroot_netemu_setup(void);

/* socket(AF_NETLINK, *, NETLINK_ROUTE) in a fake-net session: a stub
 * fd. TAWC_ENOSYS for every other socket. */
long tawcroot_netemu_socket(long domain, long type, long proto);

/* bind/connect/sendto/sendmsg/recvfrom/recvmsg/read/write/getsockname/
 * getpeername/getsockopt on a netlink stub (and bind to the fake
 * address), else TAWC_ENOSYS. */
long tawcroot_netemu_call(const tawcroot_syscall_args *a, ucontext_t *uc);

/* SIOC* interface ioctls, else TAWC_ENOSYS. */
long tawcroot_netemu_ioctl(int fd, unsigned cmd, long arg);

/* Lazy handlers (trapped from setup on). */
long tawcroot_netemu_setsockopt(const tawcroot_syscall_args *a, ucontext_t *uc);
long tawcroot_netemu_read(const tawcroot_syscall_args *a, ucontext_t *uc);
