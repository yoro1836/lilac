/* Static x86_64 fixture: the hardlink "publish" idiom, guest-observable.
 *
 * argv[1] = src path, argv[2] = dst path (absolute guest paths in a
 * writable part of the rootfs view).
 *
 * Sequence (mirrors how git publishes objects/packs):
 *   1. create src, write "publish-me" (10 bytes)
 *   2. linkat(src -> dst)                      -> 0
 *   3. linkat(src -> dst) again                -> -EEXIST
 *   4. open+read src, content intact           (v1 SELinux fallback
 *      leaves a back-symlink at src; open follows it)
 *   5. unlink src
 *   6. open+read dst, content intact           (the real file must
 *      live at the NEW name or the publish dangles)
 *   7. unlink dst
 *
 * This is the guest-observable contract of tawcroot's linkat handling
 * regardless of environment: on hosts where hardlinks just work it
 * passes through, and under Android SELinux hardlink denial
 * (untrusted_app on-device — the tawcroot prod-env suite) it exercises
 * the v1 rename+symlink fallback end to end. See
 * docs/tawcroot/testing.md "Device-environment sensitivities".
 *
 * Exit code key:
 *    42 — all steps held
 *   209 — argc < 3
 *   210 — openat(src, O_CREAT) failed
 *   211 — write != 10
 *   212 — linkat(src -> dst) failed
 *   213 — second linkat != -EEXIST
 *   214 — open src after link failed
 *   215 — src content mismatch after link
 *   216 — unlinkat(src) failed
 *   217 — open dst after source unlink failed
 *   218 — dst content mismatch after source unlink
 *   219 — unlinkat(dst) failed
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rax              /* argc                          */
	cmp     $3, %rax
	jl      .Lfail209
	mov     16(%rsp), %r12            /* src = argv[1]                 */
	mov     24(%rsp), %r13            /* dst = argv[2]                 */

	/* openat(AT_FDCWD, src, O_WRONLY|O_CREAT|O_TRUNC, 0644) */
	mov     $-100, %rdi
	mov     %r12, %rsi
	mov     $(1 | 0100 | 01000), %rdx
	mov     $0x1a4, %r10              /* mode 0644                     */
	mov     $257, %rax                /* SYS_openat                    */
	syscall
	test    %rax, %rax
	js      .Lfail210
	mov     %rax, %rbx

	/* write(fd, "publish-me", 10) == 10 */
	mov     %rbx, %rdi
	lea     msg(%rip), %rsi
	mov     $10, %rdx
	mov     $1, %rax                  /* SYS_write                     */
	syscall
	cmp     $10, %rax
	jne     .Lfail211

	mov     %rbx, %rdi
	mov     $3, %rax                  /* SYS_close                     */
	syscall

	/* linkat(AT_FDCWD, src, AT_FDCWD, dst, 0) -> 0 */
	mov     $-100, %rdi
	mov     %r12, %rsi
	mov     $-100, %rdx
	mov     %r13, %r10
	xor     %r8, %r8
	mov     $265, %rax                /* SYS_linkat                    */
	syscall
	test    %rax, %rax
	jnz     .Lfail212

	/* linkat onto the existing dst -> -EEXIST */
	mov     $-100, %rdi
	mov     %r12, %rsi
	mov     $-100, %rdx
	mov     %r13, %r10
	xor     %r8, %r8
	mov     $265, %rax                /* SYS_linkat                    */
	syscall
	cmp     $-17, %rax                /* -EEXIST                       */
	jne     .Lfail213

	/* src still opens and holds the content */
	mov     %r12, %rdi
	call    check_content
	test    %rax, %rax
	jnz     .Lfail_src_check          /* rax: 1 = open, 2 = content    */

	/* unlinkat(AT_FDCWD, src, 0) -> 0 */
	mov     $-100, %rdi
	mov     %r12, %rsi
	xor     %rdx, %rdx
	mov     $263, %rax                /* SYS_unlinkat                  */
	syscall
	test    %rax, %rax
	jnz     .Lfail216

	/* dst still opens and holds the content */
	mov     %r13, %rdi
	call    check_content
	test    %rax, %rax
	jnz     .Lfail_dst_check

	/* unlinkat(AT_FDCWD, dst, 0) -> 0 */
	mov     $-100, %rdi
	mov     %r13, %rsi
	xor     %rdx, %rdx
	mov     $263, %rax                /* SYS_unlinkat                  */
	syscall
	test    %rax, %rax
	jnz     .Lfail219

	mov     $42, %rdi
	jmp     .Lexit

/* check_content(path in %rdi): open O_RDONLY, read 10 bytes, compare
 * against msg. Returns %rax = 0 ok, 1 open failed, 2 content mismatch.
 * Clobbers caller-saved regs + %r14. */
check_content:
	mov     %rdi, %rsi
	mov     $-100, %rdi
	xor     %rdx, %rdx                /* O_RDONLY                      */
	xor     %r10, %r10
	mov     $257, %rax                /* SYS_openat                    */
	syscall
	test    %rax, %rax
	js      .Lcc_open_fail
	mov     %rax, %r14

	mov     %r14, %rdi
	lea     buf(%rip), %rsi
	mov     $10, %rdx
	xor     %rax, %rax                /* SYS_read                      */
	syscall
	push    %rax                      /* byte count                    */
	mov     %r14, %rdi
	mov     $3, %rax                  /* SYS_close                     */
	syscall
	pop     %rax
	cmp     $10, %rax
	jne     .Lcc_mismatch

	xor     %rcx, %rcx
.Lcc_loop:
	lea     buf(%rip), %rsi
	movzbl  (%rsi,%rcx), %eax
	lea     msg(%rip), %rsi
	movzbl  (%rsi,%rcx), %edx
	cmp     %al, %dl
	jne     .Lcc_mismatch
	inc     %rcx
	cmp     $10, %rcx
	jl      .Lcc_loop
	xor     %rax, %rax
	ret
.Lcc_open_fail:
	mov     $1, %rax
	ret
.Lcc_mismatch:
	mov     $2, %rax
	ret

.Lfail_src_check:
	cmp     $1, %rax
	je      .Lfail214
	mov     $215, %rdi
	jmp     .Lexit
.Lfail_dst_check:
	cmp     $1, %rax
	je      .Lfail217
	mov     $218, %rdi
	jmp     .Lexit

.Lfail209:
	mov     $209, %rdi
	jmp     .Lexit
.Lfail210:
	mov     $210, %rdi
	jmp     .Lexit
.Lfail211:
	mov     $211, %rdi
	jmp     .Lexit
.Lfail212:
	mov     $212, %rdi
	jmp     .Lexit
.Lfail213:
	mov     $213, %rdi
	jmp     .Lexit
.Lfail214:
	mov     $214, %rdi
	jmp     .Lexit
.Lfail216:
	mov     $216, %rdi
	jmp     .Lexit
.Lfail217:
	mov     $217, %rdi
	jmp     .Lexit
.Lfail219:
	mov     $219, %rdi
.Lexit:
	mov     $231, %rax                /* SYS_exit_group                */
	syscall
	ud2
	.size _start, .-_start

	.section .rodata
msg:
	.ascii  "publish-me"

	.bss
	.align  8
buf:
	.skip   16

	.section .note.GNU-stack,"",@progbits
