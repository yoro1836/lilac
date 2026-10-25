/* `--owners`: persistent file ownership under production tawcroot. The
 * rootfs gets its own parent dir so the store (<rootfs>/../tawcroot) and
 * its owners table are this test's alone. */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

#include "rootfs_helpers.h"

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define OWNERS_BASE   TAWCROOT_TEST_TMPDIR "/tawcroot-test-owners"
#define OWNERS_ROOTFS OWNERS_BASE "/rootfs"

static int run_owner_check(const char *mode)
{
	VecStr cmd = c_init(vec_str, {
		TAWCROOT_PROD_BIN, "-r", OWNERS_ROOTFS, "--owners", "--",
		"/bin/static_owner_check", mode, "/work",
	});
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){ .vec_cmd = cmd, .exit_code = &rc });
	failable_result_drop(&res);
	return rc;
}

test(prod_owners_persist)
{
	rh_rmrf(OWNERS_BASE);
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/bin", OWNERS_ROOTFS);
	test_true(rh_mkdir_p(p, 0755));
	snprintf(p, sizeof p, "%s/bin/static_owner_check", OWNERS_ROOTFS);
	test_true(rh_copy_file(TAWCROOT_STATIC_OWNER_CHECK_BIN, p, 0755));

	test_int_eq(run_owner_check("setup"), 42);
	/* A fresh tawcroot reads the same table. */
	test_int_eq(run_owner_check("check"), 42);

	/* The table lives in the store, outside the guest tree. */
	struct stat st;
	test_int_eq(stat(OWNERS_BASE "/tawcroot/owners", &st), 0);
	rh_rmrf(OWNERS_BASE);
}
