#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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

    {
        struct {
            char path[64];
            int64_t magic;
        } cases[] = {
            {"/", 0xEF53},
            {"/proc", 0x9FA0},
            {"/proc/mounts", 0x9FA0},
            {"/etc", 0xEF53},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            struct {
                int64_t f_type;
                int64_t f_bsize;
                int64_t f_blocks;
                int64_t f_bfree;
                int64_t f_bavail;
                int64_t f_files;
                int64_t f_ffree;
                int32_t f_fsid[2];
                int64_t f_namelen;
                int64_t f_frsize;
                int64_t f_flags;
                int64_t f_spare[4];
            } sfs;
            memset(&sfs, 0, sizeof(sfs));
            errno = 0;
            rc = (int)lsys(SYS_statfs, (long)cases[i].path, (long)&sfs, 0, 0, 0,
                           0);
            printf("  statfs(%s)=%d magic=0x%lx\n", cases[i].path, rc,
                   (unsigned long)sfs.f_type);
            CK(rc == 0 && sfs.f_type == cases[i].magic);
            CK(sfs.f_bsize > 0 && sfs.f_namelen == 255);
        }
    }

    errno = 0;
    rc = (int)lsys(SYS_statfs, (long)"/no/such/fs", (long)val, sizeof val, 0, 0,
                   0);
    printf("  statfs(bogus)=%d errno=%d\n", rc, errno);
    CK(fail_rc(rc) && errno == ENOENT);

    errno = 0;
    rc = (int)lsys(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0,
                   0);
    printf("  mount(proc,/proc)=%d errno=%d\n", rc, errno);
    CK(rc == 0);

    errno = 0;
    rc = (int)lsys(SYS_mount, (long)"proc", (long)"/tmp", (long)"proc", 0, 0,
                   0);
    printf("  mount(proc,/tmp)=%d errno=%d\n", rc, errno);
    CK(fail_rc(rc));

    errno = 0;
    rc = (int)lsys(SYS_mount, (long)"nosuchfs", (long)"/tmp", (long)"nosuchfs",
                   0, 0, 0);
    printf("  mount(nosuchfs)=%d errno=%d\n", rc, errno);
    CK(fail_rc(rc) && (errno == ENODEV || errno == EINVAL || errno == ENOENT));

    errno = 0;
    rc = (int)lsys(SYS_umount2, (long)"/proc", 2, 0, 0, 0, 0);
    printf("  umount2(/proc,DETACH)=%d errno=%d\n", rc, errno);
    CK(rc == 0);

    errno = 0;
    rc = (int)lsys(SYS_umount2, (long)"/not/mounted", 0, 0, 0, 0, 0);
    printf("  umount2(bogus)=%d errno=%d\n", rc, errno);
    CK(fail_rc(rc) && errno == EINVAL);

    {
        errno = 0;
        rc = (int)lsys(SYS_fchmod, fd, 0640, 0, 0, 0, 0);
        printf("  fchmod=0%o -> %d errno=%d\n", 0640, rc, errno);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && (st.st_mode & 07777u) == 0640u);

        errno = 0;
        rc = (int)lsys(SYS_fchown, fd, 1000, 1000, 0, 0, 0);
        printf("  fchown(1000,1000)=%d errno=%d\n", rc, errno);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000);

        errno = 0;
        rc = (int)lsys(SYS_fchown, fd, (long)-1, (long)-1, 0, 0, 0);
        printf("  fchown(-1,-1)=%d errno=%d\n", rc, errno);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000);

        errno = 0;
        rc = (int)lsys(SYS_fchmod, fd, 0777, 0, 0, 0, 0);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && (st.st_mode & 07777u) == 0777u);

        errno = 0;
        rc = (int)lsys(SYS_fchmodat, 0, (long)path, 0600, 0, 0, 0);
        printf("  fchmodat=0%o -> %d errno=%d\n", 0600, rc, errno);
        CK(rc == 0);
        CK(stat(path, &st) == 0 && (st.st_mode & 07777u) == 0600u);

        struct timespec keep[2] = {{1234567890, 0}, {1234567890, 0}};
        errno = 0;
        rc = (int)lsys(SYS_utimensat, fd, 0, (long)keep, 0, 0, 0);
        printf("  utimensat(fd,NULL)=%d errno=%d\n", rc, errno);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && st.st_mtim.tv_sec == 1234567890);

        struct timespec now[2] = {{0, 1073741824}, {0, 1073741824}};
        errno = 0;
        rc = (int)lsys(SYS_utimensat, fd, 0, (long)now, 0, 0, 0);
        printf("  utimensat(UTIME_NOW)=%d errno=%d\n", rc, errno);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && st.st_mtim.tv_sec > 1234567890);

        struct timespec omit[2] = {{0, 1073741823}, {0, 1073741823}};
        errno = 0;
        rc = (int)lsys(SYS_utimensat, fd, 0, (long)omit, 0, 0, 0);
        printf("  utimensat(UTIME_OMIT)=%d errno=%d\n", rc, errno);
        CK(rc == 0);
        CK(fstat(fd, &st) == 0 && st.st_mtim.tv_sec > 1234567890);
    }

    if (close(fd) == 0)
        unlink(path);

    printf("compat_stub: %d/%d\n", passed, checks);
    printf("compat_stub: %s\n", passed == checks ? "PASS" : "FAIL");
    return passed == checks ? 0 : 1;
}
