/* Emulated binder: binderd on the host side, guests under production
 * tawcroot with `-b <binderd dir>:/dev/binderfs`. The guest fixture
 * (static_binder_check) drives raw binder ioctls between two processes;
 * see its header for what it covers. */

#include <cleat/test.h>
#include <cleat/subproc.h>
#include <stc/cstr.h>

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rootfs_helpers.h"

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

#ifndef TAWCROOT_TEST_TMPDIR
# define TAWCROOT_TEST_TMPDIR "/tmp"
#endif

#define BINDER_ROOTFS TAWCROOT_TEST_TMPDIR "/tawcroot-test-rootfs-binder"
#define BINDER_DIR    TAWCROOT_TEST_TMPDIR "/tawcroot-test-binderfs"

static bool build_binder_rootfs(void)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/bin", BINDER_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;
	snprintf(p, sizeof p, "%s/dev/binderfs", BINDER_ROOTFS);
	if (!rh_mkdir_p(p, 0755)) return false;
	snprintf(p, sizeof p, "%s/bin/static_binder_check", BINDER_ROOTFS);
	return rh_copy_file(TAWCROOT_STATIC_BINDER_CHECK_BIN, p, 0755);
}

static pid_t start_binderd(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		execl(TAWCROOT_BINDERD_BIN, "binderd", BINDER_DIR, (char *)NULL);
		_exit(127);
	}
	return pid;
}

test(prod_binder_transactions)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only: Android shell SELinux denies"
	       " filesystem AF_UNIX socket creation under the test dir)\n");
	return;
#else
	rh_rmrf(BINDER_ROOTFS);
	rh_rmrf(BINDER_DIR);
	test_true(build_binder_rootfs());
	test_true(rh_mkdir_p(BINDER_DIR, 0755));

	pid_t binderd = start_binderd();
	test_true(binderd > 0);
	test_true(rh_poll_for_path(BINDER_DIR "/ready", 100));

	VecStr cmd = c_init(vec_str, {
		TAWCROOT_PROD_BIN, "-r", BINDER_ROOTFS,
		"-b", BINDER_DIR ":/dev/binderfs",
		"--", "/bin/static_binder_check",
	});
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){ .vec_cmd = cmd, .exit_code = &rc });
	failable_result_drop(&res);

	/* binderd must survive the guests' deaths and exit cleanly (its
	 * sanitizers abort on any memory error along the way). */
	int alive = waitpid(binderd, 0, WNOHANG) == 0;
	kill(binderd, SIGTERM);
	int status = 0;
	waitpid(binderd, &status, 0);

	test_int_eq(rc, 42);
	test_true(alive);
	test_true(WIFEXITED(status));
	test_int_eq(WEXITSTATUS(status), 0);

	rh_rmrf(BINDER_ROOTFS);
	rh_rmrf(BINDER_DIR);
#endif
}

/* The manager dies holding the child's call and with a nested call into
 * the child outstanding: the child's call must end in BR_DEAD_REPLY,
 * and binderd must unwind both stacks without leaking or crashing. */
test(prod_binder_dead_reply_through_nested_call)
{
#if defined(__ANDROID__)
	printf("    skipping (host-only, see prod_binder_transactions)\n");
	return;
#else
	rh_rmrf(BINDER_ROOTFS);
	rh_rmrf(BINDER_DIR);
	test_true(build_binder_rootfs());
	test_true(rh_mkdir_p(BINDER_DIR, 0755));
	char marker[PATH_MAX];
	snprintf(marker, sizeof marker, "%s/dead-ok", BINDER_ROOTFS);

	pid_t binderd = start_binderd();
	test_true(binderd > 0);
	test_true(rh_poll_for_path(BINDER_DIR "/ready", 100));

	VecStr cmd = c_init(vec_str, {
		TAWCROOT_PROD_BIN, "-r", BINDER_ROOTFS,
		"-b", BINDER_DIR ":/dev/binderfs",
		"--", "/bin/static_binder_check", "dead", "/dead-ok",
	});
	int rc = -1;
	FailableResult res = run_subproc((SubprocArgs){ .vec_cmd = cmd, .exit_code = &rc });
	failable_result_drop(&res);
	bool ok = rh_poll_for_path(marker, 100);

	int alive = waitpid(binderd, 0, WNOHANG) == 0;
	kill(binderd, SIGTERM);
	int status = 0;
	waitpid(binderd, &status, 0);

	test_int_eq(rc, 42);
	test_true(ok);
	test_true(alive);
	test_true(WIFEXITED(status));
	test_int_eq(WEXITSTATUS(status), 0);

	rh_rmrf(BINDER_ROOTFS);
	rh_rmrf(BINDER_DIR);
#endif
}
