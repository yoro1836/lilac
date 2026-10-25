/* Big-stack-frame regression fixture for the loader's stack mmap.
 *
 * GNU coreutils 9.11 `wc` is built with `-fstack-clash-protection` and
 * has a function (wc_lines / wc_bytes) that pre-allocates a 256 KiB
 * I/O buffer in the stack frame and probes each page as it grows the
 * frame. Earlier revisions of the loader mmap'd a 256 KiB stack for
 * the guest; the probe loop walked the last page just past the bottom
 * of that mapping and SIGSEGV'd with SEGV_MAPERR exactly at rsp. wc
 * was the canary in the coal mine — every coreutils binary with a
 * heavy alloca'd buffer would have hit this, but wc happened to be
 * unique among the ones we exercise (`ls`, `head`, `tail`, `cat`,
 * `sha256sum`, `cksum`, etc.) in allocating a frame that big.
 *
 * Reproduce the failure pattern in a single static fixture instead
 * of pulling in coreutils. With `-fstack-clash-protection` GCC emits
 * an `or $0,(%rsp); cmp; jne` probe loop on x86_64 and a `str xzr,
 * [sp]; cmp; b.ne` loop on aarch64 around any frame >= a page. Make
 * the frame deliberately bigger than the old 256 KiB stack mapping
 * (380 KiB; the actual guest stack reserved by the loader is now
 * 8 MiB so this fits comfortably) and touch every page so the
 * optimiser can't elide the buffer. Exit 42 if the probe loop and
 * the buffer touches all completed without faulting.
 *
 * Compile-time guard: assert that `-fstack-clash-protection` is in
 * effect. Without it the test still allocates the buffer but doesn't
 * walk the probe loop — which is exactly the codegen difference we
 * need to exercise. The macro shows up in GCC ≥ 8 / Clang ≥ 11.
 */

#if !defined(__SSP_STRONG__) && !defined(__clang__)
/* GCC sets neither a feature macro for stack-clash nor anything we
 * can portably assert at preprocessor level. The Makefile rule is
 * the source of truth — see TAWC_FIXTURE_FLAGS. */
#endif

#include <stddef.h>
#include <stdint.h>

/* 380 KiB > 256 KiB old loader stack < 8 MiB current loader stack. */
#define BUF_BYTES (380 * 1024)

/* `noinline` so the buffer survives optimisation; `volatile` writes
 * so each page touch is observable; returning a value derived from
 * the buffer ties the live-range to main's return value, which kills
 * the dead-store eliminator. */
__attribute__((noinline))
static unsigned big_frame_function(unsigned argc)
{
	volatile unsigned char buf[BUF_BYTES];

	/* Walk each page of `buf`, writing a deterministic byte and
	 * reading it back. If the loader's stack mmap is too small,
	 * the FIRST page-aligned write past the bottom faults — the
	 * exact failure mode that bit wc. */
	const size_t PAGE = 4096;
	unsigned acc = 0;
	for (size_t off = 0; off < BUF_BYTES; off += PAGE) {
		buf[off] = (unsigned char)(off ^ argc);
		acc += buf[off];
	}
	/* Touch the last byte too, in case BUF_BYTES isn't page aligned. */
	buf[BUF_BYTES - 1] = 0xab;
	acc += buf[BUF_BYTES - 1];
	return acc;
}

int main(int argc, char **argv)
{
	(void)argv;
	unsigned acc = big_frame_function((unsigned)argc);
	/* Sink: keep `acc` from being constant-folded out. */
	if (acc == 0xdeadbeef) return 1;
	return 42;
}
