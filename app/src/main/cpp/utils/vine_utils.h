#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace vine {

bool mkdirs(const std::string& path, mode_t mode = 0755);
bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path);

// Returns nullopt on failure.
std::optional<std::string> read_file(const std::string& path);

// Reads at most max_bytes from the start of a file; nullopt if it can't be opened.
std::optional<std::string> read_file_prefix(const std::string& path, size_t max_bytes);

// Creates or truncates the file and writes all of contents (handles short writes).
bool write_file(const std::string& path, const std::string& contents, mode_t mode = 0644);

// Writes to a temp file next to path, then renames over it, so readers never see a half-written file.
bool write_file_atomic(const std::string& path, const std::string& contents, mode_t mode = 0644);

bool append_file(const std::string& path, const std::string& contents);

// Returns -1 on error.
int64_t file_size(const std::string& path);

// Creates (or extends) a sparse file of exactly size_bytes without allocating blocks.
bool create_sparse_file(const std::string& path, uint64_t size_bytes);

// Sparse aware copy that keeps holes, plain copy when the filesystem cannot report them
bool copy_file_sparse(const std::string& src, const std::string& dst,
                      const std::function<void(uint64_t)>& progress = nullptr);

// Deletes a file or directory tree without following symlinks and without crossing into other filesystems.
bool remove_recursive(const std::string& path);

// Empties a directory but keeps the directory itself.
bool clear_directory(const std::string& path);

// Execute and wait. Returns exit code or -1 on fork/exec failure.
int exec_wait(const std::vector<std::string>& args);

// Fork and exec without waiting. Returns child PID or -1 on failure.
pid_t exec_async(const std::vector<std::string>& args);

// Runs a program with captured output and a timeout, returns its exit code or -1
int exec_capture(const std::vector<std::string>& args, std::string* output,
                 int timeout_ms = 30000, size_t max_bytes = 262144);

// Sends SIGTERM, then SIGKILL after timeout_ms if still alive; returns true if the process exited on SIGTERM
bool terminate_gracefully(pid_t pid, int timeout_ms = 5000);

// True if the host CPU supports AArch32 execution state; checks abilist32, abilist, /proc/sys/abi/*, and cpuinfo, false on the arm64-only SoCs common across many vendors
bool host_supports_aarch32();

// Returns ABIs from ro.product.cpu.abilist.
std::vector<std::string> host_abi_list();

// Returns available RAM in megabytes.
long available_ram_mb();

// True if the running kernel lists the filesystem in /proc/filesystems (e.g. "binder" for binderfs).
bool kernel_has_filesystem(const std::string& name);

// Looks for an executable by name in $PATH and the usual Android and Linux tool directories; "" if not found.
std::string find_executable(const std::string& name);

// Splits text into lines, dropping the trailing newline and empty last line.
std::vector<std::string> split_lines(const std::string& text);

// Escapes newline, carriage return, tab and backslash so any string fits in one tab-separated protocol field, and the inverse.
std::string escape_line(const std::string& text);
std::string unescape_line(const std::string& text);

// Splits on a single-character separator, keeping empty fields.
std::vector<std::string> split_fields(const std::string& text, char sep);

// Joins path components with exactly one slash between them.
std::string path_join(const std::string& a, const std::string& b);

// True if path (after lexical normalisation, no symlink resolution) is inside root.
bool path_is_within(const std::string& path, const std::string& root);

// Simple monotonic clock helpers.
int64_t monotonic_ms();

} // namespace vine
