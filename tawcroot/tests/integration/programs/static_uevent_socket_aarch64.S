/* Static aarch64 fixture: run libudev's uevent-monitor bring-up
 * sequence and exit_group(42) if it completes.
 *
 * Android's SELinux policy denies untrusted_app
 * `socket(AF_NETLINK, …, NETLINK_KOBJECT_UEVENT)` with EACCES, and
 * libudev turns that into a NULL monitor, which turns into
 * "SDL_INIT_HAPTIC failed", which — because SDL3's SDL_Init is atomic —
 * tears the guest's already-initialized video subsystem back down
 * (issue: sdl-haptic-udev-netlink-kills-video-init). syscalls_socket.c
 * answers with a silent AF_UNIX stub instead; this pins that the stub
 * satisfies every call libudev makes.
 *
 * This fixture only means something in the prod-env suite, which runs
 * production tawcroot at the app uid in the untrusted_app domain — the
 * one context where the kernel actually refuses. Run on the host or as
 * the adb shell uid it may well get a real netlink socket, and the same
 * assertions hold, so it is a valid check either way.
 *
 * Sequence (device-monitor.c: device_monitor_new_full):
 *   socket(AF_NETLINK, SOCK_RAW|CLOEXEC|NONBLOCK, NETLINK_KOBJECT_UEVENT)
 *   setsockopt(SOL_SOCKET, SO_PASSCRED, 1)
 *   bind(&sockaddr_nl{AF_NETLINK, pid 0, groups 0}, 12)
 *   getsockname() -> must report AF_NETLINK and 12 bytes, because
 *                    monitor_set_nl_address() reads nl_pid back out of
 *                    it and fails the monitor if the call errors.
 *
 * Aarch64 syscall numbers:
 *   __NR_socket      = 198   __NR_bind       = 200
 *   __NR_getsockname = 204   __NR_setsockopt = 208
 *   __NR_close       =  57   __NR_exit_group =  94
 *
 * Stack frame (sub 64): snl at sp+0, snl2 at sp+16, len at sp+40,
 * one at sp+48.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib.
 *
 * Exit code key:
 *    42 — whole sequence succeeded
 *  90+errno (cap 60) — socket failed (codes 90..150)
 * 160+errno (cap 60) — bind failed   (codes 160..220)
 *   201 — setsockopt(SO_PASSCRED) failed
 *   206 — getsockname syscall failed
 *   207 — getsockname family != AF_NETLINK
 *   208 — getsockname addrlen != 12
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	sub     sp, sp, #64
	mov     x11, #0
	str     x11, [sp, #0]
	str     x11, [sp, #8]
	str     x11, [sp, #16]
	str     x11, [sp, #24]
	str     x11, [sp, #32]
	str     x11, [sp, #40]
	str     x11, [sp, #48]
	str     x11, [sp, #56]

	/* snl.nl_family = AF_NETLINK = 16 (u16 at offset 0) */
	mov     w11, #16
	strh    w11, [sp]

	/* socket(AF_NETLINK=16, SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,
	 *        NETLINK_KOBJECT_UEVENT=15). type = 3|0x800|0x80000. */
	mov     x0, #16
	mov     w1, #0x803
	movk    w1, #0x8, lsl #16
	mov     x2, #15
	mov     x8, #198
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_socket
	mov     x19, x0                /* sock_fd */

	/* setsockopt(fd, SOL_SOCKET=1, SO_PASSCRED=16, &one, 4) */
	mov     w11, #1
	str     w11, [sp, #48]
	mov     x0, x19
	mov     x1, #1
	mov     x2, #16
	add     x3, sp, #48
	mov     x4, #4
	mov     x8, #208
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_setsockopt

	/* bind(fd, &snl, 12) */
	mov     x0, x19
	mov     x1, sp
	mov     x2, #12
	mov     x8, #200
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_bind

	/* getsockname(fd, &snl2, &len) with *len = 16 (buffer cap). */
	mov     w11, #16
	str     w11, [sp, #40]
	mov     x0, x19
	add     x1, sp, #16
	add     x2, sp, #40
	mov     x8, #204
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_getsockname

	ldrh    w12, [sp, #16]         /* snl2.nl_family */
	cmp     w12, #16
	b.ne    .Lfail_family
	ldr     w13, [sp, #40]         /* reported addrlen */
	cmp     w13, #12
	b.ne    .Lfail_len

	/* close(fd) */
	mov     x0, x19
	mov     x8, #57
	svc     #0

	mov     x0, #42
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_socket:
	neg     x0, x0
	cmp     x0, #60
	b.le    1f
	mov     x0, #60
1:
	add     x0, x0, #90
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_bind:
	neg     x0, x0
	cmp     x0, #60
	b.le    1f
	mov     x0, #60
1:
	add     x0, x0, #160
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_setsockopt:
	mov     x0, #201
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_getsockname:
	mov     x0, #206
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_family:
	mov     x0, #207
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_len:
	mov     x0, #208
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start
	.section .note.GNU-stack,"",%progbits
