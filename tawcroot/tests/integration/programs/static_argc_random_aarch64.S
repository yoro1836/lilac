/* Aarch64 port of static_argc_random_x86_64.S — same exit-code key.
 *
 * Aarch64 ABI:
 *   sp on entry points at argc; argv at sp+8; envp follows after the
 *   argv NULL terminator; auxv after envp NULL.
 *
 * Syscalls: x8=nr, x0..x5=args, svc #0. __NR_exit_group = 94.
 *
 * Exit codes (mirror x86_64 fixture):
 *   99   argc != 3
 *   98   argv[0][0] != '/'
 *   97   envp[0][0] != 'X'
 *   96   AT_RANDOM not found in auxv
 *   100..227  (random16[0] & 0x7f) + 100
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	/* x9 = sp (we'll use x9 to walk the initial stack) */
	mov     x9, sp

	/* argc == 3 ? */
	ldr     x10, [x9]               /* argc */
	cmp     x10, #3
	b.ne    .Lfail_argc

	/* argv[0][0] == '/' ? */
	ldr     x11, [x9, #8]           /* argv[0] pointer */
	ldrb    w12, [x11]
	cmp     w12, #0x2f              /* '/' */
	b.ne    .Lfail_argv

	/* &envp[0] = sp + 8 + 8*(argc+1) */
	add     x9, x9, #8              /* x9 = &argv[0] */
	add     x9, x9, x10, lsl #3     /* x9 += 8*argc */
	add     x9, x9, #8              /* x9 += 8 (skip argv NULL) */

	/* envp[0] non-null and starts with 'X' */
	ldr     x11, [x9]               /* envp[0] */
	cbz     x11, .Lfail_envp
	ldrb    w12, [x11]
	cmp     w12, #'X'
	b.ne    .Lfail_envp

	/* Walk envp to its NULL terminator. */
.Lenv_loop:
	ldr     x11, [x9]
	cbz     x11, .Lenv_done
	add     x9, x9, #8
	b       .Lenv_loop
.Lenv_done:
	add     x9, x9, #8              /* skip envp NULL slot → &auxv[0] */

	/* Walk auxv looking for AT_RANDOM (a_type == 25). */
.Laux_loop:
	ldr     x10, [x9]               /* a_type */
	cbz     x10, .Lfail_no_random   /* AT_NULL terminator */
	cmp     x10, #25                /* AT_RANDOM */
	b.eq    .Laux_random
	add     x9, x9, #16
	b       .Laux_loop

.Laux_random:
	ldr     x10, [x9, #8]           /* a_val = pointer to 16 random bytes */
	ldrb    w11, [x10]              /* first random byte */
	and     x0, x11, #0x7f
	add     x0, x0, #100            /* exit code = byte mod 128 + 100 */
	mov     x8, #94                 /* __NR_exit_group */
	svc     #0
	brk     #0

.Lfail_argc:
	mov     x0, #99
	b       .Lexit
.Lfail_argv:
	mov     x0, #98
	b       .Lexit
.Lfail_envp:
	mov     x0, #97
	b       .Lexit
.Lfail_no_random:
	mov     x0, #96
.Lexit:
	mov     x8, #94
	svc     #0
	brk     #0
	.size _start, .-_start

	.section .note.GNU-stack,"",%progbits
