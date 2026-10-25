/* Handler-layer tests: foundation trap/bootstrap smoke.
 *
 * Forks `tawcroot-testhost` with no args once and registers one cleat
 * test per `[ok ]` / `[FAIL]` line in its output. The testhost installs
 * NNP, the SIGSYS handler, and the seccomp filter, runs the trap-contract
 * assertion (stub-IP allow vs inline-asm trap), exercises every raw
 * syscall the handler/bootstrap will use, sets up a state-fd memfd, and
 * `execveat`s into itself with `--exec-child`. The child reinstalls the
 * handler, reads the inherited state-fd, re-runs the trap contract, and
 * verifies pid is preserved across exec.
 *
 * Each individual check inside testhost (~30+) becomes its own cleat
 * test. Failure attribution is per-check, and `tawcroot/test.sh
 * '.*PR_SET_NO_NEW_PRIVS.*'` runs just that one. See tawcroot/tests/handler/steps.h.
 */

#include <cleat/test.h>

#include "steps.h"

register_dynamic_tests
{
	steps_register_from_testhost(c_sv("foundation_smoke"), NULL);
}
