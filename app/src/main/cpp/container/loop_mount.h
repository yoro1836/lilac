#pragma once

#include <string>

namespace vine {

// Mounts an image on target through an autoclear loop device, tries ext4, erofs, squashfs, f2fs
bool mount_image(const std::string& image, const std::string& target, bool read_only, std::string* error);

} // namespace vine
