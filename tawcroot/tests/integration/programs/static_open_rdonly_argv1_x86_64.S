/* Static x86_64 fixture: open(argv[1], O_RDONLY) then exit_group(0).
 *
 * Used by the cross-bind-symlink integration test: argv[1] is the
 * in-rootfs path of a symlink whose target lies in a different bind
 * (e.g. /a/link → /b/target.txt). The test pre-creates the target,
 * runs this fixture, and asserts exit 0. With the openat2/RESOLVE_IN_ROOT
 * bug the absolute symlink target is re-rooted at the dirfd of bind A,
 * the open returns ENOENT, and the fixture exits 201.
 *
 * Built `-static -nostdlib` so it loads at the default base
 * with no PT_INTERP — same shape as static_open_creat_argv1.
 *
 * Exit code encoding (intentionally distinct from any tawcroot loader
 * code in 60..89, the static_argc_random codes 96..99/100..227, the
 * static_execve_exit42 codes 90..150, and the static_open_creat_argv1
 * codes 200/201):
 *   0   — open succeeded
 *   200 — argc < 2 (argv plumbing broken)
 *   201 — open(2) returned an error
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check: must be >= 2 */
	mov     (%rsp), %rax              /* argc                          */
	cmp     $2, %rax
	jl      .Lfail_argc

	/* argv[1] is the path to open. argv at rsp+8; argv[1] = *(rsp+16). */
	mov     16(%rsp), %rdi            /* path                          */
	xor     %rsi, %rsi                /* O_RDONLY                      */
	xor     %rdx, %rdx                /* mode (ignored without O_CREAT)*/
	mov     $2, %rax                  /* SYS_open                      */
	syscall
	test    %rax, %rax
	js      .Lfail_open

	xor     %rdi, %rdi                /* exit_group(0)                 */
	mov     $231, %rax
	syscall
	ud2

.Lfail_argc:
	mov     $200, %rdi
	jmp     .Lexit
.Lfail_open:
	mov     $201, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
