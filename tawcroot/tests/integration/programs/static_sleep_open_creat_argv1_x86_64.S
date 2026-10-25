/* Static x86_64 fixture: nanosleep({1,0}, NULL), then open(argv[1],
 * O_WRONLY|O_CREAT|O_TRUNC, 0600), then exit_group(0).
 *
 * Used by the PDEATHSIG regression test in test_exec_child.c. The test
 * spawns this fixture inside a gpgme-shape double-fork where the
 * intermediate exits ~100 ms after the grandchild starts. If
 * arm_pdeathsig were (incorrectly) called on the --exec-child re-exec,
 * the kernel queues SIGKILL on the grandchild from forget_original_parent
 * during intermediate's do_exit, killing the fixture mid-nanosleep. The
 * marker file then never appears. Without the bug (current code skips
 * the bind on --exec-child) the fixture runs to completion and creates
 * the marker.
 *
 * Why a 1-second sleep: spans intermediate's exit with margin even on
 * slow Android arm cores, so the test is timing-resilient. Cost: every
 * test run pays ~1 s. Acceptable for a once-per-test regression guard.
 *
 * Built `-static -nostdlib`; same shape as static_open_creat_argv1.
 *
 * Exit codes (distinct from existing fixture codes):
 *   0   — sleep + open both succeeded (success)
 *   210 — argc < 2
 *   211 — open(2) returned an error
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check: must be >= 2 */
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	/* nanosleep({1, 0}, NULL).
	 * Use red-zone-style scratch on stack: timespec is 16 bytes
	 * (tv_sec=long, tv_nsec=long). Push two zero quads, then patch
	 * tv_sec=1 in place. Stack discipline below is fragile because
	 * argv[1] is at *(rsp+16) BEFORE the push but *(rsp+32) AFTER —
	 * we restore sp before the open. */
	sub     $16, %rsp
	movq    $1, (%rsp)              /* tv_sec = 1                      */
	movq    $0, 8(%rsp)             /* tv_nsec = 0                     */
	mov     %rsp, %rdi              /* req                             */
	xor     %rsi, %rsi              /* rem = NULL                      */
	mov     $35, %rax               /* SYS_nanosleep                   */
	syscall
	add     $16, %rsp

	/* openat(AT_FDCWD, argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600). */
	mov     16(%rsp), %rsi          /* path (argv[1])                  */
	mov     $-100, %rdi             /* AT_FDCWD                        */
	mov     $(1 | 0100 | 01000), %rdx /* O_WRONLY|O_CREAT|O_TRUNC      */
	mov     $0x180, %r10            /* mode 0600                       */
	mov     $257, %rax              /* SYS_openat                      */
	syscall
	test    %rax, %rax
	js      .Lfail_open

	xor     %rdi, %rdi              /* exit_group(0)                   */
	mov     $231, %rax
	syscall
	ud2

.Lfail_argc:
	mov     $210, %rdi
	jmp     .Lexit
.Lfail_open:
	mov     $211, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
