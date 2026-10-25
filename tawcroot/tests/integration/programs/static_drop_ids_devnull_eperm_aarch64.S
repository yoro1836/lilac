/* Aarch64 port of static_drop_ids_devnull_eperm_x86_64.S — see that
 * file's header for the rationale and the exit-code key (42 pass,
 * 220..226 per-step failures).
 *
 * Syscall numbers (asm-generic):
 *   fchmodat=53 fchownat=54 fchown=55 openat=56
 *   setresuid=147 setresgid=149 exit_group=94
 * AT_FDCWD = -100. Aarch64 ABI: x0..x5 args, x8 nr, svc #0.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	/* fchmodat(AT_FDCWD, "/dev/null", 0666) at virtual root -> 0 */
	mov     x0, #-100
	ldr     x1, =path
	mov     x2, #0x1b6              /* mode 0666                     */
	mov     x8, #53                 /* __NR_fchmodat                 */
	svc     #0
	cbnz    x0, .Lfail220

	/* setresgid(994, 994, 994) -> 0 */
	mov     x0, #994
	mov     x1, #994
	mov     x2, #994
	mov     x8, #149                /* __NR_setresgid                */
	svc     #0
	cbnz    x0, .Lfail221

	/* setresuid(994, 994, 994) -> 0 */
	mov     x0, #994
	mov     x1, #994
	mov     x2, #994
	mov     x8, #147                /* __NR_setresuid                */
	svc     #0
	cbnz    x0, .Lfail222

	/* dropped fchmodat -> real -EPERM / -EACCES */
	mov     x0, #-100
	ldr     x1, =path
	mov     x2, #0x1b6
	mov     x8, #53                 /* __NR_fchmodat                 */
	svc     #0
	cmn     x0, #1                  /* -EPERM                        */
	b.eq    1f
	cmn     x0, #13                 /* -EACCES                       */
	b.ne    .Lfail223
1:
	/* dropped fchownat(/dev/null, 0, 0, 0) -> real -EPERM / -EACCES */
	mov     x0, #-100
	ldr     x1, =path
	mov     x2, #0
	mov     x3, #0
	mov     x4, #0
	mov     x8, #54                 /* __NR_fchownat                 */
	svc     #0
	cmn     x0, #1
	b.eq    2f
	cmn     x0, #13
	b.ne    .Lfail224
2:
	/* openat(AT_FDCWD, "/dev/null", O_RDONLY) */
	mov     x0, #-100
	ldr     x1, =path
	mov     x2, #0
	mov     x8, #56                 /* __NR_openat                   */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail225
	mov     x19, x0

	/* dropped fchown(fd, 0, 0) -> real -EPERM / -EACCES */
	mov     x0, x19
	mov     x1, #0
	mov     x2, #0
	mov     x8, #55                 /* __NR_fchown                   */
	svc     #0
	cmn     x0, #1
	b.eq    3f
	cmn     x0, #13
	b.ne    .Lfail226
3:
	mov     x0, #42
	b       .Lexit

.Lfail220:
	mov     x0, #220
	b       .Lexit
.Lfail221:
	mov     x0, #221
	b       .Lexit
.Lfail222:
	mov     x0, #222
	b       .Lexit
.Lfail223:
	mov     x0, #223
	b       .Lexit
.Lfail224:
	mov     x0, #224
	b       .Lexit
.Lfail225:
	mov     x0, #225
	b       .Lexit
.Lfail226:
	mov     x0, #226
.Lexit:
	mov     x8, #94                 /* __NR_exit_group               */
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .rodata
path:
	.asciz  "/dev/null"

	.section .note.GNU-stack,"",%progbits
