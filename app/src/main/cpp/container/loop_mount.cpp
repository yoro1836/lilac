#include "loop_mount.h"
#include "../utils/vine_log.h"
#include "../utils/vine_utils.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace vine {

namespace {

// Android keeps loop nodes in /dev/block, desktop Linux in /dev; create one if neither exists yet
std::string loop_node(int index) {
    const std::string a = "/dev/loop" + std::to_string(index);
    const std::string b = "/dev/block/loop" + std::to_string(index);
    if (path_exists(a)) return a;
    if (path_exists(b)) return b;
    if (mknod(a.c_str(), S_IFBLK | 0600, makedev(7, (unsigned)index)) == 0) return a;
    if (mknod(b.c_str(), S_IFBLK | 0600, makedev(7, (unsigned)index)) == 0) return b;
    return "";
}

struct FsCandidate {
    const char* type;
    const char* options;
};

} // namespace

bool mount_image(const std::string& image, const std::string& target, bool read_only, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        VINE_LOGE("mount_image(%s): %s", image.c_str(), msg.c_str());
        return false;
    };

    int img_fd = open(image.c_str(), (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (img_fd < 0 && !read_only && (errno == EACCES || errno == EROFS)) {
        read_only = true; // read-only backing file: mount it read-only instead of failing
        img_fd = open(image.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (img_fd < 0) return fail("cannot open " + image + ": " + strerror(errno));

    int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        close(img_fd);
        return fail(std::string("cannot open /dev/loop-control: ") + strerror(errno) + " (kernel without loop device support?)");
    }

    int loop_fd = -1;
    std::string node;
    for (int attempt = 0; attempt < 8 && loop_fd < 0; ++attempt) {
        int index = ioctl(ctl, LOOP_CTL_GET_FREE);
        if (index < 0) break;
        node = loop_node(index);
        if (node.empty()) continue;
        int fd = open(node.c_str(), (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, LOOP_SET_FD, img_fd) == 0) loop_fd = fd; // EBUSY here means another process took it first
        else close(fd);
    }
    close(ctl);
    close(img_fd);
    if (loop_fd < 0) return fail("no free loop device");

    struct loop_info64 li{};
    li.lo_flags = LO_FLAGS_AUTOCLEAR | (read_only ? LO_FLAGS_READ_ONLY : 0);
    strncpy(reinterpret_cast<char*>(li.lo_file_name), image.c_str(), LO_NAME_SIZE - 1);
    ioctl(loop_fd, LOOP_SET_STATUS64, &li);

    // The fd must stay open until the mount holds its own reference, or autoclear would detach the device first
    static const FsCandidate kCandidates[] = {
        {"ext4", nullptr}, {"erofs", nullptr}, {"squashfs", nullptr}, {"f2fs", nullptr},
    };
    std::string last_error = "unknown filesystem";
    const unsigned long flags = (read_only ? (unsigned long)MS_RDONLY : 0UL) | (unsigned long)MS_NOATIME;
    for (const auto& c : kCandidates) {
        if (mount(node.c_str(), target.c_str(), c.type, flags, c.options) == 0) {
            close(loop_fd);
            VINE_LOGI("mounted %s (%s) on %s via %s%s", image.c_str(), c.type, target.c_str(), node.c_str(),
                      read_only ? " [ro]" : "");
            return true;
        }
        last_error = std::string(c.type) + ": " + strerror(errno);
    }

    ioctl(loop_fd, LOOP_CLR_FD);
    close(loop_fd);
    return fail("mount " + image + " on " + target + " failed (" + last_error + ")");
}

} // namespace vine
