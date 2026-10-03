#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int checks, passed;

#define CK(c)                                                                                                              \
    do {                                                                                                                   \
        checks++;                                                                                                          \
        if (c)                                                                                                             \
            passed++;                                                                                                      \
        else                                                                                                               \
            printf("  compat_stub fail@%d\n", __LINE__);                                                                      \
    } while (0)

static long lsys(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    if (r < 0 && r > -4096)
        errno = (int)-r;
    return r;
}

#define SYS_utimensat 280
#define SYS_fchdir 81
#define SYS_memfd_create 319
#define SYS_getxattr 191
#define SYS_lgetxattr 192
#define SYS_fgetxattr 193
#define SYS_setxattr 188
#define SYS_lsetxattr 189
#define SYS_fsetxattr 190
#define SYS_removexattr 197
#define SYS_sync 162
#define SYS_msync 26

static int fail_rc(int rc) {
    return rc < 0 && rc > -4096;
}

static int unsupported_ok(int rc) {
    if (!fail_rc(rc))
        return 0;
    return errno == ENOTSUP || errno == ENODATA || errno == EPERM ||
           errno == EACCES || errno == EINVAL;
}

static int nodata_ok(int rc) {
    if (!fail_rc(rc))
        return 0;
    return errno == ENOTSUP || errno == ENODATA || errno == EPERM ||
           errno == EACCES;
}

int main(void) {
    struct stat st;
    char cwd[128];
    char val[64];
    char nbuf[64];
    const char *path = "/tmp/compat_stubf";
    int rc;

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CK(fd >= 0);
    if (fd < 0) {
        printf("compat_stub: 0/%d\n", checks);
        return 1;
    }
    CK(write(fd, "hello", 5) == 5);

    errno = 0;
    rc = (int)lsys(285, fd, 0, 0, 4096, 0, 0);
    printf("  fallocate=%d errno=%d\n", rc, errno);
    CK(rc == 0);
    CK(fstat(fd, &st) == 0 && st.st_size == 5);

    int pf = posix_fallocate(fd, 0, 4096);
    printf("  posix_fallocate=%d\n", pf);
    CK(pf == 0);

    struct timespec ts[2] = {{1000000000, 0}, {1000000000, 0}};
    errno = 0;
    rc = (int)lsys(SYS_utimensat, fd, 0, (long)ts, 0, 0, 0);
    printf("  utimensat(fd,NULL)=%d errno=%d\n", rc, errno);
    CK(rc == 0);
    CK(fstat(fd, &st) == 0 && st.st_mtim.tv_sec == 1000000000);

    errno = 0;
    int frc = futimens(fd, ts);
    printf("  futimens=%d\n", frc);
    CK(frc == 0);
    CK(fstat(fd, &st) == 0 && st.st_mtim.tv_sec == 1000000000);

    {
        int dfd = open("/etc", O_RDONLY | O_DIRECTORY);
        CK(dfd >= 0);
        errno = 0;
        rc = (int)lsys(SYS_fchdir, dfd, 0, 0, 0, 0, 0);
        printf("  fchdir=%d errno=%d\n", rc, errno);
        CK(rc == 0);
        if (rc == 0) {
            CK(getcwd(cwd, sizeof cwd) != 0 && strcmp(cwd, "/etc") == 0);
            chdir("/");
        }
        if (dfd >= 0)
            close(dfd);
    }

    errno = 0;
    rc = (int)lsys(SYS_memfd_create, (long)"/dev/shm/none", 0, 0, 0, 0, 0);
    printf("  memfd_create=%d errno=%d\n", rc, errno);
    CK(unsupported_ok(rc));
    if (rc >= 0)
        close(rc);

    errno = 0;
    rc = (int)lsys(SYS_getxattr, (long)"/etc/passwd", (long)val, sizeof val,
                   (long)nbuf, sizeof nbuf, 0);
    printf("  getxattr=%d errno=%d\n", rc, errno);
    CK(nodata_ok(rc));

    errno = 0;
    rc = (int)lsys(SYS_lgetxattr, (long)"/bin/sh", (long)val, sizeof val,
                   (long)nbuf, sizeof nbuf, 0);
    printf("  lgetxattr=%d errno=%d\n", rc, errno);
    CK(nodata_ok(rc));

    errno = 0;
    rc = (int)lsys(SYS_fgetxattr, fd, (long)val, sizeof val, 0, 0, 0);
    printf("  fgetxattr=%d errno=%d\n", rc, errno);
    CK(nodata_ok(rc));

    errno = 0;
    rc = (int)lsys(SYS_setxattr, (long)"/etc/passwd", (long)"user.compat_stub", 16,
                   (long)val, 1, 0);
    printf("  setxattr=%d errno=%d\n", rc, errno);
    CK(unsupported_ok(rc));

    errno = 0;
    rc = (int)lsys(SYS_lsetxattr, (long)"/bin/sh", (long)"user.compat_stub", 16,
                   (long)val, 1, 0);
    printf("  lsetxattr=%d errno=%d\n", rc, errno);
    CK(unsupported_ok(rc));

    errno = 0;
    rc = (int)lsys(SYS_fsetxattr, fd, (long)"user.compat_stub", 16, (long)val, 1,
                   0);
    printf("  fsetxattr=%d errno=%d\n", rc, errno);
    CK(unsupported_ok(rc));

    errno = 0;
    rc = (int)lsys(SYS_removexattr, (long)"/etc/passwd", (long)"user.compat_stub",
                   16, 0, 0, 0);
    printf("  removexattr=%d errno=%d\n", rc, errno);
    CK(nodata_ok(rc));

    errno = 0;
    lsys(SYS_sync, 0, 0, 0, 0, 0, 0);
    CK(errno == 0);
    errno = 0;
    lsys(SYS_msync, 0, 0, 0, 0, 0, 0);
    CK(errno == 0);

    CK(fstat(fd, &st) == 0 && st.st_size == 5);
    if (close(fd) == 0)
        unlink(path);

    printf("compat_stub: %d/%d\n", passed, checks);
    printf("compat_stub: %s\n", passed == checks ? "PASS" : "FAIL");
    return passed == checks ? 0 : 1;
}
