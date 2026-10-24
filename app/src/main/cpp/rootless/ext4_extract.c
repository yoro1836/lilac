// Read-only ext4 extractor, see ext4_extract.h.
//
// Covers what ROM images use: extent-mapped and block-mapped files, linear
// and htree directories (htree blocks read fine as linear ones), fast and
// slow symlinks, 32- and 64-bit group descriptors, flex_bg. Refuses
// inline data, encryption and meta_bg. Build the CLI with
// -DVINE_EXT4_EXTRACT_MAIN.

#define _GNU_SOURCE
#include "ext4_extract.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "owners_table.h"

#define INCOMPAT_FILETYPE    0x2u
#define INCOMPAT_RECOVER     0x4u
#define INCOMPAT_META_BG     0x10u
#define INCOMPAT_EXTENTS     0x40u
#define INCOMPAT_64BIT       0x80u
#define INCOMPAT_MMP         0x100u
#define INCOMPAT_FLEX_BG     0x200u
#define INCOMPAT_EA_INODE    0x400u
#define INCOMPAT_CSUM_SEED   0x2000u
#define INCOMPAT_LARGEDIR    0x4000u
#define INCOMPAT_SUPPORTED   (INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | \
                              INCOMPAT_MMP | INCOMPAT_FLEX_BG | INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | \
                              INCOMPAT_LARGEDIR)

#define EXTENTS_FL     0x80000u
#define INLINE_DATA_FL 0x10000000u
#define EXTENT_MAGIC   0xf30a
#define ROOT_INO       2

struct fs {
    int fd;
    uint64_t image_size;
    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t inodes_count;
    uint32_t desc_size;
    uint64_t gdt_block;
    char *err;
    unsigned long err_len;
    // first extracted path per inode, for hard links
    char **seen;
    int root_fd;
    // ownership table, NULL when not recording
    struct tawcroot_owners_header *owners;
    uint8_t *buf;  // copy buffer, one block run at a time
    size_t buf_len;
};

struct inode {
    uint16_t mode;
    uint32_t uid, gid;
    uint64_t size;
    uint32_t atime, mtime;
    uint16_t links;
    uint32_t flags;
    uint8_t block[60];
};

static int fail(struct fs *fs, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fs->err, fs->err_len, fmt, ap);
    va_end(ap);
    return -1;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(struct fs *fs, void *out, size_t len, uint64_t off)
{
    if (off > fs->image_size || fs->image_size - off < len) return fail(fs, "read past the end of the image");
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fs->fd, (uint8_t *)out + done, len - done, (off_t)(off + done));
        if (n <= 0) return fail(fs, "read image: %s", n ? strerror(errno) : "short read");
        done += (size_t)n;
    }
    return 0;
}

static int read_inode(struct fs *fs, uint32_t ino, struct inode *in)
{
    if (!ino || ino > fs->inodes_count) return fail(fs, "bad inode %u", ino);
    uint32_t group = (ino - 1) / fs->inodes_per_group, index = (ino - 1) % fs->inodes_per_group;
    uint8_t gd[64] = {0};
    if (read_at(fs, gd, fs->desc_size, fs->gdt_block * fs->block_size + (uint64_t)group * fs->desc_size)) return -1;
    uint64_t table = le32(gd + 8);
    if (fs->desc_size >= 64) table |= (uint64_t)le32(gd + 0x28) << 32;
    uint8_t raw[256] = {0};
    size_t len = fs->inode_size < sizeof raw ? fs->inode_size : sizeof raw;
    if (read_at(fs, raw, len, table * fs->block_size + (uint64_t)index * fs->inode_size)) return -1;
    in->mode = le16(raw);
    in->uid = le16(raw + 2) | (uint32_t)le16(raw + 120) << 16;
    in->gid = le16(raw + 24) | (uint32_t)le16(raw + 122) << 16;
    in->size = le32(raw + 4) | (uint64_t)le32(raw + 108) << 32;
    in->atime = le32(raw + 8);
    in->mtime = le32(raw + 16);
    in->links = le16(raw + 26);
    in->flags = le32(raw + 32);
    memcpy(in->block, raw + 40, sizeof in->block);
    if (in->flags & INLINE_DATA_FL) return fail(fs, "inode %u uses inline data, not supported", ino);
    return 0;
}

// ---- block mapping ------------------------------------------------------

typedef int (*run_fn)(struct fs *fs, uint64_t logical, uint64_t physical, uint64_t count, void *ctx);

static int walk_extents(struct fs *fs, const uint8_t *node, size_t node_len, int depth_left, run_fn fn, void *ctx)
{
    if (node_len < 12 || le16(node) != EXTENT_MAGIC) return fail(fs, "bad extent header");
    uint16_t entries = le16(node + 2), depth = le16(node + 6);
    if ((size_t)(entries + 1) * 12 > node_len || depth > 5 || depth_left < 0) return fail(fs, "bad extent node");
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *e = node + 12 + i * 12;
        if (depth == 0) {
            uint32_t logical = le32(e);
            uint16_t len = le16(e + 4);
            uint64_t start = (uint64_t)le16(e + 6) << 32 | le32(e + 8);
            if (len > 32768) continue;  // uninitialized: reads as zeros, a hole for us
            if (fn(fs, logical, start, len, ctx)) return -1;
        } else {
            uint64_t leaf = (uint64_t)le16(e + 8) << 32 | le32(e + 4);
            uint8_t *block = malloc(fs->block_size);
            if (!block) return fail(fs, "out of memory");
            int rv = read_at(fs, block, fs->block_size, leaf * fs->block_size) ? -1
                     : walk_extents(fs, block, fs->block_size, depth_left - 1, fn, ctx);
            free(block);
            if (rv) return -1;
        }
    }
    return 0;
}

static int walk_indirect(struct fs *fs, uint64_t block, int level, uint64_t *logical, uint64_t limit, run_fn fn, void *ctx)
{
    uint32_t per = fs->block_size / 4;
    uint64_t span = 1;
    for (int i = 0; i < level; i++) span *= per;
    if (!block) {
        *logical += span;
        return 0;
    }
    if (level == 0) {
        int rv = *logical < limit ? fn(fs, *logical, block, 1, ctx) : 0;
        (*logical)++;
        return rv;
    }
    uint8_t *ptrs = malloc(fs->block_size);
    if (!ptrs) return fail(fs, "out of memory");
    int rv = read_at(fs, ptrs, fs->block_size, block * fs->block_size);
    for (uint32_t i = 0; !rv && i < per && *logical < limit; i++)
        rv = walk_indirect(fs, le32(ptrs + i * 4), level - 1, logical, limit, fn, ctx);
    free(ptrs);
    return rv;
}

static int walk_blocks(struct fs *fs, const struct inode *in, run_fn fn, void *ctx)
{
    if (in->flags & EXTENTS_FL) return walk_extents(fs, in->block, sizeof in->block, 5, fn, ctx);
    uint64_t limit = (in->size + fs->block_size - 1) / fs->block_size, logical = 0;
    for (int i = 0; i < 12 && logical < limit; i++)
        if (walk_indirect(fs, le32(in->block + i * 4), 0, &logical, limit, fn, ctx)) return -1;
    for (int level = 1; level <= 3 && logical < limit; level++)
        if (walk_indirect(fs, le32(in->block + (11 + level) * 4), level, &logical, limit, fn, ctx)) return -1;
    return 0;
}

// Whole contents into memory (directories, slow symlinks).
struct mem { uint8_t *data; uint64_t size; };

static int to_mem(struct fs *fs, uint64_t logical, uint64_t physical, uint64_t count, void *ctx)
{
    struct mem *m = ctx;
    uint64_t off = logical * fs->block_size;
    if (off >= m->size) return 0;
    uint64_t len = count * fs->block_size;
    if (len > m->size - off) len = m->size - off;
    return read_at(fs, m->data + off, len, physical * fs->block_size);
}

static int read_all(struct fs *fs, const struct inode *in, struct mem *m)
{
    if (in->size > (64u << 20)) return fail(fs, "directory or symlink too large");
    m->size = in->size;
    m->data = calloc(1, in->size + 1);
    if (!m->data) return fail(fs, "out of memory");
    if (walk_blocks(fs, in, to_mem, m)) {
        free(m->data);
        return -1;
    }
    return 0;
}

// Regular file contents, holes left sparse.
struct copy { int out; uint64_t size; const char *name; };

static int to_file(struct fs *fs, uint64_t logical, uint64_t physical, uint64_t count, void *ctx)
{
    struct copy *c = ctx;
    uint64_t off = logical * fs->block_size;
    if (off >= c->size) return 0;
    uint64_t len = count * fs->block_size;
    if (len > c->size - off) len = c->size - off;
    for (uint64_t done = 0; done < len;) {
        size_t chunk = len - done < fs->buf_len ? (size_t)(len - done) : fs->buf_len;
        if (read_at(fs, fs->buf, chunk, physical * fs->block_size + done)) return -1;
        for (size_t w = 0; w < chunk;) {
            ssize_t n = pwrite(c->out, fs->buf + w, chunk - w, (off_t)(off + done + w));
            if (n < 0) return fail(fs, "write %s: %s", c->name, strerror(errno));
            w += (size_t)n;
        }
        done += chunk;
    }
    return 0;
}

// ---- ownership ------------------------------------------------------------

static void own(struct fs *fs, const struct stat *st, uint32_t uid, uint32_t gid)
{
    struct tawcroot_owners_header *h = fs->owners;
    if (!h) return;
    struct tawcroot_owner_slot *slots = (struct tawcroot_owner_slot *)(h + 1);
    uint64_t dev = (uint64_t)st->st_dev, ino = (uint64_t)st->st_ino;
    uint64_t ids = uid | (uint64_t)gid << 32;
    uint32_t i = tawcroot_owners_hash(dev, ino);
    struct tawcroot_owner_slot *tomb = NULL;
    // Same publication order as tawcroot's writer (owners_table.h).
    for (uint32_t n = 0; n < TAWCROOT_OWNERS_SLOTS; n++, i = (i + 1) & (TAWCROOT_OWNERS_SLOTS - 1)) {
        struct tawcroot_owner_slot *s = &slots[i];
        if (!s->ino) break;
        if (s->dev == dev && s->ino == ino) {
            if (ids) __atomic_store_n(&s->ids, ids, __ATOMIC_RELEASE);
            else __atomic_store_n(&s->dev, TAWCROOT_OWNERS_TOMB, __ATOMIC_RELEASE);
            return;
        }
        if (s->dev == TAWCROOT_OWNERS_TOMB && !tomb) tomb = s;
    }
    if (!ids) return;
    struct tawcroot_owner_slot *s = tomb;
    if (!s) {
        if (h->used >= TAWCROOT_OWNERS_SLOTS / 4 * 3) return;
        s = &slots[i];
        __atomic_store_n(&s->dev, TAWCROOT_OWNERS_TOMB, __ATOMIC_RELAXED);
        h->used++;
    }
    __atomic_store_n(&s->ino, ino, __ATOMIC_RELEASE);
    __atomic_store_n(&s->ids, ids, __ATOMIC_RELEASE);
    __atomic_store_n(&s->dev, dev, __ATOMIC_RELEASE);
}

static int open_owners(struct fs *fs, const char *path, int *fd_out)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return fail(fs, "open %s: %s", path, strerror(errno));
    struct stat st;
    if (flock(fd, LOCK_EX) || fstat(fd, &st) ||
        ((uint64_t)st.st_size < TAWCROOT_OWNERS_BYTES && ftruncate(fd, (off_t)TAWCROOT_OWNERS_BYTES))) {
        close(fd);
        return fail(fs, "prepare %s: %s", path, strerror(errno));
    }
    void *p = mmap(NULL, TAWCROOT_OWNERS_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return fail(fs, "map %s: %s", path, strerror(errno));
    }
    struct tawcroot_owners_header *h = p;
    if (!h->magic) {
        h->magic = TAWCROOT_OWNERS_MAGIC;
        h->version = TAWCROOT_OWNERS_VERSION;
        h->slots = TAWCROOT_OWNERS_SLOTS;
    } else if (h->magic != TAWCROOT_OWNERS_MAGIC || h->version != TAWCROOT_OWNERS_VERSION ||
               h->slots != TAWCROOT_OWNERS_SLOTS) {
        munmap(p, TAWCROOT_OWNERS_BYTES);
        close(fd);
        return fail(fs, "%s is not a tawcroot ownership table", path);
    }
    fs->owners = h;
    *fd_out = fd;  // closing it drops the lock
    return 0;
}

// ---- tree -----------------------------------------------------------------

static void set_times(int dirfd, const char *name, const struct inode *in)
{
    struct timespec ts[2] = { { in->atime, 0 }, { in->mtime, 0 } };
    utimensat(dirfd, name, ts, AT_SYMLINK_NOFOLLOW);
}

static void set_times_fd(int fd, const struct inode *in)
{
    struct timespec ts[2] = { { in->atime, 0 }, { in->mtime, 0 } };
    futimens(fd, ts);
}

static int extract_dir(struct fs *fs, uint32_t ino, int dirfd, const char *rel, int depth);

static int extract_entry(struct fs *fs, uint32_t ino, int dirfd, const char *name, const char *rel, int depth)
{
    struct inode in;
    if (read_inode(fs, ino, &in)) return -1;
    struct stat st;
    switch (in.mode & S_IFMT) {
    case S_IFDIR: {
        if (mkdirat(dirfd, name, 0700) && errno != EEXIST) return fail(fs, "mkdir %s: %s", rel, strerror(errno));
        int sub = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) return fail(fs, "open %s: %s", rel, strerror(errno));
        int rv = extract_dir(fs, ino, sub, rel, depth + 1);
        if (!rv) {
            fchmod(sub, in.mode & 07777);
            set_times_fd(sub, &in);
            if (!fstat(sub, &st)) own(fs, &st, in.uid, in.gid);
        }
        close(sub);
        return rv;
    }
    case S_IFREG: {
        if (in.links > 1 && fs->seen[ino]) {
            if (!linkat(fs->root_fd, fs->seen[ino], dirfd, name, 0)) return 0;
            // No hard links here (Android denies link(2) to apps): copy.
        }
        int out = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (out < 0) return fail(fs, "create %s: %s", rel, strerror(errno));
        struct copy c = { out, in.size, rel };
        int rv = walk_blocks(fs, &in, to_file, &c);
        if (!rv && ftruncate(out, (off_t)in.size)) rv = fail(fs, "truncate %s: %s", rel, strerror(errno));
        if (!rv) {
            fchmod(out, in.mode & 07777);
            set_times_fd(out, &in);
            if (!fstat(out, &st)) own(fs, &st, in.uid, in.gid);
            if (in.links > 1 && !fs->seen[ino]) fs->seen[ino] = strdup(rel);
        }
        close(out);
        return rv;
    }
    case S_IFLNK: {
        struct mem m = {0};
        if (in.size < 60 && !(in.flags & EXTENTS_FL)) {
            m.data = calloc(1, 61);
            if (!m.data) return fail(fs, "out of memory");
            memcpy(m.data, in.block, in.size);
        } else if (read_all(fs, &in, &m)) {
            return -1;
        }
        m.data[in.size] = 0;
        int rv = symlinkat((const char *)m.data, dirfd, name) ? fail(fs, "symlink %s: %s", rel, strerror(errno)) : 0;
        free(m.data);
        if (!rv) {
            set_times(dirfd, name, &in);
            if (!fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) own(fs, &st, in.uid, in.gid);
        }
        return rv;
    }
    default:
        // Device nodes, fifos and sockets: not in ROM images, not creatable unprivileged.
        return 0;
    }
}

static int extract_dir(struct fs *fs, uint32_t ino, int dirfd, const char *rel, int depth)
{
    if (depth > 64) return fail(fs, "directory nesting too deep at %s", rel);
    struct inode in;
    struct mem m;
    if (read_inode(fs, ino, &in) || !S_ISDIR(in.mode) || read_all(fs, &in, &m)) {
        if (!fs->err[0]) fail(fs, "inode %u is not a directory", ino);
        return -1;
    }
    int rv = 0;
    char child[4096];
    for (uint64_t pos = 0; !rv && pos + 8 <= m.size;) {
        const uint8_t *d = m.data + pos;
        uint32_t child_ino = le32(d);
        uint16_t rec_len = le16(d + 4);
        uint8_t name_len = d[6];
        if (rec_len < 8 || pos + rec_len > m.size || 8u + name_len > rec_len) {
            rv = fail(fs, "corrupt directory %s", rel);
            break;
        }
        pos += rec_len;
        if (!child_ino || !name_len) continue;  // free slot, htree node, csum tail
        char name[256];
        memcpy(name, d + 8, name_len);
        name[name_len] = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) continue;
        snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", name);
        rv = extract_entry(fs, child_ino, dirfd, name, child, depth);
    }
    free(m.data);
    return rv;
}

int vine_ext4_extract(const char *image, const char *dest, const char *owners_path, char *err, unsigned long err_len)
{
    struct fs fs = { .fd = -1, .root_fd = -1, .err = err, .err_len = err_len };
    if (err_len) err[0] = 0;
    int owners_fd = -1, rv = -1;
    fs.fd = open(image, O_RDONLY | O_CLOEXEC);
    if (fs.fd < 0) return fail(&fs, "open %s: %s", image, strerror(errno));
    struct stat ist;
    uint8_t sb[1024];
    if (fstat(fs.fd, &ist)) {
        fail(&fs, "stat %s: %s", image, strerror(errno));
        goto out;
    }
    fs.image_size = (uint64_t)ist.st_size;
    if (read_at(&fs, sb, sizeof sb, 1024)) goto out;
    if (le16(sb + 56) != 0xef53) {
        fail(&fs, "%s is not an ext2/3/4 image", image);
        goto out;
    }
    uint32_t incompat = le32(sb + 96);
    if (incompat & ~INCOMPAT_SUPPORTED) {
        fail(&fs, "unsupported ext4 features %#x", incompat & ~INCOMPAT_SUPPORTED);
        goto out;
    }
    fs.block_size = 1024u << le32(sb + 24);
    fs.inodes_count = le32(sb);
    fs.inodes_per_group = le32(sb + 40);
    fs.inode_size = le32(sb + 76) ? le16(sb + 88) : 128;
    fs.desc_size = (incompat & INCOMPAT_64BIT) && le16(sb + 254) >= 64 ? le16(sb + 254) : 32;
    fs.gdt_block = le32(sb + 20) + 1;
    if (fs.block_size > 65536 || !fs.inodes_per_group || fs.inode_size < 128 || fs.desc_size > 64) {
        fail(&fs, "implausible superblock");
        goto out;
    }
    fs.seen = calloc((size_t)fs.inodes_count + 1, sizeof *fs.seen);
    fs.buf_len = 1u << 20;
    fs.buf = malloc(fs.buf_len);
    if (!fs.seen || !fs.buf) {
        fail(&fs, "out of memory");
        goto out;
    }
    if (mkdir(dest, 0700) && errno != EEXIST) {
        fail(&fs, "mkdir %s: %s", dest, strerror(errno));
        goto out;
    }
    fs.root_fd = open(dest, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fs.root_fd < 0) {
        fail(&fs, "open %s: %s", dest, strerror(errno));
        goto out;
    }
    if (owners_path && open_owners(&fs, owners_path, &owners_fd)) goto out;
    rv = extract_dir(&fs, ROOT_INO, fs.root_fd, "", 0);
    if (!rv) {
        struct inode root;
        struct stat st;
        if (!read_inode(&fs, ROOT_INO, &root)) {
            fchmod(fs.root_fd, root.mode & 07777);
            set_times_fd(fs.root_fd, &root);
            if (!fstat(fs.root_fd, &st)) own(&fs, &st, root.uid, root.gid);
        }
    }
out:
    if (fs.owners) munmap(fs.owners, TAWCROOT_OWNERS_BYTES);
    if (owners_fd >= 0) close(owners_fd);
    if (fs.seen) {
        for (uint32_t i = 0; i <= fs.inodes_count; i++) free(fs.seen[i]);
        free(fs.seen);
    }
    free(fs.buf);
    if (fs.root_fd >= 0) close(fs.root_fd);
    close(fs.fd);
    return rv;
}

#ifdef VINE_EXT4_EXTRACT_MAIN
int main(int argc, char **argv)
{
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: ext4extract IMAGE DEST [--owners TABLE]\n");
        return 2;
    }
    const char *owners = argc == 5 && !strcmp(argv[3], "--owners") ? argv[4] : NULL;
    if (argc == 5 && !owners) {
        fprintf(stderr, "usage: ext4extract IMAGE DEST [--owners TABLE]\n");
        return 2;
    }
    char err[512];
    if (vine_ext4_extract(argv[1], argv[2], owners, err, sizeof err)) {
        fprintf(stderr, "ext4extract: %s\n", err);
        return 1;
    }
    return 0;
}
#endif
