/* Aarch64 port of static_check_ids_exit42_x86_64.S — exit 42 iff
 * uid/euid/gid/egid == 994 and getgroups reports exactly {994}.
 * Exit-code key matches the x86_64 fixture (43..47).
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x8, #174                 /* __NR_getuid */
	svc     #0
	cmp     x0, #994
	b.ne    .Lfail43

	mov     x8, #175                 /* __NR_geteuid */
	svc     #0
	cmp     x0, #994
	b.ne    .Lfail44

	mov     x8, #176                 /* __NR_getgid */
	svc     #0
	cmp     x0, #994
	b.ne    .Lfail45

	mov     x8, #177                 /* __NR_getegid */
	svc     #0
	cmp     x0, #994
	b.ne    .Lfail46

	sub     sp, sp, #144             /* keep sp 16-aligned */
	mov     x0, #32
	mov     x1, sp
	mov     x8, #158                 /* __NR_getgroups */
	svc     #0
	cmp     x0, #1                   /* exactly one group... */
	b.ne    .Lfail47
	ldr     w0, [sp]
	cmp     w0, #994                 /* ...and it is 994 */
	b.ne    .Lfail47

	mov     x0, #42
	b       .Lexit
.Lfail43:
	mov     x0, #43
	b       .Lexit
.Lfail44:
	mov     x0, #44
	b       .Lexit
.Lfail45:
	mov     x0, #45
	b       .Lexit
.Lfail46:
	mov     x0, #46
	b       .Lexit
.Lfail47:
	mov     x0, #47
.Lexit:
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
