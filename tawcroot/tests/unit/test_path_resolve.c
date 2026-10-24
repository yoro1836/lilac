/* Unit tests for the manual symlink resolver
 * (tawcroot/src/path_resolve.c).
 *
 * The resolver is pure: it operates on a path buffer + a filesystem
 * oracle, with no syscalls of its own. So we link path_resolve.c +
 * path_fold.c into the cleat orchestrator (under hosted glibc) and
 * supply a mock oracle backed by an in-memory table of (path, target)
 * entries. This lets us cover the edge cases that are awkward to set
 * up on a real filesystem: depth-bombs, mid-path errors,
 * pathologically long targets, etc.
 *
 * The integration tests in tests/handler/test_rootfs_syscalls_smoke.c plus
 * tests/testhost/src/rootfs_smoke.c cover the same resolver end-to-end
 * against a real fake rootfs (this confirms the production oracle in
 * path.c, which these unit tests don't exercise).
 *
 * Structure:
 *   - mock_fs: array of "path -> target" mappings; default-not-symlink.
 *   - mock_readlink: oracle implementation; -EINVAL when the path isn't
 *     in the table, returns the target otherwise.
 *   - tests assert post-resolve buffer contents and return value.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <string.h>

#include "path.h"
#include "path_oracle.h"
#include "path_resolve.h"

struct mock_link {
	const char *path;       /* exact-match key, no leading '/' */
	const char *target;
};

struct mock_fs {
	const struct mock_link *links;
	size_t                  n_links;
};

static long mock_readlink(void *ctx, const char *suffix,
			  char *out, size_t out_cap)
{
	struct mock_fs *fs = ctx;
	if (suffix[0] == 0) return -22;  /* root, never a symlink */
	for (size_t i = 0; i < fs->n_links; i++) {
		if (strcmp(fs->links[i].path, suffix) == 0) {
			size_t tl = strlen(fs->links[i].target);
			if (tl > out_cap) return -36;  /* ENAMETOOLONG */
			memcpy(out, fs->links[i].target, tl);
			return (long)tl;
		}
	}
	return -22;  /* not in table => treat as non-symlink */
}

static struct tawcroot_path_oracle make_oracle(struct mock_fs *fs)
{
	return (struct tawcroot_path_oracle){ .ctx = fs, .readlink = mock_readlink };
}

/* ---- relative target ---- */

test(resolver_relative_target_basic)
{
	const struct mock_link links[] = {
		{ "lib", "usr/lib" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "lib/x86_64");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "usr/lib/x86_64");
}

/* ---- absolute target gets re-rooted ---- */

test(resolver_absolute_target_clamps_in_rootfs)
{
	const struct mock_link links[] = {
		{ "etc/host-secret", "/etc/passwd" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "etc/host-secret");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	/* Absolute /etc/passwd re-rooted at rootfs => "etc/passwd" */
	test_str_eq(suf, "etc/passwd");
}

/* ---- chain ---- */

test(resolver_three_hop_chain_follows)
{
	const struct mock_link links[] = {
		{ "chain1", "chain2" },
		{ "chain2", "chain3" },
		{ "chain3", "etc/probe" },
	};
	struct mock_fs fs = { links, 3 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "chain1");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "etc/probe");
}

/* ---- self-loop ---- */

test(resolver_self_loop_returns_ELOOP)
{
	const struct mock_link links[] = {
		{ "loop", "loop" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "loop");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, -40);  /* -ELOOP */
}

/* ---- chain bomb beyond SYMLOOP_MAX ---- */

test(resolver_chain_longer_than_SYMLOOP_MAX_returns_ELOOP)
{
	/* Build a chain of 50 links: a0 -> a1 -> ... -> a49.
	 * SYMLOOP_MAX is 40; the resolver must terminate. */
	struct mock_link links[50];
	char names[50][8];
	for (int i = 0; i < 50; i++) {
		snprintf(names[i], sizeof names[i], "a%d", i);
	}
	for (int i = 0; i < 49; i++) {
		links[i].path   = names[i];
		links[i].target = names[i + 1];
	}
	links[49].path   = names[49];
	links[49].target = names[49];   /* dead end */
	struct mock_fs fs = { links, 50 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "a0");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, -40);  /* -ELOOP */
}

/* ---- NOFOLLOW preserves leaf ---- */

test(resolver_NOFOLLOW_does_not_follow_leaf)
{
	const struct mock_link links[] = {
		{ "lib",       "usr/lib" },
		{ "lib/leaf",  "../somewhere" },  /* leaf-as-symlink */
	};
	struct mock_fs fs = { links, 2 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "lib/leaf");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_NOFOLLOW, &ora);
	test_int_eq(rv, 0);
	/* Parent "lib" gets followed, leaf "leaf" preserved verbatim. */
	test_str_eq(suf, "usr/lib/leaf");
}

test(resolver_FOLLOW_does_follow_leaf)
{
	const struct mock_link links[] = {
		{ "leafsym", "target" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "leafsym");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "target");
}

/* ---- PARENT_CREATE / PARENT_REMOVE preserve leaf, follow parents ---- */

test(resolver_PARENT_CREATE_preserves_leaf)
{
	const struct mock_link links[] = {
		{ "lib", "usr/lib" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "lib/newfile");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_PARENT_CREATE, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "usr/lib/newfile");
}

/* ---- root: nothing to do ---- */

test(resolver_empty_suffix_is_noop)
{
	struct mock_fs fs = { NULL, 0 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "");
}

/* ---- absolute target with `..` clamps at rootfs ---- */

test(resolver_absolute_target_with_dotdot_escapes_clamp_at_root)
{
	const struct mock_link links[] = {
		{ "etc/sneaky", "/../../host-secret" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "etc/sneaky");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	/* The `..`s eat past the root and clamp; result is just
	 * "host-secret" inside the rootfs. */
	test_str_eq(suf, "host-secret");
}

/* ---- zero-length symlink target (review finding B9) ----
 *
 * Linux refuses to create empty symlinks, but a hostile or corrupted
 * rootfs could expose one (and a `readlinkat` against a regular file
 * legitimately returns `n == 0` if the kernel zero-truncates instead
 * of -EINVAL). Treat n==0 as -ENOENT rather than silently splicing
 * "nothing" into the path -- otherwise the leaf component vanishes
 * and we'd open the parent dir or the next sibling. */

test(resolver_zero_length_target_treated_as_enoent)
{
	const struct mock_link links[] = {
		{ "etc/empty", "" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "etc/empty");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	/* Pre-fix: rv == 0 with suf rewritten to "etc" (the leaf "empty"
	 * gets spliced away). Post-fix: -ENOENT, suf untouched. */
	test_int_eq(rv, -2);
	test_str_eq(suf, "etc/empty");
}

/* ---- non-ENOENT/EINVAL errno bubbles up ----
 *
 * The oracle's contract: -EINVAL means "not a symlink, keep walking",
 * -ENOENT means "component missing, stop with what we resolved", any
 * other -errno is fatal — must propagate so the guest's syscall sees
 * it. Earlier the resolver folded all non-EINVAL errors into "stop
 * with the prefix as-is", which silently swallowed -EACCES from a
 * restricted parent dir. */

static long mock_readlink_eacces(void *ctx, const char *suffix,
				 char *out, size_t out_cap)
{
	(void)ctx; (void)out; (void)out_cap; (void)suffix;
	return -13;  /* -EACCES */
}

test(resolver_propagates_eacces)
{
	struct tawcroot_path_oracle ora = {
		.ctx = NULL, .readlink = mock_readlink_eacces,
	};

	char suf[256];
	strcpy(suf, "restricted/file");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, -13);  /* -EACCES propagated */
}

static long mock_readlink_enoent(void *ctx, const char *suffix,
				 char *out, size_t out_cap)
{
	(void)ctx; (void)out; (void)out_cap; (void)suffix;
	return -2;  /* -ENOENT */
}

test(resolver_stops_on_enoent_with_prefix)
{
	/* -ENOENT mid-walk: stop with whatever we've resolved so far,
	 * leave suf untouched, return 0 — the downstream syscall will
	 * produce the kernel-native ENOENT. */
	struct tawcroot_path_oracle ora = {
		.ctx = NULL, .readlink = mock_readlink_enoent,
	};

	char suf[256];
	strcpy(suf, "missing/file");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "missing/file");
}

/* ---- saturated readlink (target == out_cap) is treated as truncation ----
 *
 * Linux readlinkat doesn't tell the caller whether the target was
 * exactly out_cap bytes or got truncated to out_cap. The resolver
 * passes the full buffer and treats `n == out_cap` as truncation, so
 * we fail loud (ENAMETOOLONG) rather than silently using a possibly-
 * truncated target the kernel won't agree with. */

static long mock_readlink_saturate(void *ctx, const char *suffix,
				   char *out, size_t out_cap)
{
	(void)ctx;
	if (suffix[0] == 0) return -22;
	if (strcmp(suffix, "huge") != 0) return -22;
	/* Fill the entire buffer with non-NUL bytes, claim out_cap bytes
	 * of target — the saturation case the kernel exposes when the
	 * actual target is at least `out_cap` bytes long. */
	memset(out, 'x', out_cap);
	return (long)out_cap;
}

test(resolver_saturated_readlink_is_ENAMETOOLONG)
{
	struct tawcroot_path_oracle ora = {
		.ctx = NULL, .readlink = mock_readlink_saturate,
	};

	char suf[256];
	strcpy(suf, "huge");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, -36);  /* -ENAMETOOLONG */
}

/* ---- mid-path symlink with relative `..` target ---- */

test(resolver_relative_dotdot_target)
{
	const struct mock_link links[] = {
		{ "a/b/c", "../d" },
	};
	struct mock_fs fs = { links, 1 };
	struct tawcroot_path_oracle ora = make_oracle(&fs);

	char suf[256];
	strcpy(suf, "a/b/c/leaf");
	long rv = tawcroot_path_resolve_symlinks(suf, sizeof suf,
						 TAWCROOT_PATH_FOLLOW, &ora);
	test_int_eq(rv, 0);
	test_str_eq(suf, "a/d/leaf");
}
