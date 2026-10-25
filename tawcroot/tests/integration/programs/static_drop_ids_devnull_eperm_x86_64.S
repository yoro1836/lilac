/* Static x86_64 fixture: privilege-gated chmod/chown against
 * root-owned /dev/null, before and after a virtual identity drop.
 * Guest-observable port of the testhost smoke's
 * test_identity_dropped_chmod_chown_real_errors — run it under the
 * tawcroot prod-env suite (real app uid, never root) and the
 * rooted-adbd euid-0 skip class from `de5f95d` cannot recur.
 * Requires a `/dev` bind (production always has one).
 *
 * Sequence:
 *   1. fchmodat(/dev/null, 0666) at virtual root -> 0 (faked success)
 *   2. setresgid(994,...) / setresuid(994,...)   -> 0 (virtual drop)
 *   3. fchmodat(/dev/null, 0666)                 -> real -EPERM/-EACCES
 *   4. fchownat(/dev/null, 0, 0)                 -> real -EPERM/-EACCES
 *   5. open(/dev/null) + fchown(fd, 0, 0)        -> real -EPERM/-EACCES
 *
 * Exit code key:
 *    42 — all steps held
 *   220 — virtual-root fchmodat was not faked to success
 *   221 — setresgid failed
 *   222 — setresuid failed
 *   223 — dropped fchmodat didn't return EPERM/EACCES
 *   224 — dropped fchownat didn't return EPERM/EACCES
 *   225 — open(/dev/null) failed
 *   226 — dropped fchown(fd) didn't return EPERM/EACCES
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* fchmodat(AT_FDCWD, "/dev/null", 0666) at virtual root -> 0 */
	mov     $-100, %rdi
	lea     path(%rip), %rsi
	mov     $0x1b6, %rdx              /* mode 0666                     */
	mov     $268, %rax                /* SYS_fchmodat                  */
	syscall
	test    %rax, %rax
	jnz     .Lfail220

	/* setresgid(994, 994, 994) -> 0 */
	mov     $994, %rdi
	mov     $994, %rsi
	mov     $994, %rdx
	mov     $119, %rax                /* SYS_setresgid                 */
	syscall
	test    %rax, %rax
	jnz     .Lfail221

	/* setresuid(994, 994, 994) -> 0 */
	mov     $994, %rdi
	mov     $994, %rsi
	mov     $994, %rdx
	mov     $117, %rax                /* SYS_setresuid                 */
	syscall
	test    %rax, %rax
	jnz     .Lfail222

	/* dropped fchmodat -> real -EPERM / -EACCES */
	mov     $-100, %rdi
	lea     path(%rip), %rsi
	mov     $0x1b6, %rdx
	mov     $268, %rax                /* SYS_fchmodat                  */
	syscall
	cmp     $-1, %rax                 /* -EPERM                        */
	je      1f
	cmp     $-13, %rax                /* -EACCES                       */
	jne     .Lfail223
1:
	/* dropped fchownat(/dev/null, 0, 0, 0) -> real -EPERM / -EACCES */
	mov     $-100, %rdi
	lea     path(%rip), %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	xor     %r8, %r8
	mov     $260, %rax                /* SYS_fchownat                  */
	syscall
	cmp     $-1, %rax
	je      2f
	cmp     $-13, %rax
	jne     .Lfail224
2:
	/* openat(AT_FDCWD, "/dev/null", O_RDONLY) */
	mov     $-100, %rdi
	lea     path(%rip), %rsi
	xor     %rdx, %rdx
	xor     %r10, %r10
	mov     $257, %rax                /* SYS_openat                    */
	syscall
	test    %rax, %rax
	js      .Lfail225
	mov     %rax, %rbx

	/* dropped fchown(fd, 0, 0) -> real -EPERM / -EACCES */
	mov     %rbx, %rdi
	xor     %rsi, %rsi
	xor     %rdx, %rdx
	mov     $93, %rax                 /* SYS_fchown                    */
	syscall
	cmp     $-1, %rax
	je      3f
	cmp     $-13, %rax
	jne     .Lfail226
3:
	mov     $42, %rdi
	jmp     .Lexit

.Lfail220:
	mov     $220, %rdi
	jmp     .Lexit
.Lfail221:
	mov     $221, %rdi
	jmp     .Lexit
.Lfail222:
	mov     $222, %rdi
	jmp     .Lexit
.Lfail223:
	mov     $223, %rdi
	jmp     .Lexit
.Lfail224:
	mov     $224, %rdi
	jmp     .Lexit
.Lfail225:
	mov     $225, %rdi
	jmp     .Lexit
.Lfail226:
	mov     $226, %rdi
.Lexit:
	mov     $231, %rax                /* SYS_exit_group                */
	syscall
	ud2
	.size _start, .-_start

	.section .rodata
path:
	.asciz  "/dev/null"

	.section .note.GNU-stack,"",@progbits
