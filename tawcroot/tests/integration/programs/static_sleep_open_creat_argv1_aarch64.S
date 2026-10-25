/* Aarch64 port of static_sleep_open_creat_argv1_x86_64.S.
 * See the x86_64 file for the rationale.
 *
 * Aarch64 syscall numbers:
 *   __NR_openat     = 56
 *   __NR_nanosleep  = 101
 *   __NR_exit_group = 94
 *   AT_FDCWD        = -100
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	/* argc >= 2 ? */
	ldr     x0, [sp]
	cmp     x0, #2
	b.lt    .Lfail_argc

	/* nanosleep({1, 0}, NULL). 16-byte timespec on stack. We adjust
	 * sp by 16 so argv[1] is at sp+32 instead of sp+16 during the
	 * open, then restore sp. */
	sub     sp, sp, #16
	mov     x9, #1
	str     x9, [sp]                /* tv_sec = 1                      */
	str     xzr, [sp, #8]           /* tv_nsec = 0                     */
	mov     x0, sp                  /* req                             */
	mov     x1, #0                  /* rem = NULL                      */
	mov     x8, #101                /* __NR_nanosleep                  */
	svc     #0
	add     sp, sp, #16

	/* openat(AT_FDCWD, argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600) */
	mov     x0, #-100               /* AT_FDCWD                        */
	ldr     x1, [sp, #16]           /* argv[1]                         */
	mov     x2, #(1 | 0100 | 01000)
	mov     x3, #0x180              /* mode 0600                       */
	mov     x8, #56                 /* __NR_openat                     */
	svc     #0
	cmp     x0, #0
	b.lt    .Lfail_open

	mov     x0, #0                  /* exit_group(0)                   */
	mov     x8, #94
	svc     #0
	brk     #0

.Lfail_argc:
	mov     x0, #210
	b       .Lexit
.Lfail_open:
	mov     x0, #211
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0
	.size _start, .-_start
