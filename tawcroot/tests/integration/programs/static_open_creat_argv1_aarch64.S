/* Aarch64 port of static_open_creat_argv1_x86_64.S — see that file's
 * header for the full rationale. Same exit-code key:
 *   0   — open succeeded, file created
 *   200 — argc < 2
 *   201 — openat(2) returned an error
 *
 * Aarch64 has no plain SYS_open; we use openat(AT_FDCWD, ...).
 *   __NR_openat     = 56
 *   __NR_exit_group = 94
 *   AT_FDCWD        = -100
 *
 * Aarch64 ABI: x0..x5 = args, x8 = nr, svc #0.
 * sp on entry points at argc; argv at sp+8; argv[1] at sp+16.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	/* argc >= 2 ? */
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	/* openat(AT_FDCWD, argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600) */
	mov     x0, #-100               /* AT_FDCWD                      */
	ldr     x1, [sp, #16]           /* argv[1]                       */
	mov     x2, #(1 | 0100 | 01000) /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     x3, #0x180              /* mode 0600                     */
	mov     x8, #56                 /* __NR_openat                   */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_open

	mov     x0, #0                  /* exit_group(0)                 */
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_argc:
	mov     x0, #200
	b       .Lexit
.Lfail_open:
	mov     x0, #201
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
