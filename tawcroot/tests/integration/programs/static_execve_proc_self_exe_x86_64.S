/* Static x86_64 fixture: re-exec self via execve("/proc/self/exe").
 *
 * busybox's standalone shell (Debian's config) runs nearly every
 * applet by exec'ing /proc/self/exe. At the kernel level that link
 * points at libtawcroot.so, so without the exec handler's guest-exe
 * substitution the loader tries to map tawcroot's own image over the
 * running incarnation and dies with LOADER_FAIL(68). The handler must
 * substitute the stashed guest exe path — the same synthesis
 * readlinkat performs — so this fixture must find *itself* on the
 * other side of the exec.
 *
 * Protocol: first invocation has argc == 1 (tawcroot passes just the
 * path); it execve's /proc/self/exe with argv {path, "again"}. The
 * re-exec'd image sees argc >= 2 and exits 42. Exit-code key matches
 * static_execve_exit42: 42 = pass, 90 + min(60, errno) = execve
 * returned an error, tawcroot loader codes 60..89 = the broken-case
 * signature (68 = mapped-over-self).
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	mov     (%rsp), %rdi
	cmp     $2, %rdi
	jge     .Ldone42

	movabs  $path, %rdi
	movabs  $argv, %rsi
	movabs  $envp, %rdx
	mov     $59, %rax              /* SYS_execve */
	syscall

	/* execve returned -errno. Encode as 90 + min(60, errno). */
	neg     %rax
	cmp     $60, %rax
	jbe     .Lcap_ok
	mov     $60, %rax
.Lcap_ok:
	add     $90, %rax
	mov     %rax, %rdi
	mov     $231, %rax             /* exit_group */
	syscall
	ud2

.Ldone42:
	mov     $42, %rdi
	mov     $231, %rax             /* exit_group */
	syscall
	ud2

	.section .rodata
path:
	.asciz  "/proc/self/exe"
arg1:
	.asciz  "again"
	.balign 8
argv:
	.quad   path
	.quad   arg1
	.quad   0
envp:
	.quad   0
	.section .note.GNU-stack,"",@progbits
