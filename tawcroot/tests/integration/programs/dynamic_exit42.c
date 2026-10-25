/* Minimal dynamically linked guest fixture.
 *
 * Built with the system glibc: ET_DYN with PT_INTERP =
 * "/lib64/ld-linux-x86-64.so.2" (or wherever the host's ld.so lives),
 * DT_NEEDED libc.so.6. Used by tests/unit/test_loader_smoke_dynamic.c
 * to validate that our loader correctly:
 *
 *   - maps the binary as ET_DYN (reservation + MAP_FIXED segments)
 *   - maps ld.so as a separate ET_DYN
 *   - synthesizes a stack with AT_BASE = ld.so's load addr,
 *     AT_ENTRY = the binary's e_entry, AT_PHDR = binary's phdrs
 *   - jumps to ld.so's entry
 *
 * ld.so then does dynamic relocation, loads libc, runs C init, and
 * eventually calls our `main`, which returns 42. exit(42) is not
 * called explicitly — glibc's _start path turns `main`'s return
 * value into exit_group(rv).
 *
 * Returning 42 from main is the strongest "dynamic linking actually
 * worked" signal we can get without printf output capture: it proves
 * ld.so found libc, relocated everything, ran __libc_start_main,
 * and called us.
 */

int main(void)
{
	return 42;
}
