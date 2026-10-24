/* Rootfs syscall smoke driver -- testhost entry for `-r <rootfs>`.
 *
 * Sets up dispatch table + filter, then runs a small set of inline-asm
 * syscall probes against an actual rootfs tree on disk. Lets us
 * validate the dispatch + path-translation contract without yet having
 * a manual ELF loader to launch a real guest.
 *
 * Returns 0 on success / 1 on any failure (number of failures printed).
 */

#pragma once

int tawcroot_rootfs_smoke_main(const char *rootfs);

/* argv-aware variant: also collects `-b src:dst` entries before running
 * the smoke. */
int tawcroot_rootfs_smoke_main_argv(int argc, char **argv, const char *rootfs);
