/* x86_64 static guest fixture exercising argc / argv / envp / auxv
 * access. Built `-static -nostdlib` (ET_EXEC, no PT_INTERP).
 *
 * Exit code encoding:
 *   99   — argc != 3            (argv plumbing broken)
 *   98   — argv[0][0] != '/'   (argv pointer didn't reach a string)
 *   97   — envp[0] missing or didn't start with 'X'  (envp broken)
 *   96   — auxv walked past AT_NULL without finding AT_RANDOM (no auxv)
 *   100..227  — first byte of AT_RANDOM, masked to 7 bits, plus 100.
 *               The test passes a deterministic 16-byte AT_RANDOM and
 *               checks the lower 7 bits of byte 0.
 *
 * This is the strongest stack-synth validation the loader pipeline
 * gets without booting a real libc: any pointer the synth got wrong,
 * any auxv entry it forgot, any alignment slip will surface as a
 * clear non-success exit.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check: must be 3 */
	mov     (%rsp), %rax              /* argc                          */
	cmp     $3, %rax
	jne     .Lfail_argc

	/* argv[0] check: first char of argv[0] must be '/' (test passes
	 * an absolute path). NB: cmpb directly — `mov (%rdi),%al` would
	 * clobber the low byte of %rax, corrupting argc for the lea below.
	 *   argv lives at &(%rsp + 8); argv[0] = *(rsp+8). */
	mov     8(%rsp), %rdi             /* argv[0] pointer               */
	cmpb    $'/', (%rdi)
	jne     .Lfail_argv

	/* Walk past the (argc+1) argv pointers (NULL-terminated) to land
	 * on envp[0]. */
	lea     8(%rsp), %rdi             /* rdi = &argv[0]               */
	lea     8(%rdi, %rax, 8), %rdi    /* rdi = argv + argc + 1 = &envp[0] */

	/* envp[0]: must exist and start with 'X' (test passes "X=1"). */
	mov     (%rdi), %rcx
	test    %rcx, %rcx
	je      .Lfail_envp                /* envp[0] is NULL              */
	cmpb    $'X', (%rcx)
	jne     .Lfail_envp

	/* Walk past envp NULL terminator to land on auxv[0]. */
.Lenv_loop:
	cmpq    $0, (%rdi)
	je      .Lenv_done
	add     $8, %rdi
	jmp     .Lenv_loop
.Lenv_done:
	add     $8, %rdi                  /* skip the envp NULL slot      */

	/* Walk auxv looking for AT_RANDOM (25). Each entry is two qwords:
	 * (a_type, a_val). Stop on AT_NULL (0). */
.Laux_loop:
	mov     (%rdi), %rax              /* a_type                        */
	test    %rax, %rax
	je      .Lfail_no_random           /* AT_NULL: never found AT_RANDOM */
	cmp     $25, %rax
	je      .Laux_random_found
	add     $16, %rdi
	jmp     .Laux_loop

.Laux_random_found:
	mov     8(%rdi), %rcx             /* a_val = pointer to 16 random bytes */
	movzbq  (%rcx), %rdi              /* first random byte             */
	and     $0x7f, %rdi               /* mask to 7 bits                */
	add     $100, %rdi                /* in [100, 227]                 */
	mov     $231, %rax                /* exit_group                    */
	syscall
	ud2

.Lfail_argc:
	mov     $99, %rdi
	jmp     .Lexit

.Lfail_argv:
	mov     $98, %rdi
	jmp     .Lexit

.Lfail_envp:
	mov     $97, %rdi
	jmp     .Lexit

.Lfail_no_random:
	mov     $96, %rdi
.Lexit:
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
