/* On-disk layout of the ownership store, `<store>/owners`.
 *
 * Shared by tawcroot (src/owners.c) and host-side writers (the app's
 * image extractor records the ownership an ext4 image carries). Fixed-
 * width types only.
 *
 * An open-addressing hash table keyed by (st_dev, st_ino), linear
 * probing, mapped MAP_SHARED by every guest process. Files without an
 * entry are root:root. Readers are lock-free; writers hold flock(LOCK_EX)
 * on the file and publish in this order so a concurrent reader never
 * matches a half-written slot:
 *
 *   fill an empty slot:  dev = TOMB, ino = key, ids, then dev = key (release)
 *   reuse a tombstone:   (dev is TOMB) ino = key, ids, then dev = key
 *   update:              ids (one 64-bit store)
 *   remove:              dev = TOMB (ino stays non-zero: probe chains hold)
 *
 * A reader stops at ino == 0 and matches on dev (acquire), then ino.
 */

#pragma once

#include <stdint.h>

#define TAWCROOT_OWNERS_MAGIC   0x314e574fu  /* "OWN1" */
#define TAWCROOT_OWNERS_VERSION 1
#define TAWCROOT_OWNERS_SLOTS   (1u << 20)   /* 24 MiB, sparse */
#define TAWCROOT_OWNERS_TOMB    (~(uint64_t)0)

struct tawcroot_owners_header {
	uint32_t magic;
	uint32_t version;
	uint32_t slots;
	uint32_t used;          /* slots ever filled (live + tombstones) */
	uint64_t reserved[6];
};

struct tawcroot_owner_slot {
	uint64_t dev;           /* st_dev, kernel encoding; TOMB = removed */
	uint64_t ino;           /* 0 = never used */
	uint64_t ids;           /* uid | (uint64_t)gid << 32 */
};

#define TAWCROOT_OWNERS_BYTES \
	(sizeof(struct tawcroot_owners_header) + \
	 (uint64_t)TAWCROOT_OWNERS_SLOTS * sizeof(struct tawcroot_owner_slot))

static inline uint32_t tawcroot_owners_hash(uint64_t dev, uint64_t ino)
{
	uint64_t h = ino * 0x9e3779b97f4a7c15ull ^ (dev + 0x632be59bd9b4e019ull);
	h ^= h >> 29;
	h *= 0xbf58476d1ce4e5b9ull;
	h ^= h >> 32;
	return (uint32_t)h & (TAWCROOT_OWNERS_SLOTS - 1);
}

/* st_dev from statx's major/minor (the kernel's new_encode_dev, which
 * 64-bit stat reports). */
static inline uint64_t tawcroot_owners_dev(uint32_t major, uint32_t minor)
{
	return (minor & 0xffu) | ((uint64_t)major << 8) | ((uint64_t)(minor & ~0xffu) << 12);
}
