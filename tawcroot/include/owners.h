/* Persistent file ownership (`--owners`).
 *
 * The app uid owns every file on the host, so without this a guest that
 * switches uids (Android: system, app uids) sees no ownership at all.
 * With it, st_uid/st_gid come from the shared table in `<store>/owners`
 * (include/owners_table.h): files without an entry are root:root, files
 * a non-root guest creates belong to its fsuid/fsgid, chown follows the
 * kernel's rules for the virtual identity (CAP_CHOWN only at euid 0),
 * and removing the last name of an inode drops its entry so a reused
 * inode number starts clean.
 *
 * Permission checks are not emulated: the kernel still sees the app uid
 * everywhere. Enabled per session (TAWC_NS_OWNERS) and inherited across
 * fork and exec. Async-signal-safe.
 */

#pragma once

#include <stdint.h>
#include <sys/stat.h>

struct statx;

struct tawcroot_owner_key { uint64_t dev, ino; };

/* --owners: create the table under the store dir and turn the session
 * flag on. -errno when there is no store. */
long tawcroot_owners_setup(void);
int  tawcroot_owners_on(void);

/* Report the recorded owner (root when none). */
void tawcroot_owners_stat(struct stat *st);
void tawcroot_owners_statx(struct statx *sx);

/* chown on the inode `st` describes; uid/gid of -1 keep the field.
 * Returns 0 or -EPERM (non-root changing the owner, or a group it is
 * not in) or a table error. */
long tawcroot_owners_chown(const struct stat *st, uint32_t uid, uint32_t gid);

/* A guest just created this inode: record the creator. */
void tawcroot_owners_created_fd(int fd);
void tawcroot_owners_created_at(int dirfd, const char *path);

/* Before removing a name: 1 and the key when the inode dies with it. */
int  tawcroot_owners_doomed(int dirfd, const char *path, struct tawcroot_owner_key *key);
void tawcroot_owners_forget(const struct tawcroot_owner_key *key);
