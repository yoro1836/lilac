/* Persistent file ownership. See include/owners.h and owners_table.h.
 *
 * Each process maps the table once (the mapping survives fork; exec
 * re-maps lazily). Writers open the file afresh and flock it: flock
 * belongs to the open file description, which threads and forked
 * children would otherwise share and so never exclude each other.
 * Writes are rare (chown, create, unlink); stat only reads the map.
 */

#include <stddef.h>
#include <stdint.h>

#include <asm/unistd.h>
#include <sys/stat.h>

#include "errno_neg.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "namespace.h"
#include "owners.h"
#include "owners_table.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"

#define PROT_READ_  1
#define PROT_WRITE_ 2
#define MAP_SHARED_ 1
#define LOCK_EX_    2
#define LOCK_UN_    8

static struct tawcroot_owners_header *g_table;
static int g_table_missing;  /* don't retry the open on every stat */
static volatile int g_map_lock;

int tawcroot_owners_on(void)
{
	return (tawcroot_namespace.session & TAWC_NS_OWNERS) != 0;
}

static struct tawcroot_owner_slot *slots(struct tawcroot_owners_header *h)
{
	return (struct tawcroot_owner_slot *)(h + 1);
}

static long table_path(char *out)
{
	size_t n = 0;
	if (!tawcroot_store_host_path_len) return TAWC_ENOENT;
	if (tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) ||
	    tawc_str_append(out, TAWCROOT_PATH_SCRATCH_SIZE, &n, "/owners"))
		return TAWC_ENAMETOOLONG;
	return 0;
}

static long open_table(int flags)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	long e = table_path(path);
	if (e) return e;
	return tawc_openat(AT_FDCWD, path, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
}

static struct tawcroot_owners_header *table(void)
{
	struct tawcroot_owners_header *h = __atomic_load_n(&g_table, __ATOMIC_ACQUIRE);
	if (h || __atomic_load_n(&g_table_missing, __ATOMIC_RELAXED)) return h;
	while (__atomic_test_and_set(&g_map_lock, __ATOMIC_ACQUIRE)) {}
	h = g_table;
	if (!h) {
		long fd = open_table(O_RDWR);
		if (fd >= 0) {
			long p = tawc_mmap(0, TAWCROOT_OWNERS_BYTES, PROT_READ_ | PROT_WRITE_,
					   MAP_SHARED_, (int)fd, 0);
			tawc_close((int)fd);
			if (p > 0 || p < -4095) {
				h = (struct tawcroot_owners_header *)p;
				if (h->magic != TAWCROOT_OWNERS_MAGIC || h->slots != TAWCROOT_OWNERS_SLOTS) {
					tawc_munmap(h, TAWCROOT_OWNERS_BYTES);
					h = 0;
				}
			}
		}
		__atomic_store_n(&g_table, h, __ATOMIC_RELEASE);
		if (!h) __atomic_store_n(&g_table_missing, 1, __ATOMIC_RELAXED);
	}
	__atomic_clear(&g_map_lock, __ATOMIC_RELEASE);
	return h;
}

long tawcroot_owners_setup(void)
{
	if (!tawcroot_store_host_path_len) return TAWC_ENOENT;
	(void)TAWC_RAW(TAWC_SYS_mkdirat, AT_FDCWD, (long)tawcroot_store_host_path, 0700, 0, 0, 0);
	long fd = open_table(O_RDWR | O_CREAT);
	if (fd < 0) return fd;
	long rv = TAWC_RAW(__NR_flock, fd, LOCK_EX_, 0, 0, 0, 0);
	struct stat st;
	if (!rv) rv = TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0);
	if (!rv && (uint64_t)st.st_size < TAWCROOT_OWNERS_BYTES)
		rv = TAWC_RAW(TAWC_SYS_ftruncate, fd, (long)TAWCROOT_OWNERS_BYTES, 0, 0, 0, 0);
	if (!rv) {
		struct tawcroot_owners_header h;
		long n = TAWC_RAW(TAWC_SYS_pread64, fd, (long)&h, sizeof h, 0, 0, 0);
		if (n != (long)sizeof h) rv = n < 0 ? n : TAWC_EIO;
		else if (!h.magic) {
			memset(&h, 0, sizeof h);
			h.magic = TAWCROOT_OWNERS_MAGIC;
			h.version = TAWCROOT_OWNERS_VERSION;
			h.slots = TAWCROOT_OWNERS_SLOTS;
			n = TAWC_RAW(TAWC_SYS_pwrite64, fd, (long)&h, sizeof h, 0, 0, 0);
			if (n != (long)sizeof h) rv = n < 0 ? n : TAWC_EIO;
		} else if (h.magic != TAWCROOT_OWNERS_MAGIC || h.version != TAWCROOT_OWNERS_VERSION ||
			   h.slots != TAWCROOT_OWNERS_SLOTS) {
			rv = TAWC_EINVAL;
		}
	}
	tawc_close((int)fd);
	if (rv) return rv;
	tawcroot_namespace.session |= TAWC_NS_OWNERS;
	return 0;
}

/* ---- table operations ---------------------------------------------------- */

static int lookup(struct tawcroot_owners_header *h, uint64_t dev, uint64_t ino, uint64_t *ids)
{
	struct tawcroot_owner_slot *s = slots(h);
	uint32_t i = tawcroot_owners_hash(dev, ino);
	for (uint32_t n = 0; n < TAWCROOT_OWNERS_SLOTS; n++, i = (i + 1) & (TAWCROOT_OWNERS_SLOTS - 1)) {
		uint64_t sino = __atomic_load_n(&s[i].ino, __ATOMIC_ACQUIRE);
		if (!sino) return 0;
		if (__atomic_load_n(&s[i].dev, __ATOMIC_ACQUIRE) != dev) continue;
		if (__atomic_load_n(&s[i].ino, __ATOMIC_RELAXED) != ino) continue;
		*ids = __atomic_load_n(&s[i].ids, __ATOMIC_RELAXED);
		return 1;
	}
	return 0;
}

/* Under the write lock. ids == 0 (root:root) removes the entry. */
static long store_locked(struct tawcroot_owners_header *h, uint64_t dev, uint64_t ino, uint64_t ids)
{
	struct tawcroot_owner_slot *s = slots(h);
	uint32_t i = tawcroot_owners_hash(dev, ino);
	struct tawcroot_owner_slot *tomb = 0;
	for (uint32_t n = 0; n < TAWCROOT_OWNERS_SLOTS; n++, i = (i + 1) & (TAWCROOT_OWNERS_SLOTS - 1)) {
		uint64_t sino = s[i].ino, sdev = s[i].dev;
		if (!sino) break;
		if (sdev == dev && sino == ino) {
			if (ids) __atomic_store_n(&s[i].ids, ids, __ATOMIC_RELEASE);
			else __atomic_store_n(&s[i].dev, TAWCROOT_OWNERS_TOMB, __ATOMIC_RELEASE);
			return 0;
		}
		if (sdev == TAWCROOT_OWNERS_TOMB && !tomb) tomb = &s[i];
	}
	if (!ids) return 0;
	struct tawcroot_owner_slot *slot = tomb;
	if (!slot) {
		/* Keep a quarter free so probes stay short and terminate. */
		if (h->used >= TAWCROOT_OWNERS_SLOTS / 4 * 3) return TAWC_ENOSPC;
		slot = &s[i];
		__atomic_store_n(&slot->dev, TAWCROOT_OWNERS_TOMB, __ATOMIC_RELAXED);
		h->used++;
	}
	__atomic_store_n(&slot->ino, ino, __ATOMIC_RELEASE);
	__atomic_store_n(&slot->ids, ids, __ATOMIC_RELEASE);
	__atomic_store_n(&slot->dev, dev, __ATOMIC_RELEASE);
	return 0;
}

static long store(uint64_t dev, uint64_t ino, uint64_t ids)
{
	struct tawcroot_owners_header *h = table();
	if (!h) return TAWC_EIO;
	long fd = open_table(O_RDWR);
	if (fd < 0) return fd;
	long rv = TAWC_RAW(__NR_flock, fd, LOCK_EX_, 0, 0, 0, 0);
	if (!rv) rv = store_locked(h, dev, ino, ids);
	tawc_close((int)fd);  /* releases the lock */
	return rv;
}

static uint64_t ids_of(uint32_t uid, uint32_t gid)
{
	return uid | (uint64_t)gid << 32;
}

static uint64_t owner_of(uint64_t dev, uint64_t ino)
{
	uint64_t ids = 0;
	struct tawcroot_owners_header *h = table();
	if (h) (void)lookup(h, dev, ino, &ids);
	return ids;
}

/* ---- hooks ---------------------------------------------------------------- */

void tawcroot_owners_stat(struct stat *st)
{
	uint64_t ids = owner_of((uint64_t)st->st_dev, (uint64_t)st->st_ino);
	st->st_uid = (uint32_t)ids;
	st->st_gid = (uint32_t)(ids >> 32);
}

void tawcroot_owners_statx(struct statx *sx)
{
	uint64_t ids = owner_of(tawcroot_owners_dev(sx->stx_dev_major, sx->stx_dev_minor), sx->stx_ino);
	sx->stx_uid = (uint32_t)ids;
	sx->stx_gid = (uint32_t)(ids >> 32);
}

static int in_groups(const tawc_identity *id, uint32_t gid)
{
	if (id->fsgid == gid) return 1;
	for (uint32_t i = 0; i < id->ngroups && i < TAWC_IDENTITY_NGROUPS; i++)
		if (id->groups[i] == gid) return 1;
	return 0;
}

long tawcroot_owners_chown(const struct stat *st, uint32_t uid, uint32_t gid)
{
	uint64_t cur = owner_of((uint64_t)st->st_dev, (uint64_t)st->st_ino);
	uint32_t cu = (uint32_t)cur, cg = (uint32_t)(cur >> 32);
	uint32_t nu = uid == (uint32_t)-1 ? cu : uid;
	uint32_t ng = gid == (uint32_t)-1 ? cg : gid;
	tawc_identity id;
	tawcroot_identity_get(&id);
	if (id.euid != 0) {
		/* Without CAP_CHOWN: the owner may only change the group, to
		 * one it belongs to. */
		if (nu != cu || id.fsuid != cu) return TAWC_EPERM;
		if (ng != cg && !in_groups(&id, ng)) return TAWC_EPERM;
	}
	if (nu == cu && ng == cg) return 0;
	return store((uint64_t)st->st_dev, (uint64_t)st->st_ino, ids_of(nu, ng));
}

static void record_creator(const struct stat *st)
{
	tawc_identity id;
	tawcroot_identity_get(&id);
	struct tawcroot_owner_key key = { (uint64_t)st->st_dev, (uint64_t)st->st_ino };
	uint64_t ids = ids_of(id.fsuid, id.fsgid);
	/* Root's files need no entry, but a reused inode may carry one. */
	if (!ids) tawcroot_owners_forget(&key);
	else (void)store(key.dev, key.ino, ids);
}

void tawcroot_owners_created_fd(int fd)
{
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, fd, (long)&st, 0, 0, 0, 0) == 0) record_creator(&st);
}

void tawcroot_owners_created_at(int dirfd, const char *path)
{
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)path, (long)&st, AT_SYMLINK_NOFOLLOW, 0, 0) == 0)
		record_creator(&st);
}

int tawcroot_owners_doomed(int dirfd, const char *path, struct tawcroot_owner_key *key)
{
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstatat, dirfd, (long)path, (long)&st, AT_SYMLINK_NOFOLLOW, 0, 0)) return 0;
	if (!S_ISDIR(st.st_mode) && st.st_nlink > 1) return 0;
	key->dev = (uint64_t)st.st_dev;
	key->ino = (uint64_t)st.st_ino;
	return 1;
}

void tawcroot_owners_forget(const struct tawcroot_owner_key *key)
{
	uint64_t ids;
	struct tawcroot_owners_header *h = table();
	/* Skip the lock when there is nothing to drop (the common case). */
	if (h && lookup(h, key->dev, key->ino, &ids)) (void)store(key->dev, key->ino, 0);
}
