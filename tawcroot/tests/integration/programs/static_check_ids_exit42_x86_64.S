/* Static x86_64 fixture: exit 42 iff the virtual identity says
 * uid/euid/gid/egid == 994 and the supplementary-group shadow is
 * exactly {994}. Run as the exec target of static_drop_ids_execve to
 * prove the dropped identity survived the SIGSYS-handler execve
 * (exec_state ferry). Distinct exit codes bisect which getter
 * regressed: 43 getuid, 44 geteuid, 45 getgid, 46 getegid,
 * 47 getgroups.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     $102, %rax             /* SYS_getuid */
	syscall
	cmp     $994, %rax
	jne     .Lfail43

	mov     $107, %rax             /* SYS_geteuid */
	syscall
	cmp     $994, %rax
	jne     .Lfail44

	mov     $104, %rax             /* SYS_getgid */
	syscall
	cmp     $994, %rax
	jne     .Lfail45

	mov     $108, %rax             /* SYS_getegid */
	syscall
	cmp     $994, %rax
	jne     .Lfail46

	sub     $128, %rsp
	mov     $32, %rdi
	mov     %rsp, %rsi
	mov     $115, %rax             /* SYS_getgroups */
	syscall
	cmp     $1, %rax               /* exactly one group... */
	jne     .Lfail47
	cmpl    $994, (%rsp)           /* ...and it is 994 */
	jne     .Lfail47

	mov     $42, %rdi
	jmp     .Lexit
.Lfail43:
	mov     $43, %rdi
	jmp     .Lexit
.Lfail44:
	mov     $44, %rdi
	jmp     .Lexit
.Lfail45:
	mov     $45, %rdi
	jmp     .Lexit
.Lfail46:
	mov     $46, %rdi
	jmp     .Lexit
.Lfail47:
	mov     $47, %rdi
.Lexit:
	mov     $231, %rax             /* exit_group */
	syscall
	ud2

	.section .note.GNU-stack,"",@progbits
