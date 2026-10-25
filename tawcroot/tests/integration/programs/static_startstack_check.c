/* /proc/self/stat's startstack (field 28) must lie in the mapping that
 * holds the running main-thread stack: bionic sizes the main thread's
 * stack from it (pthread_getattr_np), and ART guards that range.
 *
 * Exit 42 on success, otherwise the failing step. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int vma_of(unsigned long addr, unsigned long *lo, unsigned long *hi)
{
	FILE *f = fopen("/proc/self/maps", "re");
	if (!f) return -1;
	char line[512];
	int found = -1;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%lx-%lx", lo, hi) == 2 && *lo <= addr && addr < *hi) {
			found = 0;
			break;
		}
	fclose(f);
	return found;
}

int main(void)
{
	char line[1024];
	FILE *f = fopen("/proc/self/stat", "re");
	if (!f || !fgets(line, sizeof line, f)) return 1;
	fclose(f);
	char *p = strrchr(line, ')');
	if (!p) return 2;
	unsigned long startstack = 0;
	/* Fields 3..27 skipped, 28 read. */
	if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u "
		   "%*d %*d %*d %*d %*d %*d %*u %*u %*d %*u %*u %*u %lu",
		   &startstack) != 1) return 3;
	volatile int here = 0;
	unsigned long lo1, hi1, lo2, hi2;
	if (vma_of(startstack, &lo1, &hi1)) return 4;
	if (vma_of((unsigned long)&here, &lo2, &hi2)) return 5;
	if (lo1 != lo2 || hi1 != hi2) {
		fprintf(stderr, "startstack %lx in %lx-%lx, stack in %lx-%lx\n",
			startstack, lo1, hi1, lo2, hi2);
		return 6;
	}
	return 42;
}
