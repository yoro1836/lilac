/* Helper: turn the existing in-binary `tawc_io_step` output into one cleat
 * test per [ok ]/[FAIL] line.
 *
 * The testhost binary already prints `  [ok ] LABEL` / `  [FAIL] LABEL` for
 * each individual check (~80+ across the foundation + rootfs smokes). Re-running the
 * testhost per check would be silly; instead each test file's
 * `register_dynamic_tests` block runs the testhost ONCE, parses the output,
 * and registers one cleat test per parsed step under its module.
 *
 * Each registered test:
 *   - PASS if its line was `[ok ]`
 *   - FAIL with the surrounding kv-context lines if it was `[FAIL]`
 *
 * If the testhost itself fails to exec or exits with a status mismatch
 * (process-level), an extra problem-test under that module surfaces it
 * as a regular failure in the report.
 *
 * cleat's test_module_from_file() doesn't apply here -- we pass `module`
 * explicitly. By convention, use the file's basename without `test_` /
 * `.c` (e.g. `foundation_smoke` for
 * `tests/handler/test_foundation_smoke.c`).
 */

#pragma once

#include <stc/csview.h>

/* Builds an argv `{TAWCROOT_TESTHOST_BIN, extra_args[0], extra_args[1], ...}`
 * (extra_args is NULL-terminated; pass NULL for no extras), forks the
 * testhost, captures stdout/stderr/exit, parses every `[ok ]` and `[FAIL]`
 * line, and registers cleat tests under `module`. Each label produces one
 * test named after the line (prefix preserved so e.g. `[parent]`/`[child]`
 * variants are distinct).
 *
 * Call from `register_dynamic_tests`.
 */
void steps_register_from_testhost(csview module, const char *const *extra_args);

/* Same as steps_register_from_testhost, but the launched argv is
 * prefixed with `prefix_argv` (NULL-terminated). The first element
 * becomes argv[0] (i.e. the actual executable forked); the testhost
 * binary path follows. Used by the synthesized-Android-filter suite to
 * route the testhost through a seccomp prefilter wrapper.
 */
void steps_register_from_testhost_prefixed(csview module,
                                           const char *const *prefix_argv,
                                           const char *const *extra_args);
