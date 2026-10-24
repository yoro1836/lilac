/* faultcatch CMD...: run CMD traced (main thread only); on the first
 * SIGSEGV/SIGBUS/SIGILL/SIGABRT dump siginfo, regs, a stack slice, the
 * frame-pointer chain and /proc/pid/maps to stderr, then let the signal
 * through. arm64 only. Build: NDK clang -static -O1. */
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static void dump(pid_t pid, int sig)
{
	siginfo_t si; memset(&si, 0, sizeof si);
	ptrace(PTRACE_GETSIGINFO, pid, 0, &si);
	struct user_regs_struct r; struct iovec io = { &r, sizeof r };
	ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io);
	fprintf(stderr, "=== faultcatch pid %d sig %d code %d addr %p\n", pid, sig, si.si_code, si.si_addr);
	fprintf(stderr, "pc %016llx lr %016llx sp %016llx\n", (unsigned long long)r.pc,
		(unsigned long long)r.regs[30], (unsigned long long)r.sp);
	for (int i = 0; i < 31; i++)
		fprintf(stderr, "x%-2d %016llx%s", i, (unsigned long long)r.regs[i], i % 4 == 3 ? "\n" : "  ");
	fprintf(stderr, "\n=== stack\n");
	for (int i = 0; i < 64; i++) {
		errno = 0;
		long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(r.sp + 8 * i), 0);
		if (errno) break;
		fprintf(stderr, "%016llx%s", (unsigned long long)v, i % 4 == 3 ? "\n" : " ");
	}
	/* Frame-pointer chain: [x29] = caller's x29, [x29+8] = return address. */
	fprintf(stderr, "=== frames\n");
	unsigned long long fp = r.regs[29];
	for (int i = 0; i < 48 && fp; i++) {
		errno = 0;
		long nfp = ptrace(PTRACE_PEEKDATA, pid, (void *)fp, 0);
		long ret = ptrace(PTRACE_PEEKDATA, pid, (void *)(fp + 8), 0);
		if (errno) break;
		fprintf(stderr, "#%02d %016llx\n", i, (unsigned long long)ret & 0xffffffffffffull);
		if ((unsigned long long)nfp <= fp) break;
		fp = (unsigned long long)nfp;
	}
	char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
	FILE *f = fopen(p, "r");
	fprintf(stderr, "=== maps\n");
	if (!f) fprintf(stderr, "(%s: %s)\n", p, strerror(errno));
	if (f) { char l[512]; while (fgets(l, sizeof l, f)) fputs(l, stderr); fclose(f); }
	fprintf(stderr, "=== end\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) return 2;
	pid_t pid = fork();
	if (pid == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		execv(argv[1], argv + 1);
		_exit(127);
	}
	int st, dumped = 0;
	waitpid(pid, &st, 0);
	if (ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_EXITKILL) < 0)
		perror("faultcatch: setoptions");
	ptrace(PTRACE_CONT, pid, 0, 0);
	for (;;) {
		if (waitpid(pid, &st, __WALL) < 0) return 1;
		if (WIFEXITED(st)) return WEXITSTATUS(st);
		if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
		int sig = WSTOPSIG(st);
		if (sig == SIGTRAP) sig = 0;
		if (!dumped && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGABRT)) {
			dump(pid, sig);
			dumped = 1;
		}
		ptrace(PTRACE_CONT, pid, 0, (void *)(long)sig);
	}
}
