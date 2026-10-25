/* Aarch64 port of static_drop_ids_execve_x86_64.S — drop virtual ids,
 * verify irreversibility, execve("/bin/static_check_ids_exit42").
 * Exit-code key matches the x86_64 fixture (91..95 for local failures).
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x0, #994
	mov     x1, #994
	mov     x2, #994
	mov     x8, #149                 /* __NR_setresgid */
	svc     #0
	cbnz    x0, .Lfail91

	mov     x0, #1
	ldr     x1, =groups
	mov     x8, #159                 /* __NR_setgroups */
	svc     #0
	cbnz    x0, .Lfail92

	mov     x0, #994
	mov     x1, #994
	mov     x2, #994
	mov     x8, #147                 /* __NR_setresuid */
	svc     #0
	cbnz    x0, .Lfail93

	mov     x0, #0
	mov     x8, #146                 /* __NR_setuid(0) — must EPERM */
	svc     #0
	cbz     x0, .Lfail94

	ldr     x0, =path
	ldr     x1, =argv
	ldr     x2, =envp
	mov     x8, #221                 /* __NR_execve */
	svc     #0
	mov     x0, #95
	b       .Lexit

.Lfail91:
	mov     x0, #91
	b       .Lexit
.Lfail92:
	mov     x0, #92
	b       .Lexit
.Lfail93:
	mov     x0, #93
	b       .Lexit
.Lfail94:
	mov     x0, #94
.Lexit:
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .rodata
path:
	.asciz  "/bin/static_check_ids_exit42"
	.balign 8
argv:
	.quad   path
	.quad   0
envp:
	.quad   0
groups:
	.long   994
	.section .note.GNU-stack,"",%progbits
