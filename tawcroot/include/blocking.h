/* Blocking calls made from the SIGSYS handler (blocking.c). */
#pragma once
#include <ucontext.h>

/* A handler's "re-run this syscall once the pending signal's handler
 * returns" (the kernel's -ERESTARTSYS): handler.c rewinds the trapped
 * instruction instead of returning. Never reaches the guest. */
#define TAWC_ERESTARTSYS (-512)

/* Before a call that may block on `fd`: wait until it has `events` or
 * an unblocked guest signal is pending. 0 to go ahead, TAWC_EAGAIN on
 * SO_RCVTIMEO expiry, TAWC_ERESTARTSYS / TAWC_EINTR for a signal. */
long tawcroot_wait_fd(int fd, short events, ucontext_t *uc);
