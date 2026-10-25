/* Emulated binder devices for Android guests, served by binderd.
 *
 * Active when the bind table maps a host directory to /dev/binderfs:
 * that directory holds binderd's sockets (binder, hwbinder, vndbinder)
 * and its features/ files. /dev/{binder,hwbinder,vndbinder} and
 * /dev/binderfs/<name> then open a connection to binderd instead of a
 * kernel device. Wire protocol: include/binder_wire.h.
 *
 * TAWC_ENOSYS means "not a binder path/fd", the caller carries on.
 */

#pragma once

#include <stddef.h>
#include <ucontext.h>

long tawcroot_binder_open(int dirfd, const char *path, int flags);
long tawcroot_binder_ioctl(int fd, unsigned int cmd, long arg, ucontext_t *uc);
/* Forget a binder fd the guest is closing; the caller closes it. */
void tawcroot_binder_close(int fd);

/* Install the mmap handler (fd-backed maps at or above the reserved
 * floor trap, see filter_build.c). */
void tawcroot_binder_register(void);
