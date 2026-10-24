/* Static x86_64 fixture: open /proc/self/fd, enumerate it with the
 * LEGACY getdents(2) (NR 78, not getdents64/217), and check that
 *   (a) the buffer is legacy linux_dirent layout — d_name starts at
 *       record offset 18, so the first record (procfs always emits
 *       "." first) has '.' at buf[18]; under raw/64 layout that byte
 *       is d_type (DT_DIR = 4), and
 *   (b) no d_name byte sequence "1000\0" appears (the reserved-fd
 *       filter applied to the legacy NR too).
 *
 * Companion to static_check_proc_self_fd (the getdents64 variant);
 * guards the NR-78 trap + in-place repack
 * (syscalls_fd.c::handle_getdents +
 * dirent_filter.c::tawcroot_dirent_filter_repack_legacy). Before that
 * handler existed, NR 78 was not in tawcroot's trap set and Android's
 * filter allows it, so legacy callers got raw kernel dirents with
 * reserved fds visible.
 *
 * x86_64-only: aarch64 never allocated a legacy getdents (NR 78 is
 * readlinkat there). The aarch64 twin exits 96 unconditionally.
 *
 * Loaded by tawcroot's manual loader; -static -nostdlib.
 *
 * Exit code key:
 *    42 — legacy layout confirmed and no "1000\0" (handler worked)
 *    91 — openat /proc/self/fd failed
 *    92 — getdents returned a negative errno (e.g. -ENOSYS: trapped
 *         but no handler registered)
 *    93 — getdents returned 0 (empty — unexpected)
 *    94 — buf[18] != '.' (record not repacked to legacy layout)
 *    95 — found "1000\0" in the dirent stream (filter regressed)
 */

	.text
	.globl _start
	.type  _start, @function
_start:
	/* openat(AT_FDCWD, "/proc/self/fd", O_RDONLY|O_DIRECTORY) */
	mov     $-100, %rdi              /* AT_FDCWD                      */
	movabs  $procfd_path, %rsi
	mov     $(0 | 0200000), %rdx     /* O_RDONLY | O_DIRECTORY        */
	xor     %r10, %r10
	mov     $257, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_open
	mov     %rax, %r12               /* dirfd */

	/* getdents(dirfd, buf, sizeof buf) — LEGACY NR 78. */
	mov     %r12, %rdi
	movabs  $dent_buf, %rsi
	mov     $4096, %rdx
	mov     $78, %rax
	syscall
	test    %rax, %rax
	js      .Lfail_dents
	jz      .Lfail_empty

	/* Legacy layout probe: first record's d_name starts at offset 18
	 * and procfs emits "." first, so buf[18] must be '.'. */
	movabs  $dent_buf, %rsi
	cmpb    $'.', 18(%rsi)
	jne     .Lfail_layout

	/* Scan the buffer for the byte sequence "1000\0". buf at rsi,
	 * length in rax. Loop i = 0..rax-5: if buf[i..i+5] == "1000\0",
	 * fail. */
	mov     %rax, %rcx               /* total bytes */
	xor     %rdi, %rdi               /* i = 0 */
.Lscan:
	mov     %rdi, %rdx
	add     $5, %rdx
	cmp     %rcx, %rdx
	ja      .Lscan_done              /* i + 5 > total → no match */
	cmpb    $'1', (%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $'0', 1(%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $'0', 2(%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $'0', 3(%rsi, %rdi)
	jne     .Lscan_next
	cmpb    $0, 4(%rsi, %rdi)
	jne     .Lscan_next
	/* Match: filter regressed. */
	mov     $95, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lscan_next:
	inc     %rdi
	jmp     .Lscan
.Lscan_done:

	mov     $42, %rdi
	mov     $231, %rax
	syscall
	ud2

.Lfail_open:
	mov     $91, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lfail_dents:
	mov     $92, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lfail_empty:
	mov     $93, %rdi
	mov     $231, %rax
	syscall
	ud2
.Lfail_layout:
	mov     $94, %rdi
	mov     $231, %rax
	syscall
	ud2

	.size _start, .-_start

	.section .rodata
procfd_path:
	.asciz  "/proc/self/fd"

	.bss
dent_buf:
	.zero   4096

	.section .note.GNU-stack,"",@progbits
