#pragma once

#include <string>

namespace vine {

// Extracts a gzip or raw cpio newc ramdisk into dest_dir, rejecting entries that escape it
bool extract_ramdisk(const std::string& archive_path, const std::string& dest_dir, std::string* error);

// Names the compression of a file from its magic bytes ("gzip", "lz4", "xz", "bzip2", "zstd", "cpio", "unknown").
std::string detect_archive_format(const std::string& path);

} // namespace vine
