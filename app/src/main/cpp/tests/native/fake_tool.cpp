// Tiny program placed in the fake system image to check that guest_exec really runs inside the guest.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    char buf[256] = {0};
    int fd = open("/system/marker.txt", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) n = 0;
        buf[n] = 0;
        close(fd);
    }
    for (char* p = buf; *p; ++p) if (*p == '\n') *p = 0;
    printf("tool pid=%d marker=%s args=", (int)getpid(), buf);
    for (int i = 1; i < argc; ++i) printf("%s%s", i > 1 ? "," : "", argv[i]);
    printf(" ANDROID_ROOT=%s\n", getenv("ANDROID_ROOT") ? getenv("ANDROID_ROOT") : "(unset)");
    if (argc > 2 && strcmp(argv[1], "exit") == 0) return atoi(argv[2]);
    if (argc > 1 && strcmp(argv[1], "sleep") == 0) sleep(30);
    return 0;
}
