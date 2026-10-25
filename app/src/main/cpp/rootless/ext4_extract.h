#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Read-only ext4 extraction for the rootless engine: unpacks a ROM's
// system/vendor image into a directory without loop mounts or root.
// Regular files (sparse), directories, symlinks and hard links, with
// mode and times. Device nodes, fifos and sockets are skipped (images
// do not ship them). Non-root owners go into the tawcroot ownership
// table at owners_path (created if missing), keyed by the extracted
// files' own st_dev/st_ino; NULL skips ownership.
//
// Returns 0, or -1 with a message in err.
int vine_ext4_extract(const char *image, const char *dest, const char *owners_path,
                      char *err, unsigned long err_len);

#ifdef __cplusplus
}
#endif
