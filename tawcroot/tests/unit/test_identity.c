/* Unit tests for the stateful virtual identity (tawcroot/src/identity.c).
 *
 * Rule coverage runs through the real dispatch handlers (hosted, ASan);
 * the trap-level path (inline syscalls through the SIGSYS handler) is
 * covered by the rootfs smoke. The multithread test mirrors
 * test_signal_shadow.c's seqlock torn-free shape: concurrent getresuid
 * snapshots during a setresuid loop must never observe a torn triple.
 */

#include <cleat/test.h>
#include <pthread.h>
#include <stdint.h>

#include "dispatch.h"
#include "errno_neg.h"
#include "identity.h"
#include "sysnr.h"

static long ident_sys(TestCtx *test_ctx, long nr, long a, long b, long c)
{
	tawcroot_handler_fn fn = tawcroot_dispatch_get((int)nr);
	test_nonnull((void *)fn);
	tawcroot_syscall_args args = { .nr = nr, .a = a, .b = b, .c = c };
	return fn(&args, NULL);
}
#define isys(nr, a, b, c) ident_sys(test_ctx, (long)(nr), (long)(a), \
				    (long)(b), (long)(c))

test(identity_drop_is_irreversible)
{
	tawcroot_dispatch_init();
	tawcroot_identity_reset();

	test_int_eq((int)isys(TAWC_SYS_setresgid, 994, 994, 994), 0);
	test_int_eq((int)isys(TAWC_SYS_setresuid, 994, 994, 994), 0);

	test_int_eq((int)isys(TAWC_SYS_setuid, 0, 0, 0), TAWC_EPERM);
	test_int_eq((int)isys(TAWC_SYS_setresuid, -1, 0, -1), TAWC_EPERM);
	test_int_eq((int)isys(TAWC_SYS_setgid, 0, 0, 0), TAWC_EPERM);
	test_int_eq((int)isys(TAWC_SYS_setresgid, -1, 0, -1), TAWC_EPERM);

	tawc_identity id;
	tawcroot_identity_get(&id);
	test_int_eq((int)id.ruid, 994);
	test_int_eq((int)id.euid, 994);
	test_int_eq((int)id.suid, 994);
	test_int_eq((int)id.fsuid, 994);
	test_int_eq((int)id.rgid, 994);
	test_int_eq((int)id.egid, 994);
	test_int_eq((int)id.sgid, 994);

	/* setuid to an id we still hold is allowed (euid only). */
	test_int_eq((int)isys(TAWC_SYS_setuid, 994, 0, 0), 0);

	tawcroot_identity_reset();
	tawcroot_identity_get(&id);
	test_int_eq((int)id.euid, 0);
	test_int_eq((int)id.ngroups, 1);
	test_int_eq((int)id.groups[0], 0);
}

test(identity_setreuid_saved_id_rule)
{
	tawcroot_dispatch_init();
	tawcroot_identity_reset();

	test_int_eq((int)isys(TAWC_SYS_setreuid, 1000, 1000, 0), 0);
	tawc_identity id;
	tawcroot_identity_get(&id);
	test_int_eq((int)id.ruid, 1000);
	test_int_eq((int)id.euid, 1000);
	test_int_eq((int)id.suid, 1000);  /* the saved-id update rule */
	test_int_eq((int)id.fsuid, 1000);

	/* setreuid(-1, e) with e == old ruid does NOT update suid. */
	tawcroot_identity_reset();
	test_int_eq((int)isys(TAWC_SYS_setresuid, 5, 6, 7), 0);
	test_int_eq((int)isys(TAWC_SYS_setreuid, -1, 5, 0), 0);
	tawcroot_identity_get(&id);
	test_int_eq((int)id.ruid, 5);
	test_int_eq((int)id.euid, 5);
	test_int_eq((int)id.suid, 7);  /* unchanged: e == old ruid */

	tawcroot_identity_reset();
}

test(identity_setfsuid_returns_prev_and_gates)
{
	tawcroot_dispatch_init();
	tawcroot_identity_reset();

	test_int_eq((int)isys(TAWC_SYS_setfsuid, 994, 0, 0), 0);
	test_int_eq((int)isys(TAWC_SYS_setfsuid, 994, 0, 0), 994);

	test_int_eq((int)isys(TAWC_SYS_setresuid, 994, 994, 994), 0);
	/* fsuid followed euid on the drop. Unprivileged setfsuid to an
	 * unrelated id: returns prev, state unchanged. */
	test_int_eq((int)isys(TAWC_SYS_setfsuid, 4321, 0, 0), 994);
	test_int_eq((int)isys(TAWC_SYS_setfsuid, 4321, 0, 0), 994);

	tawcroot_identity_reset();
}

test(identity_setreuid_eperm_is_atomic)
{
	tawcroot_dispatch_init();
	tawcroot_identity_reset();

	test_int_eq((int)isys(TAWC_SYS_setresuid, 5, 6, 7), 0);
	/* r=6 alone would be allowed (== old euid) but e=8 is not: the
	 * call must fail atomically, leaving even the r half unapplied —
	 * the kernel commits creds only after every check passes. */
	test_int_eq((int)isys(TAWC_SYS_setreuid, 6, 8, 0), TAWC_EPERM);
	tawc_identity id;
	tawcroot_identity_get(&id);
	test_int_eq((int)id.ruid, 5);
	test_int_eq((int)id.euid, 6);
	test_int_eq((int)id.suid, 7);
	test_int_eq((int)id.fsuid, 6);

	tawcroot_identity_reset();
}

test(identity_setuid_minus_one_is_einval)
{
	tawcroot_dispatch_init();
	tawcroot_identity_reset();
	test_int_eq((int)isys(TAWC_SYS_setuid, -1, 0, 0), TAWC_EINVAL);
	test_int_eq((int)isys(TAWC_SYS_setgid, -1, 0, 0), TAWC_EINVAL);
	/* All-keep setre/setres are no-op successes. */
	test_int_eq((int)isys(TAWC_SYS_setreuid, -1, -1, 0), 0);
	test_int_eq((int)isys(TAWC_SYS_setresuid, -1, -1, -1), 0);
	tawcroot_identity_reset();
}

/* --- multithread: seqlock torn-free ------------------------------- */

#define IDENT_WRITERS     4
#define IDENT_READERS     4
#define IDENT_WRITER_ITER 20000

static volatile int g_ident_stop;

/* Writers keep euid == 0 (stays privileged, so every call succeeds)
 * and maintain ruid == suid within each call — the coherence invariant
 * readers check. Uses the real setresuid handler so the RMW claim path
 * races for real. */
static void *ident_writer(void *p)
{
	long base = (long)(uintptr_t)p;
	tawcroot_handler_fn fn = tawcroot_dispatch_get(TAWC_SYS_setresuid);
	for (long i = 0; i < IDENT_WRITER_ITER; i++) {
		long v = 1 + base * IDENT_WRITER_ITER + i;
		tawcroot_syscall_args args = {
			.nr = TAWC_SYS_setresuid,
			.a = v, .b = 0, .c = v,
		};
		(void)fn(&args, NULL);
	}
	return NULL;
}

struct ident_reader_result {
	int torn_reads;
	int total_reads;
};

static void *ident_reader(void *p)
{
	struct ident_reader_result *r = p;
	while (!g_ident_stop) {
		tawc_identity id;
		tawcroot_identity_get(&id);
		r->total_reads++;
		/* Every writer publishes ruid == suid, euid == 0, and
		 * fsuid == euid. A snapshot mixing two writers' values
		 * is a torn read. */
		if (id.ruid != id.suid || id.euid != 0 || id.fsuid != 0)
			r->torn_reads++;
	}
	return NULL;
}

test(identity_multithread_seqlock_is_torn_free)
{
	tawcroot_dispatch_init();
	tawcroot_identity_reset();
	g_ident_stop = 0;

	pthread_t wt[IDENT_WRITERS];
	pthread_t rt[IDENT_READERS];
	struct ident_reader_result rresults[IDENT_READERS];

	for (int i = 0; i < IDENT_WRITERS; i++) {
		int rc = pthread_create(&wt[i], NULL, ident_writer,
					(void *)(uintptr_t)i);
		test_int_eq(rc, 0);
	}
	for (int i = 0; i < IDENT_READERS; i++) {
		rresults[i].torn_reads = 0;
		rresults[i].total_reads = 0;
		int rc = pthread_create(&rt[i], NULL, ident_reader,
					&rresults[i]);
		test_int_eq(rc, 0);
	}

	for (int i = 0; i < IDENT_WRITERS; i++) pthread_join(wt[i], NULL);
	g_ident_stop = 1;
	for (int i = 0; i < IDENT_READERS; i++) pthread_join(rt[i], NULL);

	int total = 0, torn = 0;
	for (int i = 0; i < IDENT_READERS; i++) {
		total += rresults[i].total_reads;
		torn  += rresults[i].torn_reads;
	}
	test_int_eq(total > 0, 1);
	test_int_eq(torn, 0);

	tawcroot_identity_reset();
}
