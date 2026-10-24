/* Tiny x86_64 static guest fixture: exit_group(42).
 *
 * Used by tests/unit/test_loader_smoke.c to verify the manual loader
 * (parser + mapper + stack synth + trampoline) end-to-end. We pick a
 * specific exit code (42, not 0) so the test fails clearly if the
 * binary never ran or ran the wrong path.
 *
 * Built as `-static -nostdlib` => ET_EXEC at the default base
 * (typically 0x400000). One PT_LOAD; no PT_INTERP. Tiny: a few hundred
 * bytes after stripping. Owns its own _start.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     $231, %rax       /* __NR_exit_group on x86_64 */
	mov     $42,  %rdi
	syscall
	ud2
	.size _start, .-_start

	.section .note.GNU-stack,"",@progbits
