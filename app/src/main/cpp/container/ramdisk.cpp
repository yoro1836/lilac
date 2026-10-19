#include "ramdisk.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace vine {

namespace {

constexpr size_t kNewcHeaderSize = 110;

struct CpioEntry {
    uint32_t ino = 0, mode = 0, uid = 0, gid = 0, nlink = 0, mtime = 0, filesize = 0;
    uint32_t rdevmajor = 0, rdevminor = 0, namesize = 0;
};

bool parse_hex8(const char* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

class GzReader {
public:
    explicit GzReader(const std::string& path) { file_ = gzopen(path.c_str(), "rb"); }
    ~GzReader() { if (file_) gzclose(file_); }
    bool ok() const { return file_ != nullptr; }

    bool read_exact(void* buf, size_t len) {
        char* p = static_cast<char*>(buf);
        size_t done = 0;
        while (done < len) {
            int n = gzread(file_, p + done, (unsigned)(len - done));
            if (n <= 0) return false;
            done += (size_t)n;
        }
        return true;
    }

    bool skip(size_t len) {
        char scratch[4096];
        while (len > 0) {
            size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
            if (!read_exact(scratch, chunk)) return false;
            len -= chunk;
        }
        return true;
    }

private:
    gzFile file_ = nullptr;
};

// Normalises an archive member name into a path relative to the destination; returns false for ".." components.
bool sanitize_name(const std::string& raw, std::string* out) {
    out->clear();
    for (const auto& comp : split_fields(raw, '/')) {
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") return false;
        if (!out->empty()) *out += '/';
        *out += comp;
    }
    return true;
}

// Refuses to write through a symlinked parent directory, which a hostile archive could use to escape dest.
bool parents_are_plain_dirs(const std::string& dest, const std::string& rel) {
    std::string cur = dest;
    const auto parts = split_fields(rel, '/');
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        cur += "/" + parts[i];
        struct stat st{};
        if (lstat(cur.c_str(), &st) != 0) return true; // not created yet, mkdirs will make a real directory
        if (!S_ISDIR(st.st_mode)) return false;
    }
    return true;
}

void apply_metadata(const std::string& path, const CpioEntry& e, bool is_symlink) {
    if (lchown(path.c_str(), e.uid, e.gid) != 0 && errno != EPERM) {
        VINE_LOGW("ramdisk: lchown(%s): %s", path.c_str(), strerror(errno));
    }
    if (!is_symlink) chmod(path.c_str(), e.mode & 07777);
    struct timespec ts[2] = {{(time_t)e.mtime, 0}, {(time_t)e.mtime, 0}};
    utimensat(AT_FDCWD, path.c_str(), ts, AT_SYMLINK_NOFOLLOW);
}

} // namespace

std::string detect_archive_format(const std::string& path) {
    auto head = read_file_prefix(path, 8);
    if (!head || head->size() < 4) return "unknown";
    const auto* b = reinterpret_cast<const unsigned char*>(head->data());
    if (b[0] == 0x1f && b[1] == 0x8b) return "gzip";
    if (b[0] == 0x04 && b[1] == 0x22 && b[2] == 0x4d && b[3] == 0x18) return "lz4";
    if (b[0] == 0x02 && b[1] == 0x21 && b[2] == 0x4c && b[3] == 0x18) return "lz4";
    if (b[0] == 0xfd && b[1] == '7' && b[2] == 'z' && b[3] == 'X') return "xz";
    if (b[0] == 'B' && b[1] == 'Z' && b[2] == 'h') return "bzip2";
    if (b[0] == 0x28 && b[1] == 0xb5 && b[2] == 0x2f && b[3] == 0xfd) return "zstd";
    if (head->compare(0, 6, "070701") == 0) return "cpio";
    return "unknown";
}

bool extract_ramdisk(const std::string& archive_path, const std::string& dest_dir, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        VINE_LOGE("extract_ramdisk: %s", msg.c_str());
        return false;
    };

    const std::string format = detect_archive_format(archive_path);
    if (format != "gzip" && format != "cpio") {
        return fail("ramdisk.img is " + format + "-compressed; repack it as gzip (zlib) or a plain cpio newc archive");
    }
    if (!mkdirs(dest_dir)) return fail("cannot create " + dest_dir);

    GzReader reader(archive_path);
    if (!reader.ok()) return fail("cannot open " + archive_path);

    std::map<uint32_t, std::vector<std::string>> pending_links; // ino -> empty placeholders waiting for their data
    size_t entries = 0;

    for (;;) {
        char hdr[kNewcHeaderSize];
        if (!reader.read_exact(hdr, sizeof(hdr))) return fail("truncated cpio header");
        if (memcmp(hdr, "070701", 6) != 0 && memcmp(hdr, "070702", 6) != 0) return fail("bad cpio magic (not newc)");

        CpioEntry e;
        uint32_t ignored = 0;
        const char* f = hdr + 6;
        if (!parse_hex8(f + 0 * 8, &e.ino) || !parse_hex8(f + 1 * 8, &e.mode) || !parse_hex8(f + 2 * 8, &e.uid) ||
            !parse_hex8(f + 3 * 8, &e.gid) || !parse_hex8(f + 4 * 8, &e.nlink) || !parse_hex8(f + 5 * 8, &e.mtime) ||
            !parse_hex8(f + 6 * 8, &e.filesize) || !parse_hex8(f + 7 * 8, &ignored) || !parse_hex8(f + 8 * 8, &ignored) ||
            !parse_hex8(f + 9 * 8, &e.rdevmajor) || !parse_hex8(f + 10 * 8, &e.rdevminor) ||
            !parse_hex8(f + 11 * 8, &e.namesize)) {
            return fail("corrupt cpio header");
        }
        if (e.namesize == 0 || e.namesize > 4096) return fail("implausible cpio name length");

        std::string name(e.namesize, '\0');
        if (!reader.read_exact(&name[0], e.namesize)) return fail("truncated cpio name");
        name.resize(strnlen(name.c_str(), name.size()));
        const size_t name_pad = (4 - ((kNewcHeaderSize + e.namesize) % 4)) % 4;
        if (!reader.skip(name_pad)) return fail("truncated cpio padding");

        if (name == "TRAILER!!!") break;

        const size_t data_pad = (4 - (e.filesize % 4)) % 4;
        std::string rel;
        if (!sanitize_name(name, &rel)) return fail("unsafe path in archive: " + name);
        std::string data;
        const bool is_dir = S_ISDIR(e.mode);
        const bool is_reg = S_ISREG(e.mode);
        const bool is_lnk = S_ISLNK(e.mode);

        if (rel.empty()) { // "." or the root itself
            if (!reader.skip(e.filesize + data_pad)) return fail("truncated cpio data");
            continue;
        }
        if (!parents_are_plain_dirs(dest_dir, rel)) return fail("archive writes through a symlink: " + name);

        const std::string path = dest_dir + "/" + rel;
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos && !is_directory(path.substr(0, slash))) mkdirs(path.substr(0, slash));

        if (is_reg) {
            const bool placeholder = e.nlink > 1 && e.filesize == 0;
            unlink(path.c_str());
            int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) return fail("cannot create " + path + ": " + strerror(errno));
            uint32_t left = e.filesize;
            char buf[65536];
            while (left > 0) {
                size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
                if (!reader.read_exact(buf, chunk)) { close(fd); return fail("truncated data for " + name); }
                size_t off = 0;
                while (off < chunk) {
                    ssize_t w = write(fd, buf + off, chunk - off);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        close(fd);
                        return fail("write failed for " + path + ": " + strerror(errno));
                    }
                    off += (size_t)w;
                }
                left -= (uint32_t)chunk;
            }
            close(fd);
            if (!reader.skip(data_pad)) return fail("truncated cpio data");
            apply_metadata(path, e, false);

            if (placeholder) {
                pending_links[e.ino].push_back(path);
            } else if (e.nlink > 1) {
                auto it = pending_links.find(e.ino);
                if (it != pending_links.end()) {
                    for (const auto& other : it->second) {
                        unlink(other.c_str());
                        if (link(path.c_str(), other.c_str()) != 0) {
                            return fail("cannot hardlink " + other + ": " + strerror(errno));
                        }
                    }
                    pending_links.erase(it);
                }
            }
        } else if (is_dir) {
            if (!reader.skip(e.filesize + data_pad)) return fail("truncated cpio data");
            if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return fail("mkdir " + path + ": " + strerror(errno));
            apply_metadata(path, e, false);
        } else if (is_lnk) {
            data.resize(e.filesize);
            if (e.filesize > 0 && !reader.read_exact(&data[0], e.filesize)) return fail("truncated symlink target");
            if (!reader.skip(data_pad)) return fail("truncated cpio data");
            unlink(path.c_str());
            if (symlink(data.c_str(), path.c_str()) != 0) return fail("symlink " + path + ": " + strerror(errno));
            apply_metadata(path, e, true);
        } else if (S_ISCHR(e.mode) || S_ISBLK(e.mode)) {
            if (!reader.skip(e.filesize + data_pad)) return fail("truncated cpio data");
            unlink(path.c_str());
            if (mknod(path.c_str(), e.mode, makedev(e.rdevmajor, e.rdevminor)) != 0) {
                VINE_LOGW("ramdisk: mknod(%s): %s (skipped)", path.c_str(), strerror(errno));
            } else {
                apply_metadata(path, e, false);
            }
        } else {
            // fifos and sockets carry no useful content in a ramdisk
            if (!reader.skip(e.filesize + data_pad)) return fail("truncated cpio data");
        }
        ++entries;
    }

    VINE_LOGI("ramdisk: extracted %zu entries into %s", entries, dest_dir.c_str());
    return true;
}

} // namespace vine
