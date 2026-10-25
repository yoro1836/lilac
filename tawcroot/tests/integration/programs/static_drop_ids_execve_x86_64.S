/* Static x86_64 fixture: drop virtual ids (sshd's preauth shape), then
 * execve("/bin/static_check_ids_exit42").
 *
 * Sequence (mirrors OpenSSH permanently_set_uid):
 *   setresgid(994,994,994) -> 0   else exit 91
 *   setgroups(1, {994})    -> 0   else exit 92
 *   setresuid(994,994,994) -> 0   else exit 93
 *   setuid(0)              -> -EPERM; a 0 return means the drop was
 *                             reversible (the pre-virtual-identity
 *                             stateless fake) -> exit 94
 *   execve(...)            -> should not return; on -errno exit 95
 *
 * The exec target asserts the dropped ids SURVIVED the execve
 * (exec_state identity ferry) and exits 42.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     $994, %rdi
	mov     $994, %rsi
	mov     $994, %rdx
	mov     $119, %rax             /* SYS_setresgid */
	syscall
	test    %rax, %rax
	jnz     .Lfail91

	mov     $1, %rdi
	movabs  $groups, %rsi
	mov     $116, %rax             /* SYS_setgroups */
	syscall
	test    %rax, %rax
	jnz     .Lfail92

	mov     $994, %rdi
	mov     $994, %rsi
	mov     $994, %rdx
	mov     $117, %rax             /* SYS_setresuid */
	syscall
	test    %rax, %rax
	jnz     .Lfail93

	xor     %rdi, %rdi
	mov     $105, %rax             /* SYS_setuid(0) — must EPERM */
	syscall
	test    %rax, %rax
	jz      .Lfail94

	movabs  $path, %rdi
	movabs  $argv, %rsi
	movabs  $envp, %rdx
	mov     $59, %rax              /* SYS_execve */
	syscall
	mov     $95, %rdi
	jmp     .Lexit

.Lfail91:
	mov     $91, %rdi
	jmp     .Lexit
.Lfail92:
	mov     $92, %rdi
	jmp     .Lexit
.Lfail93:
	mov     $93, %rdi
	jmp     .Lexit
.Lfail94:
	mov     $94, %rdi
.Lexit:
	mov     $231, %rax             /* exit_group */
	syscall
	ud2

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
	.section .note.GNU-stack,"",@progbits
