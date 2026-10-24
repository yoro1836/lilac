/* Aarch64 port of static_execve_proc_self_exe_x86_64.S — see that
 * file's header for rationale and the exit-code key.
 *
 * Aarch64 syscall numbers:
 *   __NR_execve     = 221
 *   __NR_exit_group = 94
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]                 /* argc */
	cmp     x0, #2
	b.ge    .Ldone42

	ldr     x0, =path
	ldr     x1, =argv
	ldr     x2, =envp
	mov     x8, #221                 /* __NR_execve */
	svc     #0

	/* execve returned -errno. Encode as 90 + min(60, errno). */
	neg     x0, x0
	cmp     x0, #60
	b.le    .Lcap_ok
	mov     x0, #60
.Lcap_ok:
	add     x0, x0, #90
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0
	brk     #0

.Ldone42:
	mov     x0, #42
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .rodata
path:
	.asciz  "/proc/self/exe"
arg1:
	.asciz  "again"
	.balign 8
argv:
	.quad   path
	.quad   arg1
	.quad   0
envp:
	.quad   0
	.section .note.GNU-stack,"",%progbits
