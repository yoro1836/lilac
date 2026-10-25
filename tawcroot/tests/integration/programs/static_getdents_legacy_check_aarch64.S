/* Aarch64 twin of static_getdents_legacy_check: exit_group(96)
 * unconditionally. Legacy getdents(2) does not exist on aarch64 (NR
 * 78 is readlinkat there), so there is nothing to probe. The exit
 * code is deliberately NOT 42: the prod test using this fixture is
 * x86_64-gated, and if that gate is ever lost the run fails loudly
 * instead of vacuously passing. */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x0, #96
	mov     x8, #94          /* __NR_exit_group */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
