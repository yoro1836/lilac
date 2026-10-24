/* Dynamic fixture that validates the full argc/argv/envp pipeline
 * survives through ld.so → __libc_start_main → main.
 *
 * Run with argv = ["dynamic_argv_check", "alpha", "beta"] and envp
 * containing "TAWC_TEST=ok". Exit 0 on full success; non-zero
 * encodes which step failed (mirrors static_argc_random.S).
 *
 *   60   argc != 3
 *   61   argv[0] doesn't equal expected first arg
 *   62   argv[1] != "alpha"
 *   63   argv[2] != "beta"
 *   64   envp didn't contain TAWC_TEST=ok
 *   42   success — chosen so it differs from any libc abort code
 *        (which would be 134/SIGABRT) and from common conventions.
 */

#include <stdlib.h>
#include <string.h>

extern char **environ;

int main(int argc, char **argv)
{
	if (argc != 3) return 60;
	if (!argv[0] || !strstr(argv[0], "dynamic_argv_check")) return 61;
	if (!argv[1] || strcmp(argv[1], "alpha") != 0) return 62;
	if (!argv[2] || strcmp(argv[2], "beta")  != 0) return 63;

	int found = 0;
	for (char **e = environ; *e; e++) {
		if (strcmp(*e, "TAWC_TEST=ok") == 0) { found = 1; break; }
	}
	if (!found) return 64;

	return 42;
}
