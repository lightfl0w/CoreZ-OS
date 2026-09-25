#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, passed;
#define CK(c)                                                                                                              \
    do {                                                                                                                   \
        checks++;                                                                                                          \
        if (c)                                                                                                             \
            passed++;                                                                                                      \
    } while (0)

int main(void) {
    struct stat st;
    char cwd[128];
    char buf[16];
    int fd = openat(AT_FDCWD, "/lib", O_RDONLY | O_DIRECTORY);

    CK(fd >= 0);
    CK(fstatat(fd, "libc.so", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
       S_ISREG(st.st_mode) && st.st_size > 0);
    CK(fstatat(fd, "../etc/passwd", &st, 0) == 0 && S_ISREG(st.st_mode));
    int f2 = openat(fd, "libc.so", O_RDONLY);
    CK(f2 >= 0 && read(f2, buf, 4) == 4 && memcmp(buf, "\177ELF", 4) == 0);
    if (f2 >= 0)
        close(f2);
    if (fd >= 0)
        close(fd);

    CK(lstat("/bin/sh", &st) == 0 && S_ISLNK(st.st_mode));
    CK(stat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);

    CK(chdir("/etc") == 0);
    CK(getcwd(cwd, sizeof cwd) != 0 && strcmp(cwd, "/etc") == 0);
    CK(stat("passwd", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    int f3 = open("shadow", O_RDONLY);
    CK(f3 >= 0 && read(f3, buf, 8) == 8);
    if (f3 >= 0)
        close(f3);
    int f4 = openat(AT_FDCWD, "passwd", O_RDONLY);
    CK(f4 >= 0);
    if (f4 >= 0)
        close(f4);

    printf("at_probe: %s %d/%d\n", passed == checks ? "PASS" : "FAIL", passed,
           checks);
    return passed == checks ? 0 : 1;
}
