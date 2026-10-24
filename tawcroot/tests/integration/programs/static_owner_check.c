/* static_owner_check: `--owners` persistent file ownership.
 *
 *   static_owner_check setup DIR   root: chown/fchown/lchown, every stat
 *       flavour agrees; a child dropped to uid 10057 creates files that
 *       are its own and gets the kernel's chown rules; rename over a
 *       file drops the victim's owner; then re-execs itself as `check`.
 *   static_owner_check check DIR   the owners survive exec and new
 *       tawcroot runs.
 *
 * Exit 42 on success, otherwise the failing step.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FAIL(n) do { fprintf(stderr, "static_owner_check: step %d (line %d) errno=%d\n", n, __LINE__, errno); _exit(n); } while (0)

static char path_buf[4][512];

static const char *at(int slot, const char *dir, const char *name)
{
	snprintf(path_buf[slot], sizeof path_buf[slot], "%s/%s", dir, name);
	return path_buf[slot];
}

/* stat, lstat-with-follow-off, statx and fstat must all report uid:gid. */
static void expect(const char *path, int follow, unsigned uid, unsigned gid, int step)
{
	struct stat st;
	if ((follow ? stat(path, &st) : lstat(path, &st)) || st.st_uid != uid || st.st_gid != gid) {
		fprintf(stderr, "static_owner_check: %s is %u:%u, want %u:%u\n", path, st.st_uid, st.st_gid, uid, gid);
		FAIL(step);
	}
	struct statx sx;
	/* raw syscall: bionic declares statx only from API 30 */
	if (syscall(__NR_statx, AT_FDCWD, path, follow ? 0 : AT_SYMLINK_NOFOLLOW, STATX_UID | STATX_GID, &sx) ||
	    sx.stx_uid != uid || sx.stx_gid != gid)
		FAIL(step);
	if (follow) {
		int fd = open(path, O_PATH | O_CLOEXEC);
		struct stat fst;
		if (fd < 0 || fstat(fd, &fst) || fst.st_uid != uid || fst.st_gid != gid) FAIL(step);
		close(fd);
	}
}

static void touch(const char *path, int step)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0) FAIL(step);
	close(fd);
}

static int child(const char *dir)
{
	gid_t g = 10057;
	if (setgroups(1, &g) || setresgid(10057, 10057, 10057) || setresuid(10057, 10057, 10057)) FAIL(20);
	touch(at(0, dir, "d1/c1"), 21);
	expect(at(0, dir, "d1/c1"), 1, 10057, 10057, 22);
	if (mkdir(at(0, dir, "d1/cd"), 0755)) FAIL(23);
	expect(at(0, dir, "d1/cd"), 1, 10057, 10057, 24);
	/* Without CAP_CHOWN: never another owner, only our own groups. */
	errno = 0;
	if (chown(at(0, dir, "f1"), 10057, -1) == 0 || errno != EPERM) FAIL(25);
	if (chown(at(0, dir, "d1/c1"), 0, -1) == 0 || errno != EPERM) FAIL(26);
	if (chown(at(0, dir, "d1/c1"), -1, 1) == 0 || errno != EPERM) FAIL(27);
	if (chown(at(0, dir, "d1/c1"), -1, 10057)) FAIL(28);
	if (chown(at(0, dir, "d1/c1"), 10057, 10057)) FAIL(29);
	return 0;
}

static int check(const char *dir)
{
	expect(at(1, dir, "f1"), 1, 3000, 2000, 40);
	expect(at(1, dir, "d1"), 1, 10057, 10057, 41);
	expect(at(1, dir, "d1/c1"), 1, 0, 0, 43);
	return 42;
}

int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	const char *dir = argv[2];
	if (!strcmp(argv[1], "check")) return check(dir);
	if (strcmp(argv[1], "setup")) return 2;

	if (mkdir(dir, 0755) && errno != EEXIST) FAIL(1);
	expect(dir, 1, 0, 0, 2);
	const char *f1 = at(1, dir, "f1");
	touch(f1, 3);
	expect(f1, 1, 0, 0, 4);
	if (chown(f1, 1000, 1001)) FAIL(5);
	expect(f1, 1, 1000, 1001, 6);
	if (chown(f1, -1, 2000)) FAIL(7);
	expect(f1, 1, 1000, 2000, 8);
	int fd = open(f1, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fchown(fd, 3000, -1)) FAIL(9);
	close(fd);
	expect(f1, 1, 3000, 2000, 10);

	const char *d1 = at(2, dir, "d1");
	if (mkdir(d1, 0777) || chown(d1, 10057, 10057)) FAIL(11);
	expect(d1, 1, 10057, 10057, 12);

	const char *l1 = at(3, dir, "l1");
	if (symlink("f1", l1) || lchown(l1, 1234, 1234)) FAIL(13);
	expect(l1, 0, 1234, 1234, 14);
	expect(l1, 1, 3000, 2000, 15);

	pid_t pid = fork();
	if (pid < 0) FAIL(16);
	if (pid == 0) _exit(child(dir));
	int status;
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "static_owner_check: child exited %d\n", WEXITSTATUS(status));
		FAIL(17);
	}
	/* Visible across processes. */
	expect(at(0, dir, "d1/c1"), 1, 10057, 10057, 30);
	if (rmdir(at(0, dir, "d1/cd"))) FAIL(31);

	/* Renaming a root file over the child's: the victim's owner goes,
	 * the moved inode keeps its own (root). */
	const char *f2 = at(0, dir, "f2");
	touch(f2, 32);
	if (rename(f2, at(3, dir, "d1/c1"))) FAIL(33);
	expect(at(3, dir, "d1/c1"), 1, 0, 0, 34);

	/* Across exec: the session keeps --owners. */
	execl("/proc/self/exe", argv[0], "check", dir, (char *)NULL);
	FAIL(35);
}
