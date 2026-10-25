/* Static x86_64 fixture: socket(AF_UNIX); bind to sun_path = argv[1];
 * getsockname and verify it reverse-translates back to argv[1];
 * close; exit_group(42).
 *
 * Used by tests/integration/test_prod_features.c to validate AF_UNIX
 * bind() sun_path translation (syscalls_socket.c). The gpg-agent
 * regression — `bind(/run/.gnupg/S.gpg-agent)` looking for `/run/`
 * on the host filesystem instead of inside the rootfs — is the canonical
 * shape this tests against. The getsockname check additionally proves
 * the address is reverse-translated back to the guest view (a server
 * re-publishing its bound path must advertise a guest path, not the
 * host one).
 *
 * argv: [self, sun_path]
 *
 * sockaddr_un layout (matches Linux's struct sockaddr_un):
 *   u16  sun_family = AF_UNIX (1)
 *   char sun_path[108]    (NUL-terminated; we zero the rest)
 *
 * Stack frame (sub $304): addr at rsp+0, addr2 at rsp+128, len2 at
 * rsp+256. argv[1] is at rsp+320 (16 + 304). addrlen passed to bind is
 * the full 110-byte struct size — Linux ignores trailing padding past
 * the NUL.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib.
 *
 * Exit code key:
 *    42 — socket+bind+getsockname(match)+close all succeeded
 *  90+errno (cap 60) — socket failed (codes 90..150)
 * 160+errno (cap 60) — bind failed   (codes 160..220)
 *   200 — argc < 2
 *   201 — sun_path too long (≥ 108 chars)
 *   205 — getsockname path != argv[1] (reverse translation broken)
 *   206 — getsockname syscall failed
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* argc check */
	mov     (%rsp), %rax
	cmp     $2, %rax
	jl      .Lfail_argc

	/* Reserve 304 bytes: addr(128) + addr2(128) + len2(8) + slack. */
	sub     $304, %rsp
	mov     %rsp, %rbx               /* &addr base */
	/* Zero addr + addr2 (256 bytes) via 32 quad stores. */
	xor     %rax, %rax
	mov     $32, %rcx
	mov     %rbx, %rdi
	rep stosq

	/* sun_family = AF_UNIX = 1 (u16 at offset 0) */
	movw    $1, (%rbx)

	/* Copy argv[1] bytes to (rbx+2), bound at 107. argv[1] @ rsp+320. */
	mov     320(%rsp), %rsi          /* argv[1] */
	lea     2(%rbx), %rdi            /* &addr.sun_path[0] */
	xor     %rcx, %rcx
.Lcpy:
	cmp     $107, %rcx
	jge     .Lpath_too_long_post
	movzbl  (%rsi, %rcx), %eax
	test    %al, %al
	jz      .Lcpy_done
	mov     %al, (%rdi, %rcx)
	inc     %rcx
	jmp     .Lcpy
.Lcpy_done:

	/* socket(AF_UNIX=1, SOCK_STREAM=1, 0). nr=41. */
	mov     $1, %rdi
	mov     $1, %rsi
	xor     %rdx, %rdx
	mov     $41, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_socket
	mov     %rax, %r12               /* sock_fd */

	/* bind(sock_fd, &addr, 110). nr=49. */
	mov     %r12, %rdi
	mov     %rbx, %rsi
	mov     $110, %rdx
	mov     $49, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_bind

	/* getsockname(sock_fd, &addr2, &len2). nr=51.
	 * *len2 = 128 (buffer cap) first. */
	movl    $128, 256(%rsp)
	mov     %r12, %rdi
	lea     128(%rsp), %rsi          /* &addr2 */
	lea     256(%rsp), %rdx          /* &len2 */
	mov     $51, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_getsockname

	/* Compare addr2.sun_path (rsp+130) to argv[1] (rsp+320). */
	lea     130(%rsp), %rdi
	mov     320(%rsp), %rsi
	xor     %rcx, %rcx
.Lcmp:
	movzbl  (%rsi, %rcx), %eax       /* argv[1][i] */
	movzbl  (%rdi, %rcx), %edx       /* sun_path[i] */
	cmp     %al, %dl
	jne     .Lfail_cmp
	test    %al, %al
	jz      .Lcmp_done
	inc     %rcx
	cmp     $108, %rcx
	jge     .Lfail_cmp
	jmp     .Lcmp
.Lcmp_done:

	/* close(sock_fd). nr=3. */
	mov     %r12, %rdi
	mov     $3, %rax
	syscall

	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_socket:
	neg     %rax
	cmp     $60, %rax
	jbe     1f
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
	jbe     1f
	mov     $60, %rax
1:
	add     $160, %rax
	mov     %rax, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_cmp:
	mov     $205, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_getsockname:
	mov     $206, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lpath_too_long_post:
	mov     $201, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_argc:
	mov     $200, %rdi
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start
	.section .note.GNU-stack,"",@progbits
