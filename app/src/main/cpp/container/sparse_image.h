#pragma once

#include <string>

namespace vine {

// True if the file starts with the Android sparse-image magic (what `fastboot` flashes and `simg2img` reads).
bool is_android_sparse_image(const std::string& path);

// Expands an Android sparse image into a raw one, DONT_CARE ranges stay holes
bool sparse_image_to_raw(const std::string& src, const std::string& dst, std::string* error);

} // namespace vine
