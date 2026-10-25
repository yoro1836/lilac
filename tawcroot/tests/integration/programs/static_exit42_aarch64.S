/* Aarch64 static guest fixture: exit_group(42).
 *
 * Aarch64 syscall numbers (arch/arm64/include/uapi/asm/unistd.h):
 *   __NR_exit_group = 94
 *
 * Aarch64 syscall calling convention:
 *   x0..x5 = args, x8 = syscall number, svc #0 = trap.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x0, #42
	mov     x8, #94          /* __NR_exit_group */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
