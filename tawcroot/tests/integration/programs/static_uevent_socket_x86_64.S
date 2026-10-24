/* x86_64 port of static_uevent_socket_aarch64.S — see that file's
 * header for rationale, the libudev sequence being reproduced, and the
 * exit-code key.
 *
 * x86_64 syscall numbers:
 *   __NR_socket      =  41   __NR_bind       = 49
 *   __NR_getsockname =  51   __NR_setsockopt = 54
 *   __NR_close       =   3   __NR_exit_group = 231
 *
 * Stack frame (sub $64): snl at rsp+0, snl2 at rsp+16, len at rsp+40,
 * one at rsp+48.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib.
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	sub     $64, %rsp
	/* Zero the whole frame (8 quad stores). */
	xor     %rax, %rax
	mov     $8, %rcx
	mov     %rsp, %rdi
	rep stosq

	/* snl.nl_family = AF_NETLINK = 16 (u16 at offset 0) */
	movw    $16, (%rsp)

	/* socket(AF_NETLINK=16, SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,
	 *        NETLINK_KOBJECT_UEVENT=15). type = 3|0x800|0x80000. */
	mov     $16, %rdi
	mov     $0x80803, %rsi
	mov     $15, %rdx
	mov     $41, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_socket
	mov     %rax, %rbx               /* sock_fd */

	/* setsockopt(fd, SOL_SOCKET=1, SO_PASSCRED=16, &one, 4) */
	movl    $1, 48(%rsp)
	mov     %rbx, %rdi
	mov     $1, %rsi
	mov     $16, %rdx
	lea     48(%rsp), %r10
	mov     $4, %r8
	mov     $54, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_setsockopt

	/* bind(fd, &snl, 12) */
	mov     %rbx, %rdi
	mov     %rsp, %rsi
	mov     $12, %rdx
	mov     $49, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_bind

	/* getsockname(fd, &snl2, &len) with *len = 16 (buffer cap). */
	movl    $16, 40(%rsp)
	mov     %rbx, %rdi
	lea     16(%rsp), %rsi
	lea     40(%rsp), %rdx
	mov     $51, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_getsockname

	movzwl  16(%rsp), %eax           /* snl2.nl_family */
	cmp     $16, %eax
	jne     .Lfail_family
	movl    40(%rsp), %eax           /* reported addrlen */
	cmp     $12, %eax
	jne     .Lfail_len

	/* close(fd) */
	mov     %rbx, %rdi
	mov     $3, %rax
	syscall

	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_socket:
	neg     %rax
	cmp     $60, %rax
	jle     1f
	mov     $60, %rax
1:
	add     $90, %rax
	mov     %rax, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_bind:
	neg     %rax
	cmp     $60, %rax
	jle     1f
	mov     $60, %rax
1:
	add     $160, %rax
	mov     %rax, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_setsockopt:
	mov     $201, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_getsockname:
	mov     $206, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_family:
	mov     $207, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_len:
	mov     $208, %rdi
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
