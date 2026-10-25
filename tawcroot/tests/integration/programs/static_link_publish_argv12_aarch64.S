/* Aarch64 port of static_link_publish_argv12_x86_64.S — see that
 * file's header for the full rationale and the exit-code key
 * (42 pass, 209..219 per-step failures).
 *
 * Syscall numbers (asm-generic):
 *   openat=56 close=57 read=63 write=64 linkat=37 unlinkat=35
 *   exit_group=94
 * AT_FDCWD = -100. Aarch64 ABI: x0..x5 args, x8 nr, svc #0.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	ldr     x0, [sp]                /* argc                          */
	cmp     x0, #3
	b.lt    .Lfail209
	ldr     x19, [sp, #16]          /* src = argv[1]                 */
	ldr     x20, [sp, #24]          /* dst = argv[2]                 */

	/* openat(AT_FDCWD, src, O_WRONLY|O_CREAT|O_TRUNC, 0644) */
	mov     x0, #-100
	mov     x1, x19
	mov     x2, #(1 | 0100 | 01000)
	mov     x3, #0x1a4              /* mode 0644                     */
	mov     x8, #56                 /* __NR_openat                   */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail210
	mov     x21, x0

	/* write(fd, "publish-me", 10) == 10 */
	mov     x0, x21
	ldr     x1, =msg
	mov     x2, #10
	mov     x8, #64                 /* __NR_write                    */
	svc     #0
	cmp     x0, #10
	b.ne    .Lfail211

	mov     x0, x21
	mov     x8, #57                 /* __NR_close                    */
	svc     #0

	/* linkat(AT_FDCWD, src, AT_FDCWD, dst, 0) -> 0 */
	mov     x0, #-100
	mov     x1, x19
	mov     x2, #-100
	mov     x3, x20
	mov     x4, #0
	mov     x8, #37                 /* __NR_linkat                   */
	svc     #0
	cbnz    x0, .Lfail212

	/* linkat onto the existing dst -> -EEXIST */
	mov     x0, #-100
	mov     x1, x19
	mov     x2, #-100
	mov     x3, x20
	mov     x4, #0
	mov     x8, #37                 /* __NR_linkat                   */
	svc     #0
	cmn     x0, #17                 /* -EEXIST                       */
	b.ne    .Lfail213

	/* src still opens and holds the content */
	mov     x0, x19
	bl      check_content
	cbnz    x0, .Lfail_src_check

	/* unlinkat(AT_FDCWD, src, 0) -> 0 */
	mov     x0, #-100
	mov     x1, x19
	mov     x2, #0
	mov     x8, #35                 /* __NR_unlinkat                 */
	svc     #0
	cbnz    x0, .Lfail216

	/* dst still opens and holds the content */
	mov     x0, x20
	bl      check_content
	cbnz    x0, .Lfail_dst_check

	/* unlinkat(AT_FDCWD, dst, 0) -> 0 */
	mov     x0, #-100
	mov     x1, x20
	mov     x2, #0
	mov     x8, #35                 /* __NR_unlinkat                 */
	svc     #0
	cbnz    x0, .Lfail219

	mov     x0, #42
	b       .Lexit

/* check_content(path in x0): open O_RDONLY, read 10 bytes, compare
 * against msg. Returns x0 = 0 ok, 1 open failed, 2 content mismatch.
 * Clobbers x1..x8, x22..x24 (and lr is preserved in x23). */
check_content:
	mov     x23, x30
	mov     x1, x0
	mov     x0, #-100
	mov     x2, #0                  /* O_RDONLY                      */
	mov     x8, #56                 /* __NR_openat                   */
	svc     #0
	cmp     x0, #0
	b.lt    .Lcc_open_fail
	mov     x22, x0

	mov     x0, x22
	ldr     x1, =buf
	mov     x2, #10
	mov     x8, #63                 /* __NR_read                     */
	svc     #0
	mov     x24, x0                 /* byte count                    */
	mov     x0, x22
	mov     x8, #57                 /* __NR_close                    */
	svc     #0
	cmp     x24, #10
	b.ne    .Lcc_mismatch

	mov     x4, #0
	ldr     x5, =buf
	ldr     x6, =msg
.Lcc_loop:
	ldrb    w1, [x5, x4]
	ldrb    w2, [x6, x4]
	cmp     w1, w2
	b.ne    .Lcc_mismatch
	add     x4, x4, #1
	cmp     x4, #10
	b.lt    .Lcc_loop
	mov     x0, #0
	ret     x23
.Lcc_open_fail:
	mov     x0, #1
	ret     x23
.Lcc_mismatch:
	mov     x0, #2
	ret     x23

.Lfail_src_check:
	cmp     x0, #1
	b.eq    .Lfail214
	mov     x0, #215
	b       .Lexit
.Lfail_dst_check:
	cmp     x0, #1
	b.eq    .Lfail217
	mov     x0, #218
	b       .Lexit

.Lfail209:
	mov     x0, #209
	b       .Lexit
.Lfail210:
	mov     x0, #210
	b       .Lexit
.Lfail211:
	mov     x0, #211
	b       .Lexit
.Lfail212:
	mov     x0, #212
	b       .Lexit
.Lfail213:
	mov     x0, #213
	b       .Lexit
.Lfail214:
	mov     x0, #214
	b       .Lexit
.Lfail216:
	mov     x0, #216
	b       .Lexit
.Lfail217:
	mov     x0, #217
	b       .Lexit
.Lfail219:
	mov     x0, #219
.Lexit:
	mov     x8, #94                 /* __NR_exit_group               */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .rodata
msg:
	.ascii  "publish-me"

	.bss
	.align  3
buf:
	.skip   16

	.section .note.GNU-stack,"",%progbits
