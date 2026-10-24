/* Aarch64 port of static_unix_bind_argv1_x86_64.S — see that file's
 * header for rationale and exit-code key.
 *
 * Aarch64 syscall numbers:
 *   __NR_socket       = 198
 *   __NR_bind         = 200
 *   __NR_getsockname  = 204
 *   __NR_close        =  57
 *   __NR_exit_group   =  94
 *
 * Stack frame (sub 304): addr at sp+0, addr2 at sp+128, len2 at sp+256.
 * argv[1] is at sp+320 (16 + 304).
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	/* Reserve 304 bytes: addr(128) + addr2(128) + len2(8) + slack.
	 * Zero the first 256 (addr + addr2). */
	sub     sp, sp, #304
	mov     x9, sp                 /* &addr */
	mov     x10, #256
	mov     x11, #0
.Lzero_loop:
	subs    x10, x10, #8
	str     x11, [x9, x10]
	b.gt    .Lzero_loop
	str     x11, [x9]

	/* sun_family = AF_UNIX = 1 (u16 at offset 0) */
	mov     w11, #1
	strh    w11, [x9]

	/* argv[1] is at sp+320. */
	ldr     x12, [sp, #320]
	add     x13, x9, #2            /* &addr.sun_path[0] */
	mov     x14, #0
.Lcpy:
	cmp     x14, #107
	b.ge    .Lpath_too_long
	ldrb    w15, [x12, x14]
	cbz     w15, .Lcpy_done
	strb    w15, [x13, x14]
	add     x14, x14, #1
	b       .Lcpy
.Lcpy_done:

	/* socket(AF_UNIX=1, SOCK_STREAM=1, 0). nr=198. */
	mov     x0, #1
	mov     x1, #1
	mov     x2, #0
	mov     x8, #198
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_socket
	mov     x19, x0                /* sock_fd */

	/* bind(sock_fd, &addr, 110). nr=200. */
	mov     x0, x19
	mov     x1, x9
	mov     x2, #110
	mov     x8, #200
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_bind

	/* getsockname(sock_fd, &addr2, &len2). nr=204.
	 * *len2 = 128 (buffer cap) first. addr2 = sp+128, len2 = sp+256. */
	mov     w11, #128
	str     w11, [sp, #256]
	mov     x0, x19
	add     x1, sp, #128           /* &addr2 */
	add     x2, sp, #256           /* &len2 */
	mov     x8, #204
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_getsockname

	/* Compare addr2.sun_path (sp+130) to argv[1] (sp+320). */
	add     x13, sp, #130          /* &addr2.sun_path[0] */
	ldr     x12, [sp, #320]        /* argv[1] */
	mov     x14, #0
.Lcmp:
	ldrb    w15, [x12, x14]        /* argv[1][i] */
	ldrb    w16, [x13, x14]        /* sun_path[i] */
	cmp     w15, w16
	b.ne    .Lfail_cmp
	cbz     w15, .Lcmp_done
	add     x14, x14, #1
	cmp     x14, #108
	b.ge    .Lfail_cmp
	b       .Lcmp
.Lcmp_done:

	/* close(sock_fd) */
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

.Lfail_cmp:
	mov     x0, #205
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_getsockname:
	mov     x0, #206
	mov     x8, #94
	svc     #0
	brk     #0

.Lpath_too_long:
	mov     x0, #201
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_argc:
	mov     x0, #200
	mov     x8, #94
	svc     #0
	brk     #0

	.size _start, .-_start
	.section .note.GNU-stack,"",%progbits
