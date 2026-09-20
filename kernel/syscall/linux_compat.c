#include "kernel/syscall/linux_compat.h"
#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/rtc.h"
#include "drivers/char/tty.h"
#include "drivers/net/socket.h"
#include "kernel/asm_func.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/proc.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/shell/pipe.h"
#include "kernel/signal.h"
#include "kernel/syscall/file_syscall.h"
#include "kernel/syscall/futex.h"
#include "kernel/syscall/mmap.h"
#include "kernel/userprog/clone.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/rand/rand.h"
#include "lib/str/str.h"
#include "libc/user/syscall.h"

uint32_t sys_brk(uint32_t addr);
int32_t sys_clock_gettime(int32_t clk_id, struct SYS_TIMESPEC *tp);
int32_t sys_gettimeofday(struct SYS_TIMEVAL *tv, void *tz);
int32_t sys_nanosleep(const struct SYS_TIMESPEC *req, struct SYS_TIMESPEC *rem);
int32_t sys_wait(int32_t *status);
int32_t sys_sigaction(int sig, const struct SYS_SIGACTION *act,
                      struct SYS_SIGACTION *old);
int32_t sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
uint64_t sys_sigreturn(struct X86_REGS *r);

static int32_t compat_read(int32_t fd, void *buf, uint32_t count);
static int32_t compat_write(int32_t fd, const void *buf, uint32_t count);
static int evfd_slot(int fd);
static int tfd_slot(int fd);
static int ep_slot(int fd);
static int io_is_file_fd(int fd);
static int64_t lc_eventfd_read(int i, void *buf, uint32_t count);
static int64_t lc_eventfd_write(int i, const void *buf, uint32_t count);
static int64_t lc_timerfd_read(int i, void *buf, uint32_t count);
static uint8_t *lc_nonblock_slot(int fd);
static int lc_close_extra(int32_t fd);

#define DIRF_FLAG 0xFFFEu

static int32_t compat_dir_fd(const char *path) {
    uint32_t ino = 0;
    int is_dir = 0;
    if (ext2_lookup(path, &ino, &is_dir) || !is_dir)
        return -1;
    int gfd = file_table_alloc_slot();
    if (gfd < 0)
        return -1;
    struct FILE *f = file_get((uint32_t)gfd);
    f->fd_inode = inode_open(cur_part, ino);
    if (f->fd_inode == NULL) {
        file_table_free_slot(gfd);
        return -1;
    }
    f->fd_pos = 0;
    f->fd_flag = DIRF_FLAG;
    f->proc_id = 0;
    f->ref_cnt = 1;
    int32_t fd = fd_install(gfd);
    if (fd < 0) {
        inode_close(f->fd_inode);
        file_table_free_slot(gfd);
        return -1;
    }
    return fd;
}

static int compat_fd_isdir(int32_t fd) {
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return 0;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    if (gfd >= MAX_FILE_OPEN)
        return 0;
    struct FILE *pf = file_get(gfd);
    return pf != NULL && pf->fd_flag == DIRF_FLAG;
}

static int32_t compat_getdents64(int32_t fd, void *dirp, uint32_t count) {
    if (dirp == NULL || fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    if (!compat_fd_isdir(fd))
        return -LINUX_ENOTDIR;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    uint32_t pos = pf->fd_pos;
    uint32_t emitted = pos;
    uint32_t written = 0;
    struct FS_DIRENT de;
    for (;;) {
        if (ext2_dir_next(pf->fd_inode, &pos, &de) != 0)
            break;
        uint32_t nl = strlen(de.filename);
        uint16_t reclen = (uint16_t)((19u + nl + 1u + 7u) & ~7u);
        if (written + reclen > count) {
            if (written == 0)
                return -LINUX_EINVAL;
            break;
        }
        struct LINUX_DIRENT64 *d =
            (struct LINUX_DIRENT64 *)((uint8_t *)dirp + written);
        d->d_ino = de.i_no;
        d->d_off = (int64_t)pos;
        d->d_reclen = reclen;
        d->d_type = de.f_type == FT_DIRECTORY    ? LINUX_DT_DIR
                    : de.f_type == FT_CHARDEVICE ? LINUX_DT_CHR
                    : de.f_type == FT_SYMLINK    ? LINUX_DT_LNK
                                                 : LINUX_DT_REG;
        memcpy(d->d_name, de.filename, nl + 1);
        written += reclen;
        emitted = pos;
    }
    pf->fd_pos = emitted;
    return (int32_t)written;
}

static int compat_fd_is_tty(int32_t fd) {
    if (fd >= 0 && fd <= 2)
        return 1;
    if (fd < 0)
        return 0;
    struct FILE *f = file_get(fd_local2global((uint32_t)fd));
    return f != NULL && f->fd_inode != NULL && fs_is_chardev(f->fd_inode) &&
           (fs_chardev_dev(f->fd_inode) >> 8) == 5u;
}

static void compat_tcgets(uint8_t *p) {
    TTY.ioctl(TTY_IOCTL_TCGETS, (uint64_t)(uintptr_t)p);
}

static int32_t compat_tcsets(uint32_t cmd, uint64_t arg) {
    (void)cmd;
    if (!arg || !access_ok((const void *)(uintptr_t)arg, 60, 0))
        return -LINUX_EFAULT;
    TTY.ioctl(TTY_IOCTL_TCSETS, arg);
    return 0;
}

static int32_t compat_ioctl(int32_t fd, uint32_t cmd, uint64_t arg) {
    if (fd >= 0 && compat_fd_is_tty(fd)) {
        switch (cmd) {
        case LINUX_TCGETS:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 60, 1))
                return -LINUX_EFAULT;
            compat_tcgets((uint8_t *)(uintptr_t)arg);
            return 0;
        case LINUX_TCSETS:
        case LINUX_TCSETSW:
        case LINUX_TCSETSF:
            return compat_tcsets(cmd, arg);
        case LINUX_TIOCGWINSZ:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 8, 1))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCGWINSZ, arg) < 0 ? -LINUX_ENOTTY
                                                            : 0;
        case LINUX_TIOCSWINSZ:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 8, 0))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCSWINSZ, arg) < 0 ? -LINUX_ENOTTY
                                                            : 0;
        case LINUX_TIOCGPGRP:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 4, 1))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCGPGRP, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_TIOCSPGRP:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 4, 0))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCSPGRP, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_FIONREAD:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 4, 1))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_FIONREAD, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_TCFLSH:
            return 0;
        default:
            return -LINUX_ENOTTY;
        }
    }
    return -LINUX_ENOTTY;
}

static int32_t compat_setpgid(uint32_t pid, uint32_t pgid) {
    if (pid >= MAX_TASKS || pgid >= MAX_TASKS)
        return -LINUX_EINVAL;
    struct TASK *t = pid2thread((int32_t)pid);
    if (t == NULL || t->status == TASK_DIED)
        return -LINUX_ESRCH;
    uint32_t want = pgid ? pgid : pid;
    if (want >= MAX_TASKS)
        return -LINUX_EINVAL;
    t->pgid = want;
    return 0;
}

static int32_t compat_getpgid(uint32_t pid) {
    if (pid >= MAX_TASKS)
        return -LINUX_EINVAL;
    struct TASK *t = pid2thread((int32_t)pid);
    if (t == NULL)
        return -LINUX_ESRCH;
    return (int32_t)(t->pgid ? t->pgid : t->pid);
}

static int user_ptr_ok(struct X86_REGS *r, uint64_t addr, uint32_t len,
                       int writable);
static int copy_user_str(struct X86_REGS *r, char *dst, uint64_t user_ptr);
static void ticks_to_timeval(struct LINUX_TIMEVAL *tv, uint32_t ticks) {
    uint64_t us = (uint64_t)ticks * (1000000ull / PIT_HZ);
    tv->tv_sec = (int64_t)(us / 1000000ull);
    tv->tv_usec = (int64_t)(us % 1000000ull);
}

static uint32_t timeval_to_ticks(const struct LINUX_TIMEVAL *tv) {
    uint64_t us = (uint64_t)tv->tv_sec * 1000000ull +
                  (uint64_t)tv->tv_usec;
    uint64_t tick_us = 1000000ull / PIT_HZ;
    return (uint32_t)((us + tick_us - 1) / tick_us);
}

static int32_t compat_setitimer(uint32_t which, uint64_t new_val,
                                uint64_t old_val) {
    if (which != LINUX_ITIMER_REAL)
        return -LINUX_EINVAL;
    struct TASK *cur = current;
    if (old_val) {
        struct LINUX_ITIMERVAL o;
        memset(&o, 0, sizeof(o));
        if (cur->itimer_expire) {
            uint32_t rem = cur->itimer_expire > tick
                               ? cur->itimer_expire - tick
                               : 0;
            ticks_to_timeval(&o.it_value, rem);
            ticks_to_timeval(&o.it_interval,
                             (uint32_t)cur->itimer_interval);
        }
        memcpy((void *)(uintptr_t)old_val, &o, sizeof(o));
    }
    if (new_val) {
        struct LINUX_ITIMERVAL n;
        memcpy(&n, (const void *)(uintptr_t)new_val, sizeof(n));
        cur->itimer_interval = timeval_to_ticks(&n.it_interval);
        uint32_t v = timeval_to_ticks(&n.it_value);
        cur->itimer_expire = v ? tick + v : 0;
        if (cur->itimer_interval && !v)
            cur->itimer_expire = 0;
    } else {
        cur->itimer_expire = 0;
        cur->itimer_interval = 0;
    }
    return 0;
}

static int32_t compat_getitimer(uint32_t which, uint64_t cur_val) {
    if (which != LINUX_ITIMER_REAL || !cur_val)
        return which != LINUX_ITIMER_REAL ? -LINUX_EINVAL : 0;
    struct TASK *cur = current;
    struct LINUX_ITIMERVAL o;
    memset(&o, 0, sizeof(o));
    if (cur->itimer_expire) {
        uint32_t rem = cur->itimer_expire > tick ? cur->itimer_expire - tick
                                                 : 0;
        ticks_to_timeval(&o.it_value, rem);
        ticks_to_timeval(&o.it_interval, (uint32_t)cur->itimer_interval);
    }
    memcpy((void *)(uintptr_t)cur_val, &o, sizeof(o));
    return 0;
}

static int32_t compat_statfs_fill(uint64_t buf) {
    struct LINUX_STATFS sf;
    uint32_t bsize, blocks, bfree, files, ffree;
    ext2_statfs_info(&bsize, &blocks, &bfree, &files, &ffree);
    memset(&sf, 0, sizeof(sf));
    sf.f_type = (int64_t)LINUX_EXT2_SUPER_MAGIC;
    sf.f_bsize = bsize;
    sf.f_blocks = blocks;
    sf.f_bfree = bfree;
    sf.f_bavail = bfree;
    sf.f_files = files;
    sf.f_ffree = ffree;
    sf.f_namelen = 255;
    sf.f_frsize = bsize;
    memcpy((void *)(uintptr_t)buf, &sf, sizeof(sf));
    return 0;
}

static int64_t lc_setitimer(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_ITIMERVAL), 0))
        return -LINUX_EFAULT;
    if (c && !user_ptr_ok(r, c, sizeof(struct LINUX_ITIMERVAL), 1))
        return -LINUX_EFAULT;
    return compat_setitimer((uint32_t)a, b, c);
}

static int64_t lc_getitimer(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_ITIMERVAL), 1))
        return -LINUX_EFAULT;
    return compat_getitimer((uint32_t)a, b);
}

static int64_t lc_statfs(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    if (strcmp(kpath, "/") != 0 && ext2_lookup(kpath, &(uint32_t){0}, &(int){0}))
        return -LINUX_ENOENT;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STATFS), 1))
        return -LINUX_EFAULT;
    return compat_statfs_fill(b);
}

static int64_t lc_fstatfs(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STATFS), 1))
        return -LINUX_EFAULT;
    return compat_statfs_fill(b);
}

static int64_t lc_getrusage(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_RUSAGE), 1))
        return -LINUX_EFAULT;
    struct LINUX_RUSAGE ru;
    memset(&ru, 0, sizeof(ru));
    if ((int32_t)a == LINUX_RUSAGE_SELF)
        ticks_to_timeval(&ru.ru_utime, current->elapsed_ticks);
    memcpy((void *)(uintptr_t)b, &ru, sizeof(ru));
    return 0;
}

static int64_t lc_getsid(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    struct TASK *t = a ? pid2thread((int32_t)a) : current;
    if (t == NULL)
        return -LINUX_ESRCH;
    return (int64_t)(t->sid ? t->sid : t->pid);
}

static int64_t lc_umask(struct X86_REGS *r, uint64_t a, uint64_t b,
                        uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    struct TASK *cur = current;
    uint32_t old = cur->umask;
    cur->umask = (uint32_t)a & 0o7777u;
    return (int64_t)old;
}

#define LC_GETID(name, field)                                                \
    static int64_t name(struct X86_REGS *r, uint64_t a, uint64_t b,         \
                        uint64_t c, uint64_t d, uint64_t e, uint64_t f) {    \
        (void)r; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;       \
        return (int64_t)current->field;                                      \
    }
LC_GETID(lc_getuid, uid)
LC_GETID(lc_getgid, gid)
LC_GETID(lc_geteuid, euid)
LC_GETID(lc_getegid, egid)
static int64_t lc_setuid(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t v = (uint32_t)a;
    if (cur->euid == 0) {
        cur->uid = cur->euid = cur->suid = v;
        return 0;
    }
    if (v == cur->uid || v == cur->suid) {
        cur->euid = v;
        return 0;
    }
    return -LINUX_EPERM;
}

static int64_t lc_setgid(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t v = (uint32_t)a;
    if (cur->egid == 0) {
        cur->gid = cur->egid = cur->sgid = v;
        return 0;
    }
    if (v == cur->gid || v == cur->sgid) {
        cur->egid = v;
        return 0;
    }
    return -LINUX_EPERM;
}

static int64_t lc_setreuid(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t ru = (a == (uint64_t)-1) ? cur->uid : (uint32_t)a;
    uint32_t eu = (b == (uint64_t)-1) ? cur->euid : (uint32_t)b;
    if (cur->euid != 0 && ((ru != cur->uid && ru != cur->suid) ||
                           (eu != cur->uid && eu != cur->euid &&
                            eu != cur->suid)))
        return -LINUX_EPERM;
    cur->uid = ru;
    cur->euid = eu;
    if (cur->euid != 0 || (b != (uint64_t)-1 && eu != cur->uid))
        cur->suid = eu;
    return 0;
}

static int64_t lc_setregid(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t rg = (a == (uint64_t)-1) ? cur->gid : (uint32_t)a;
    uint32_t eg = (b == (uint64_t)-1) ? cur->egid : (uint32_t)b;
    if (cur->egid != 0 && ((rg != cur->gid && rg != cur->sgid) ||
                           (eg != cur->gid && eg != cur->egid &&
                            eg != cur->sgid)))
        return -LINUX_EPERM;
    cur->gid = rg;
    cur->egid = eg;
    if (cur->egid != 0 || (b != (uint64_t)-1 && eg != cur->gid))
        cur->sgid = eg;
    return 0;
}

static int64_t lc_setresuid(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t ru = (a == (uint64_t)-1) ? cur->uid : (uint32_t)a;
    uint32_t eu = (b == (uint64_t)-1) ? cur->euid : (uint32_t)b;
    uint32_t su = (c == (uint64_t)-1) ? cur->suid : (uint32_t)c;
    if (cur->euid != 0 &&
        ((ru != cur->uid && ru != cur->euid && ru != cur->suid) ||
         (eu != cur->uid && eu != cur->euid && eu != cur->suid) ||
         (su != cur->uid && su != cur->euid && su != cur->suid)))
        return -LINUX_EPERM;
    cur->uid = ru;
    cur->euid = eu;
    cur->suid = su;
    return 0;
}

static int64_t lc_setresgid(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t rg = (a == (uint64_t)-1) ? cur->gid : (uint32_t)a;
    uint32_t eg = (b == (uint64_t)-1) ? cur->egid : (uint32_t)b;
    uint32_t sg = (c == (uint64_t)-1) ? cur->sgid : (uint32_t)c;
    if (cur->egid != 0 &&
        ((rg != cur->gid && rg != cur->egid && rg != cur->sgid) ||
         (eg != cur->gid && eg != cur->egid && eg != cur->sgid) ||
         (sg != cur->gid && sg != cur->egid && sg != cur->sgid)))
        return -LINUX_EPERM;
    cur->gid = rg;
    cur->egid = eg;
    cur->sgid = sg;
    return 0;
}

static int64_t lc_getresuid(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)d; (void)e; (void)f;
    if (!user_ptr_ok(r, a, 4, 1) || !user_ptr_ok(r, b, 4, 1) ||
        !user_ptr_ok(r, c, 4, 1))
        return -LINUX_EFAULT;
    *(uint32_t *)(uintptr_t)a = current->uid;
    *(uint32_t *)(uintptr_t)b = current->euid;
    *(uint32_t *)(uintptr_t)c = current->suid;
    return 0;
}

static int64_t lc_getresgid(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)d; (void)e; (void)f;
    if (!user_ptr_ok(r, a, 4, 1) || !user_ptr_ok(r, b, 4, 1) ||
        !user_ptr_ok(r, c, 4, 1))
        return -LINUX_EFAULT;
    *(uint32_t *)(uintptr_t)a = current->gid;
    *(uint32_t *)(uintptr_t)b = current->egid;
    *(uint32_t *)(uintptr_t)c = current->sgid;
    return 0;
}

static int64_t lc_getgroups(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}

static int64_t lc_setgroups(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    if (current->egid != 0 && current->euid != 0)
        return -LINUX_EPERM;
    return 0;
}

static int64_t lc_chown(struct X86_REGS *r, uint64_t a, uint64_t b,
                        uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chown(kpath, (uint32_t)b, (uint32_t)c);
}

static int64_t lc_lchown(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    return lc_chown(r, a, b, c, d, e, f);
}

static int64_t lc_fchown(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    if (a < 3 || a >= MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    uint32_t gfd = fd_local2global((uint32_t)a);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    struct FS_INODE obj;
    if (ext2_read_inode(pf->fd_inode->i_no, &obj))
        return -LINUX_EIO;
    if (b != (uint64_t)-1)
        obj.i_uid = (uint16_t)b;
    if (c != (uint64_t)-1)
        obj.i_gid = (uint16_t)c;
    return ext2_write_inode(pf->fd_inode->i_no, &obj) ? -LINUX_EIO : 0;
}

static int64_t lc_fchownat(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a;
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_chown(kpath, (uint32_t)c, (uint32_t)d);
}

static int64_t lc_fchmod(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    if (a < 3 || a >= MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    uint32_t gfd = fd_local2global((uint32_t)a);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    struct FS_INODE obj;
    if (ext2_read_inode(pf->fd_inode->i_no, &obj))
        return -LINUX_EIO;
    obj.i_mode = (obj.i_mode & 0xF000u) | ((uint32_t)b & 0x0FFFu);
    return ext2_write_inode(pf->fd_inode->i_no, &obj) ? -LINUX_EIO : 0;
}

static int64_t lc_fchmodat(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a; (void)d;
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_chmod(kpath, (uint32_t)c);
}

static int64_t lc_getid_field_dispatch(struct X86_REGS *r, uint64_t a,
                                       uint64_t b, uint64_t c, uint64_t d,
                                       uint64_t e, uint64_t f);
#define UNIX_FD_BASE 0x400
#define MAX_UNIX_SOCK 16
#define UNIX_BUF_SIZE 2048

static struct {
    uint8_t active;
    uint8_t connected;
    uint8_t type;
    uint8_t nonblock;
    int8_t peer;
    int8_t self;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint8_t buf[UNIX_BUF_SIZE];
} u_unix[MAX_UNIX_SOCK];

static int unix_fd_slot(uint64_t fd) {
    uint64_t idx = fd - UNIX_FD_BASE;
    if (idx >= MAX_UNIX_SOCK || !u_unix[idx].active)
        return -1;
    return (int)idx;
}

static int unix_alloc_slot(void) {
    for (int i = 0; i < MAX_UNIX_SOCK; i++) {
        if (!u_unix[i].active)
            return i;
    }
    return -1;
}

static uint32_t unix_buf_write(int idx, const uint8_t *src, uint32_t len) {
    uint32_t done = 0;
    while (done < len && u_unix[idx].count < UNIX_BUF_SIZE) {
        u_unix[idx].buf[u_unix[idx].tail] = src[done];
        u_unix[idx].tail =
            (uint16_t)((u_unix[idx].tail + 1) % UNIX_BUF_SIZE);
        u_unix[idx].count++;
        done++;
    }
    return done;
}

static uint32_t unix_buf_read(int idx, uint8_t *dst, uint32_t len) {
    uint32_t done = 0;
    while (done < len && u_unix[idx].count > 0) {
        dst[done] = u_unix[idx].buf[u_unix[idx].head];
        u_unix[idx].head =
            (uint16_t)((u_unix[idx].head + 1) % UNIX_BUF_SIZE);
        u_unix[idx].count--;
        done++;
    }
    return done;
}

static int unix_peer_alive(int idx) {
    int8_t p = u_unix[idx].peer;
    return p >= 0 && u_unix[p].active;
}

static int32_t unix_send(int idx, const void *buf, uint32_t len) {
    if (!u_unix[idx].connected || u_unix[idx].peer < 0)
        return -LINUX_ENOTCONN;
    int p = u_unix[idx].peer;
    if (!u_unix[p].active)
        return -LINUX_EPIPE;
    for (;;) {
        uint32_t n = unix_buf_write(p, (const uint8_t *)buf, len);
        if (n > 0 || len == 0)
            return (int32_t)n;
        if (u_unix[idx].nonblock)
            return -LINUX_EAGAIN;
        mtime_sleep(1);
    }
}

static int32_t unix_recv(int idx, void *buf, uint32_t len) {
    if (u_unix[idx].count == 0) {
        if (len == 0)
            return 0;
        while (u_unix[idx].count == 0) {
            if (!unix_peer_alive(idx))
                return 0;
            if (u_unix[idx].nonblock)
                return -LINUX_EAGAIN;
            mtime_sleep(1);
        }
    }
    return (int32_t)unix_buf_read(idx, (uint8_t *)buf, len);
}

static int unix_path_ok(struct X86_REGS *r, uint64_t addr, uint32_t addrlen) {
    if (addr == 0 || addrlen < 3)
        return 0;
    const char *p = (const char *)(uintptr_t)(addr + 2);
    uint32_t left = addrlen - 2;
    for (uint32_t i = 0; i < left; i++) {
        char c;
        if (!user_ptr_ok(r, (uintptr_t)(p + i), 1, 0))
            return 0;
        c = p[i];
        if (c == 0)
            return 1;
    }
    return 1;
}

static int64_t lc_socket(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)d; (void)e; (void)f;
    int domain = (int)a;
    int type = (int)b;
    if (domain == 1) {
        int i = unix_alloc_slot();
        if (i < 0)
            return -LINUX_ENFILE;
        memset(&u_unix[i], 0, sizeof(u_unix[i]));
        u_unix[i].active = 1;
        u_unix[i].connected = 0;
        u_unix[i].type = (uint8_t)type;
        u_unix[i].peer = -1;
        u_unix[i].self = (int8_t)i;
        return UNIX_FD_BASE + i;
    }
    if (domain != 2)
        return -LINUX_EAFNOSUPPORT;
    return net_socket(domain, type, (int)c);
}

static int sockaddr_in_parts(struct X86_REGS *r, uint64_t addr,
                             uint64_t addrlen, uint32_t *ip,
                             uint16_t *port) {
    if (addr == 0 || addrlen < 8) {
        return -1;
    }

    uint8_t tmp[8];
    if (copy_from_user(tmp, (const void *)(uintptr_t)addr, sizeof(tmp)) != 0)
        return -1;
    uint16_t family = (uint16_t)(tmp[0] | ((uint16_t)tmp[1] << 8));
    if (family != 2)
        return -1;
    *port = (uint16_t)((uint16_t)(tmp[2] << 8) | tmp[3]);
    *ip = ((uint32_t)tmp[4]) | ((uint32_t)tmp[5] << 8) |
          ((uint32_t)tmp[6] << 16) | ((uint32_t)tmp[7] << 24);
    return 0;
}

static int fill_sockaddr_in(struct X86_REGS *r, uint64_t addr,
                            uint64_t addrlen_ptr, uint32_t ip,
                            uint16_t port) {
    if (addr == 0 || addrlen_ptr == 0)
        return 0;

    uint32_t klen = 0;
    if (copy_from_user(&klen, (const void *)(uintptr_t)addrlen_ptr,
                       sizeof(klen)) != 0)
        return -1;
    uint8_t sa[8];
    sa[0] = 2;
    sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)ip;
    sa[5] = (uint8_t)(ip >> 8);
    sa[6] = (uint8_t)(ip >> 16);
    sa[7] = (uint8_t)(ip >> 24);
    uint32_t n = (klen < sizeof(sa)) ? klen : (uint32_t)sizeof(sa);
    if (n != 0 && copy_to_user((void *)(uintptr_t)addr, sa, n) != 0)
        return -1;
    uint32_t alen = 16;
    if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &alen, sizeof(alen)) != 0)
        return -1;
    return 0;
}

static int64_t lc_connect(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOENT;
    uint32_t ip;
    uint16_t port;
    if (sockaddr_in_parts(r, b, c, &ip, &port) != 0)
        return -LINUX_EAFNOSUPPORT;
    return net_connect((int)a, ip, port);
}

static int64_t lc_bind(struct X86_REGS *r, uint64_t a, uint64_t b,
                       uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    uint32_t ip;
    uint16_t port;
    if (sockaddr_in_parts(r, b, c, &ip, &port) != 0)
        return -LINUX_EAFNOSUPPORT;
    return net_bind((int)a, ip, port);
}

static int64_t lc_listen(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    return net_listen((int)a, (int)b);
}

static int64_t lc_accept(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_EAGAIN;
    return net_accept((int)a);
}

static int64_t lc_shutdown(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    return net_shutdown((int)a, (int)b);
}

static int64_t lc_sendto(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)d;
    int uslot = unix_fd_slot(a);
    if (uslot >= 0) {
        if (!user_ptr_ok(r, b, (uint32_t)c, 0))
            return -LINUX_EFAULT;
        return unix_send(uslot, (const void *)(uintptr_t)b, (uint32_t)c);
    }
    if (!user_ptr_ok(r, b, (uint32_t)c, 0))
        return -LINUX_EFAULT;
    if (e != 0) {
        uint32_t ip;
        uint16_t port;
        if (sockaddr_in_parts(r, e, f, &ip, &port) != 0)
            return -LINUX_EAFNOSUPPORT;
        return net_sendto((int)a, (const void *)(uintptr_t)b, (uint32_t)c,
                          ip, port);
    }
    return net_send((int)a, (const void *)(uintptr_t)b, (uint32_t)c);
}

static int64_t lc_recvfrom(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)d;
    int uslot = unix_fd_slot(a);
    if (uslot >= 0) {
        if (!user_ptr_ok(r, b, (uint32_t)c, 1))
            return -LINUX_EFAULT;
        return unix_recv(uslot, (void *)(uintptr_t)b, (uint32_t)c);
    }
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    uint32_t sip = 0;
    uint16_t sport = 0;
    int n = net_recvfrom((int)a, (void *)(uintptr_t)b, (uint32_t)c, &sip,
                         &sport);
    if (n < 0)
        return -LINUX_EAGAIN;
    if (e != 0 &&
        fill_sockaddr_in(r, e, f, sip, sport) != 0)
        return -LINUX_EFAULT;
    return n;
}

static int64_t lc_getsockname(struct X86_REGS *r, uint64_t a, uint64_t b,
                              uint64_t c, uint64_t d, uint64_t e,
                              uint64_t f) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_EINVAL;
    uint32_t ip = 0;
    uint16_t port = 0;
    if (net_getsockname((int)a, &ip, &port) != 0)
        return -LINUX_ENOTSOCK;
    if (fill_sockaddr_in(r, b, c, ip, port) != 0)
        return -LINUX_EFAULT;
    return 0;
}

static int64_t lc_getpeername(struct X86_REGS *r, uint64_t a, uint64_t b,
                              uint64_t c, uint64_t d, uint64_t e,
                              uint64_t f) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOTCONN;
    uint32_t ip = 0;
    uint16_t port = 0;
    if (net_getpeername((int)a, &ip, &port) != 0)
        return -LINUX_ENOTSOCK;
    if (fill_sockaddr_in(r, b, c, ip, port) != 0)
        return -LINUX_EFAULT;
    return 0;
}

static int64_t lc_setsockopt(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e,
                             uint64_t f) {
    (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;

    if (d != 0 && !user_ptr_ok(r, d, (uint32_t)e, 0))
        return -LINUX_EFAULT;
    return net_setsockopt((int)a, (int)b, (int)c, (const void *)(uintptr_t)d,
                          (uint32_t)e);
}

static int64_t lc_getsockopt(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e,
                             uint64_t f) {
    (void)f;
    uint32_t klen = 0;
    uint32_t kval = 0;
    if (e != 0 &&
        copy_from_user(&klen, (const void *)(uintptr_t)e, sizeof(klen)) != 0)
        return -LINUX_EFAULT;
    if (unix_fd_slot(a) >= 0) {
        if (d != 0) {
            if (klen < 4)
                return -LINUX_EINVAL;
            if (copy_to_user((void *)(uintptr_t)d, &kval, sizeof(kval)) != 0)
                return -LINUX_EFAULT;
        }
        return 0;
    }

    if (d != 0 && e != 0 && klen < 4)
        return -LINUX_EINVAL;
    if (net_getsockopt((int)a, (int)b, (int)c, d != 0 ? &kval : NULL,
                       e != 0 ? &klen : NULL) != 0)
        return -LINUX_EINVAL;
    if (d != 0 &&
        copy_to_user((void *)(uintptr_t)d, &kval, sizeof(kval)) != 0)
        return -LINUX_EFAULT;
    if (e != 0 &&
        copy_to_user((void *)(uintptr_t)e, &klen, sizeof(klen)) != 0)
        return -LINUX_EFAULT;
    return 0;
}

struct LINUX_MSGHDR {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t ctrl;
    uint64_t ctrllen;
    int32_t flags;
};

static int64_t lc_sendmsg(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOTCONN;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_MSGHDR), 0))
        return -LINUX_EFAULT;
    struct LINUX_MSGHDR mh;
    memcpy(&mh, (const void *)(uintptr_t)b, sizeof mh);
    uint32_t ip = 0;
    uint16_t port = 0;
    int have_addr = 0;
    if (mh.name != 0 && mh.namelen >= 6) {
        if (sockaddr_in_parts(r, mh.name, mh.namelen, &ip, &port) != 0)
            return -LINUX_EAFNOSUPPORT;
        have_addr = 1;
    }
    uint8_t sbuf[2048];
    uint32_t total = 0;
    for (uint64_t i = 0; i < mh.iovlen && total < sizeof sbuf; i++) {
        uint64_t ent = mh.iov + i * sizeof(struct LINUX_IOVEC);
        if (!user_ptr_ok(r, ent, sizeof(struct LINUX_IOVEC), 0))
            return -LINUX_EFAULT;
        struct LINUX_IOVEC iv;
        memcpy(&iv, (const void *)(uintptr_t)ent, sizeof iv);
        uint32_t n = iv.iov_len > sizeof sbuf - total ? sizeof sbuf - total
                                                  : (uint32_t)iv.iov_len;
        if (n == 0)
            continue;
        if (!user_ptr_ok(r, (uint64_t)iv.iov_base, n, 0))
            return -LINUX_EFAULT;
        memcpy(sbuf + total, (const void *)(uintptr_t)iv.iov_base, n);
        total += n;
    }
    if (have_addr)
        return net_sendto((int)a, sbuf, total, ip, port);
    return net_send((int)a, sbuf, total);
}

static int64_t lc_recvmsg(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOTCONN;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_MSGHDR), 1))
        return -LINUX_EFAULT;
    struct LINUX_MSGHDR mh;
    memcpy(&mh, (const void *)(uintptr_t)b, sizeof mh);
    uint8_t rbuf[2048];
    uint32_t sip = 0;
    uint16_t sport = 0;
    int n = net_recvfrom((int)a, rbuf, sizeof rbuf, &sip, &sport);
    if (n < 0)
        return -LINUX_EAGAIN;
    uint32_t copied = 0;
    if (mh.iov != 0 && mh.iovlen > 0) {
        uint64_t ent = mh.iov;
        if (!user_ptr_ok(r, ent, sizeof(struct LINUX_IOVEC), 0))
            return -LINUX_EFAULT;
        struct LINUX_IOVEC iv;
        memcpy(&iv, (const void *)(uintptr_t)ent, sizeof iv);
        uint32_t n2 = iv.iov_len > (uint64_t)n ? (uint32_t)n : (uint32_t)iv.iov_len;
        if (n2 != 0 && !user_ptr_ok(r, (uint64_t)iv.iov_base, n2, 1))
            return -LINUX_EFAULT;
        if (n2 != 0)
            memcpy((void *)(uintptr_t)iv.iov_base, rbuf, n2);
        copied = n2;
    }
    if (mh.name != 0 && mh.namelen >= 6) {
        if (!user_ptr_ok(r, mh.name, 8, 1))
            return -LINUX_EFAULT;
        uint8_t sa[8];
        sa[0] = 2;
        sa[1] = 0;
        sa[2] = (uint8_t)(sport >> 8);
        sa[3] = (uint8_t)sport;
        sa[4] = (uint8_t)sip;
        sa[5] = (uint8_t)(sip >> 8);
        sa[6] = (uint8_t)(sip >> 16);
        sa[7] = (uint8_t)(sip >> 24);
        memcpy((void *)(uintptr_t)mh.name, sa, 8);
    }
    mh.namelen = 16;
    memcpy((void *)(uintptr_t)b, &mh, sizeof mh);
    return (int64_t)copied;
}

static int64_t lc_socketpair(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)c;
    (void)e;
    (void)f;
    if (a != 1)
        return -LINUX_EAFNOSUPPORT;
    if (b != 1 && b != 2)
        return -LINUX_EINVAL;
    if (d == 0 || !user_ptr_ok(r, d, 8, 1))
        return -LINUX_EFAULT;
    int i0 = unix_alloc_slot();
    int i1 = -1;
    if (i0 >= 0)
        i1 = unix_alloc_slot();
    if (i0 < 0 || i1 < 0)
        return -LINUX_ENFILE;
    memset(&u_unix[i0], 0, sizeof(u_unix[i0]));
    memset(&u_unix[i1], 0, sizeof(u_unix[i1]));
    u_unix[i0].active = 1;
    u_unix[i1].active = 1;
    u_unix[i0].connected = 1;
    u_unix[i1].connected = 1;
    u_unix[i0].type = (uint8_t)b;
    u_unix[i1].type = (uint8_t)b;
    u_unix[i0].peer = (int8_t)i1;
    u_unix[i1].peer = (int8_t)i0;
    u_unix[i0].self = (int8_t)i0;
    u_unix[i1].self = (int8_t)i1;
    int32_t out[2];
    out[0] = UNIX_FD_BASE + i0;
    out[1] = UNIX_FD_BASE + i1;
    memcpy((void *)(uintptr_t)d, out, sizeof(out));
    return 0;
}

static int64_t lc_close(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    int uslot = unix_fd_slot(a);
    if (uslot >= 0) {
        u_unix[uslot].active = 0;
        u_unix[uslot].count = 0;
        u_unix[uslot].head = 0;
        u_unix[uslot].tail = 0;
        return 0;
    }
    if (lc_close_extra((int32_t)a) != 0)
        return 0;
    return close_file((int32_t)a);
}

static int64_t lc_tkill(struct X86_REGS *r, uint64_t a, uint64_t b,
                        uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_kill((int)a, (int)b) < 0 ? -LINUX_EINVAL : 0;
}

static int64_t lc_tgkill(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    (void)a;
    return sys_kill((int)b, (int)c) < 0 ? -LINUX_EINVAL : 0;
}

static int64_t lc_setsid(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    (void)a;
    struct TASK *cur = current;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct TASK *t = &task_table[i];
        if (t == cur || !t->slot_used || t->status == TASK_DIED)
            continue;
        if (t->pgid == cur->pid) {
            return -LINUX_EPERM;
        }
    }
    cur->sid = cur->pid;
    cur->pgid = cur->pid;
    return (int64_t)cur->pid;
}

static int64_t lc_waitid(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (c && !user_ptr_ok(r, c, sizeof(struct LINUX_SIGINFO), 1))
        return -LINUX_EFAULT;
    int rc = sys_waitid((int)a, (int32_t)b, (struct LINUX_SIGINFO *)(uintptr_t)c,
                        (uint32_t)d);
    return rc == 0 ? 0 : -LINUX_ECHILD;
}

static int64_t lc_sigaltstack(struct X86_REGS *r, uint64_t a, uint64_t b,
                              uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    struct TASK *cur = current;
   
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_STACK_T), 1))
        return -LINUX_EFAULT;
    if (b) {
        struct LINUX_STACK_T o;
        o.ss_sp = cur->sigalt_sp;
        o.ss_flags = cur->sigalt_sp ? 0 : LINUX_SS_DISABLE;
        o.ss_pad = 0;
        o.ss_size = cur->sigalt_size;
        memcpy((void *)(uintptr_t)b, &o, sizeof(o));
    }
    if (a) {
        if (!user_ptr_ok(r, a, sizeof(struct LINUX_STACK_T), 0))
            return -LINUX_EFAULT;
        struct LINUX_STACK_T n;
        memcpy(&n, (const void *)(uintptr_t)a, sizeof(n));
        if (n.ss_flags & ~LINUX_SS_DISABLE)
            return -LINUX_EINVAL;
        if (n.ss_flags & LINUX_SS_DISABLE) {
            cur->sigalt_sp = 0;
            cur->sigalt_size = 0;
            cur->sigalt_flags = LINUX_SS_DISABLE;
        } else {
            if (n.ss_size < LINUX_MINSIGSTKSZ)
                return -LINUX_ENOMEM;
            cur->sigalt_sp = n.ss_sp;
            cur->sigalt_size = (uint32_t)n.ss_size;
            cur->sigalt_flags = 0;
        }
    }
    return 0;
}

static int64_t lc_sigsuspend(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, 4, 0))
        return -LINUX_EFAULT;
    uint32_t mask;
    memcpy(&mask, (const void *)(uintptr_t)a, 4);
    struct TASK *cur = current;
    uint32_t old = cur->signal_mask;
    cur->signal_mask = (mask << 1) & ~((1u << SIGKILL) | (1u << SIGSTOP));
    for (;;) {
        if (cur->signal_pending & ~cur->signal_mask)
            break;
        thread_block_with_status(TASK_WAITING);
    }
    check_pending_signals(r);
    cur->signal_mask = old;
    return -LINUX_EINTR;
}

static int32_t compat_readv(int32_t fd, struct LINUX_IOVEC *iov,
                            int32_t iovcnt) {
    if (iovcnt < 0 || iovcnt > 16)
        return -LINUX_EINVAL;
    int32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        struct LINUX_IOVEC v;
        memcpy(&v, (const void *)(uintptr_t)&iov[i], sizeof(v));
        if (v.iov_len == 0)
            continue;
        int32_t n = compat_read(fd, v.iov_base, (uint32_t)v.iov_len);
        if (n < 0)
            return total > 0 ? total : n;
        total += n;
        if ((uint32_t)n < v.iov_len)
            break;
    }
    return total;
}

static int32_t compat_wait4(int32_t *status_out) {
    int32_t st = 0;
    int32_t pid = sys_wait(&st);
    if (pid < 0)
        return -LINUX_ECHILD;
    if (status_out) {
        int32_t s8 = st & 0xff;
        uint32_t lst;
        if (s8 >= 128)
            lst = (uint32_t)(s8 - 128);
        else
            lst = (uint32_t)s8 << 8;
        *(int32_t *)status_out = (int32_t)lst;
    }
    return pid;
}

static int32_t compat_uname(void *buf) {
    struct LINUX_UTSNAME u;
    memset(&u, 0, sizeof(u));
    memcpy(u.sysname, "Linux", 6);
    memcpy(u.nodename, "corez", 6);
    memcpy(u.release, "5.10.0-corez", 13);
    memcpy(u.version, "#1 CoreZOS SMP", 16);
    memcpy(u.machine, "x86_64", 7);
    memcpy(u.domainname, "(none)", 7);
    memcpy(buf, &u, sizeof(u));
    return 0;
}

static int32_t compat_sysinfo(void *buf) {
    struct LINUX_SYSINFO si;
    memset(&si, 0, sizeof(si));
    si.uptime = (int64_t)(tick / PIT_HZ);
    si.totalram = (uint64_t)kernel_pool.pool_size;
    si.freeram = (uint64_t)kernel_pool_free_count() * PAGE_SIZE;
    si.mem_unit = 1;
    si.procs = 1;
    memcpy(buf, &si, sizeof(si));
    return 0;
}

static int32_t compat_times(void *buf) {
    if (buf) {
        struct LINUX_TMS t;
        memset(&t, 0, sizeof(t));
        t.utime = (int64_t)current->elapsed_ticks;
        memcpy(buf, &t, sizeof(t));
    }
    return (int32_t)tick;
}

static int32_t compat_ftruncate(int32_t fd, int32_t length) {
    (void)length;
    if (fd < 3 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EINVAL;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL || pf->fd_flag == PIPE_FLAG)
        return -LINUX_EINVAL;
    ext2_truncate_inode(pf->fd_inode);
    ext2_write_inode(pf->fd_inode->i_no, pf->fd_inode);
    return 0;
}

static int32_t compat_flags_linux2native(uint32_t lflags) {
    int32_t nflags = (int32_t)(lflags & 3u);
    if (lflags & LINUX_O_CREAT)
        nflags |= O_CREAT;
    return nflags;
}

static uint32_t compat_mode_native(uint32_t filetype) {
    if (filetype == FT_DIRECTORY)
        return LINUX_S_IFDIR | 0755u;
    if (filetype == FT_CHARDEVICE)
        return LINUX_S_IFCHR | 0666u;
    if (filetype == FT_SYMLINK)
        return LINUX_S_IFLNK | 0777u;
    return LINUX_S_IFREG | 0644u;
}

static void compat_stat_fill(struct LINUX_STAT *ls, uint32_t ino, int64_t size,
                             uint32_t mode, uint32_t uid, uint32_t gid) {
    memset(ls, 0, sizeof(*ls));
    ls->st_dev = 0x800u;
    ls->st_ino = ino;
    ls->st_nlink = 1;
    ls->st_mode = mode;
    ls->st_uid = uid;
    ls->st_gid = gid;
    ls->st_blksize = 512;
    ls->st_blocks = (int64_t)((size + 511) / 512);
    ls->st_size = size;
    ls->st_atim.tv_sec = (int64_t)(tick / PIT_HZ);
    ls->st_mtim = ls->st_atim;
    ls->st_ctim = ls->st_atim;
}

static int32_t compat_stat_linux(const char *path, uint64_t ub) {
    struct LINUX_STAT ls;
    uint32_t ino = 0, size = 0, mode = 0, uid = 0, gid = 0;
    if (proc_match(path)) {
        compat_stat_fill(&ls, 2, 0, LINUX_S_IFREG | 0444u, 0, 0);
    } else if (fs_stat_full(path, &ino, &size, &mode, &uid, &gid) != 0) {
        return -LINUX_ENOENT;
    } else {
        compat_stat_fill(&ls, ino, (int64_t)size, mode, uid, gid);
    }
    memcpy((void *)(uintptr_t)ub, &ls, sizeof(ls));
    return 0;
}

static int32_t compat_fstat_linux(int32_t fd, uint64_t ub) {
    struct LINUX_STAT ls;
    if (fd >= 0 && fd < 3) {
        compat_stat_fill(&ls, 0, 0, LINUX_S_IFCHR | 0600u, 0, 0);
    } else if (compat_fd_isdir(fd)) {
        uint32_t gfd = fd_local2global((uint32_t)fd);
        struct FILE *pf = file_get(gfd);
        compat_stat_fill(&ls, pf->fd_inode->i_no, (int64_t)pf->fd_inode->i_size,
                         pf->fd_inode->i_mode, pf->fd_inode->i_uid,
                         pf->fd_inode->i_gid);
    } else if (is_pipe(fd)) {
        compat_stat_fill(&ls, 0, 0, LINUX_S_IFIFO | 0600u, 0, 0);
    } else {
        uint32_t gfd = fd_local2global((uint32_t)fd);
        struct FILE *pf = file_get(gfd);
        if (pf == NULL || pf->fd_inode == NULL)
            return -LINUX_EBADF;
        compat_stat_fill(&ls, pf->fd_inode->i_no,
                         (int64_t)pf->fd_inode->i_size, pf->fd_inode->i_mode,
                         pf->fd_inode->i_uid, pf->fd_inode->i_gid);
    }
    memcpy((void *)(uintptr_t)ub, &ls, sizeof(ls));
    return 0;
}

static int32_t compat_openat(int32_t dirfd, const char *kpath,
                             uint32_t lflags) {
    if (dirfd != LINUX_AT_FDCWD)
        return -LINUX_EINVAL;
    struct FS_STAT pst;
    uint32_t ino = 0;
    int is_dir = 0;
    if (ext2_lookup(kpath, &ino, &is_dir) == 0 && is_dir) {
        if (lflags & (LINUX_O_CREAT | LINUX_O_TRUNC | LINUX_O_APPEND))
            return -LINUX_EISDIR;
        int32_t fd = compat_dir_fd(kpath);
        return fd < 0 ? -LINUX_ENOENT : fd;
    }
    if (lflags & LINUX_O_DIRECTORY) {
        if (sys_stat(kpath, &pst) != 0 || pst.st_filetype != FT_DIRECTORY)
            return -LINUX_ENOTDIR;
    }
    if ((lflags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
        (LINUX_O_CREAT | LINUX_O_EXCL)) {
        if (sys_stat(kpath, &pst) == 0)
            return -LINUX_EEXIST;
    }
    int32_t fd = open_file(kpath, (uint8_t)compat_flags_linux2native(lflags));
    if (fd < 0)
        return -(current->errno > 0 ? current->errno : LINUX_ENOENT);
    if (lflags & LINUX_O_TRUNC)
        compat_ftruncate(fd, 0);
    if (lflags & LINUX_O_APPEND)
        sys_lseek(fd, 0, (uint8_t)SEEK_END);
    return fd;
}

static void lc_seterrno(struct TASK *cur, int32_t val) {
    cur->errno = val;
    if (cur->tls_selector == SELECTOR_TLS && cur->tls_base != 0) {
        *(volatile int32_t *)cur->tls_base = val;
    }
}

static int32_t compat_write(int32_t fd, const void *buf, uint32_t count) {
    if (fd < 0)
        return -1;
    int xi = evfd_slot(fd);
    if (xi >= 0)
        return (int32_t)lc_eventfd_write(xi, buf, count);
    xi = unix_fd_slot((uint64_t)fd);
    if (xi >= 0)
        return unix_send(xi, buf, count);
    if (tfd_slot(fd) >= 0 || ep_slot(fd) >= 0)
        return -LINUX_EINVAL;
    if (net_is_socket(fd)) {
        int32_t n = (int32_t)net_send(fd, buf, count);
        return n < 0 ? -LINUX_EAGAIN : n;
    }
    if (compat_fd_isdir(fd))
        return -LINUX_EISDIR;
    if (io_is_file_fd(fd)) {
        if (is_pipe(fd)) {
            struct FILE *pf2 = file_get(fd_local2global((uint32_t)fd));
            if (pf2 == NULL || pf2->fd_inode == NULL)
                return -LINUX_EBADF;
            uint32_t len = ioq_length((struct TTY_IOQUEUE *)pf2->fd_inode);
            if (len >= BUFSIZE) {
                if (pf2->fd_nonblock)
                    return -LINUX_EAGAIN;
                while (len >= BUFSIZE) {
                    mtime_sleep(1);
                    len = ioq_length((struct TTY_IOQUEUE *)pf2->fd_inode);
                }
            }
            return (int32_t)pipe_write(fd, buf, count);
        }
        return (int32_t)write_file(fd, buf, count);
    }
    if (fd == 1 || fd == 2)
        return TTY.write((const char *)buf, count);
    const char *s = (const char *)buf;
    for (uint32_t k = 0; k < count; k++) {
        console_putc(s[k]);
    }
    return (int32_t)count;
}

static int32_t compat_read(int32_t fd, void *buf, uint32_t count) {
    int xi = evfd_slot(fd);
    if (xi >= 0)
        return (int32_t)lc_eventfd_read(xi, buf, count);
    xi = tfd_slot(fd);
    if (xi >= 0)
        return (int32_t)lc_timerfd_read(xi, buf, count);
    xi = unix_fd_slot((uint64_t)fd);
    if (xi >= 0)
        return unix_recv(xi, buf, count);
    if (ep_slot(fd) >= 0)
        return -LINUX_EINVAL;
    if (net_is_socket(fd)) {
        int32_t n = (int32_t)net_recv(fd, buf, count);
        return n < 0 ? -LINUX_EAGAIN : n;
    }
    if (io_is_file_fd(fd)) {
        if (compat_fd_isdir(fd))
            return -LINUX_EISDIR;
        if (is_pipe(fd)) {
            struct FILE *pf3 = file_get(fd_local2global((uint32_t)fd));
            if (pf3 == NULL || pf3->fd_inode == NULL)
                return -LINUX_EBADF;
            uint32_t len = ioq_length((struct TTY_IOQUEUE *)pf3->fd_inode);
            if (len == 0) {
                if (pf3->fd_nonblock)
                    return -LINUX_EAGAIN;
                if (count == 0)
                    return 0;
                while (len == 0) {
                    if (!pipe_has_writer(fd_local2global((uint32_t)fd)))
                        return 0;
                    mtime_sleep(1);
                    len = ioq_length((struct TTY_IOQUEUE *)pf3->fd_inode);
                }
            }
            return (int32_t)pipe_read(fd, buf, count);
        }
        return (int32_t)read_file(fd, buf, count);
    }
    if (fd == 0)
        return TTY.read((char *)buf, count);
    return -LINUX_EBADF;
}

static int32_t compat_set_thread_area(uint32_t base) {
    if (base == 0 || !user_range_writable(base, sizeof(int32_t)))
        return -LINUX_EFAULT;
    struct TASK *cur = current;
    cur->tls_base = base;
    cur->tls_selector = SELECTOR_TLS;
    cur->tls_msr = 0;
    tls_desc_set_base(base);
    return 0;
}

static int32_t sys_compat_writev(int32_t fd, struct LINUX_IOVEC *iov,
                                 int32_t iovcnt) {
    if (iovcnt < 0)
        return -1;
    uint32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        int32_t n =
            compat_write(fd, (const void *)iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return -1;
        total += (uint32_t)n;
    }
    return (int32_t)total;
}

static int user_ptr_ok(struct X86_REGS *r, uint64_t ptr, uint32_t len,
                       int wr) {
    return (r->cs & 3) != 3 || access_ok((const void *)(uintptr_t)ptr, len, wr);
}

static int copy_user_str(struct X86_REGS *r, char *dst, uint64_t ptr) {
    return (r->cs & 3) != 3 ||
           copy_str_from_user(dst, (const char *)(uintptr_t)ptr,
                              MAX_PATH_LEN) == 0;
}

typedef int64_t (*LcFn)(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f);

static int64_t lc_getpid(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return (int64_t)current->pid;
}

static int64_t lc_getppid(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    struct TASK *cur = current;
    return cur->parent_pid >= 0 ? (int64_t)cur->parent_pid : 0;
}

static int64_t lc_getid(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return 0;
}

static int64_t lc_write(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)d;
    (void)e;
    (void)f;
    if (!user_ptr_ok(r, b, (uint32_t)c, 0))
        return -LINUX_EFAULT;
    return compat_write((int32_t)a, (const void *)b, (uint32_t)c);
}

static int64_t lc_read(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)d;
    (void)e;
    (void)f;
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return compat_read((int32_t)a, (void *)b, (uint32_t)c);
}

static int64_t lc_pread64(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    int32_t fd = (int32_t)a;
    int32_t saved;
    int32_t n;

    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    if (fd < 0 || (uint32_t)d > 0x7fffffffu)
        return -LINUX_EINVAL;
    saved = sys_lseek(fd, 0, SEEK_CUR);
    if (saved < 0)
        return -LINUX_ESPIPE;
    if (sys_lseek(fd, (int32_t)d, SEEK_SET) < 0)
        return -LINUX_EINVAL;
    n = compat_read(fd, (void *)b, (uint32_t)c);
    sys_lseek(fd, saved, SEEK_SET);
    return n;
}

static int64_t lc_exit(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    sys_exit((int32_t)a);
    return 0;
}

__attribute__((noreturn)) static int64_t lc_exit_group(struct X86_REGS *r,
                                                       uint64_t a, uint64_t b,
                                                       uint64_t c, uint64_t d,
                                                       uint64_t e, uint64_t f) {
    (void)r;
    sys_exit((int32_t)a);
    for (;;) {
    }
}

static int64_t lc_brk(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                      uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return (int64_t)sys_brk((uint32_t)a);
}

static int64_t lc_mmap(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    struct SYS_MMAP_ARGS m = {(uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d,
                          (uint32_t)e,
                          (uint32_t)(f & ~(uint32_t)(PAGE_SIZE - 1u))};
    int64_t ret = (int32_t)sys_mmap(&m);
    return ret;
}

static int64_t lc_munmap(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_munmap((uint32_t)a, (uint32_t)b);
}

static int64_t lc_set_thread_area(struct X86_REGS *r, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f) {
    (void)r;
    return compat_set_thread_area((uint32_t)a);
}

static int64_t lc_set_tid_address(struct X86_REGS *r, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f) {
    (void)r;
    struct TASK *cur = current;
    if (a)
        *(volatile int32_t *)a = (int32_t)cur->pid;
    return (int64_t)cur->pid;
}

static int64_t lc_writev(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)d;
    (void)e;
    (void)f;
    if (!user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    return sys_compat_writev((int32_t)a, (struct LINUX_IOVEC *)b, (int32_t)c);
}

static int64_t lc0_writev(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if ((b == 0 && c > 0) || c > 1024 ||
        !user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    int32_t total = 0;
    for (int32_t i = 0; i < (int32_t)c; i++) {
        uint32_t pair[2];
        memcpy(pair, (const void *)(uintptr_t)(b + (uint32_t)i * 8u),
               sizeof(pair));
        if (pair[1] == 0)
            continue;
        if (!user_ptr_ok(r, pair[0], pair[1], 0))
            return -LINUX_EFAULT;
        int32_t n =
            compat_write((int32_t)a, (const void *)(uintptr_t)pair[0], pair[1]);
        if (n < 0)
            return n;
        total += n;
        if ((uint32_t)n < pair[1])
            break;
    }
    return total;
}

static int64_t lc_fstat(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_fstat_linux((int32_t)a, b);
}

static int64_t lc_stat(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) ||
        !user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_stat_linux(kpath, b);
}

static int64_t lc_lseek(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_lseek((int32_t)a, (int32_t)b, (uint8_t)(c + 1u));
}

static int64_t lc_fcntl(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_fcntl((int32_t)a, (int32_t)b, c);
}

static int64_t lc_readlink(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) || !user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return sys_readlink(kpath, (char *)b, c);
}

static int64_t lc_chdir(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chdir(kpath);
}

static int64_t lc_getcwd(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_EFAULT;
    return sys_getcwd((char *)a, (uint32_t)b) ? (int64_t)a : -LINUX_ENOENT;
}

static int64_t lc_mkdir(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_mkdir(kpath);
}

static int64_t lc_rmdir(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_rmdir(kpath);
}

static int64_t lc_unlink(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_unlink(kpath);
}

static int64_t lc_rename(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    char kpath2[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) || !copy_user_str(r, kpath2, b))
        return -LINUX_EFAULT;
    return sys_rename(kpath, kpath2);
}

static int64_t lc_chmod(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chmod(kpath, b);
}

static int64_t lc_mknod(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_mknod(kpath, (uint32_t)b, (uint32_t)c);
}

static int64_t lc_symlink(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char ktarget[MAX_PATH_LEN];
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, ktarget, a) || !copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_symlink(ktarget, kpath);
}

static int64_t lc_symlinkat(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a;
    char ktarget[MAX_PATH_LEN];
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, ktarget, b) || !copy_user_str(r, kpath, c))
        return -LINUX_EFAULT;
    return sys_symlink(ktarget, kpath);
}

static int64_t lc_mknodat(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a;
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_mknod(kpath, (uint32_t)c, (uint32_t)d);
}

static int64_t lc_access(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_access(kpath, (int32_t)b);
}

static int64_t lc_kill(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_kill((int)a, (int)b);
}

static int64_t lc_futex(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, 4, 0))
        return -LINUX_EFAULT;
    return sys_futex(a, b, c, d);
}

static int64_t lc_gettimeofday(struct X86_REGS *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    struct LINUX_TIMEVAL tv;
    tv.tv_sec = (int64_t)rtc_unix_time();
    tv.tv_usec = 0;
    if (a && !user_ptr_ok(r, a, sizeof(tv), 1))
        return -LINUX_EFAULT;
    if (b && !user_ptr_ok(r, b, 16, 1))
        return -LINUX_EFAULT;
    if (a)
        memcpy((void *)(uintptr_t)a, &tv, sizeof(tv));
    if (b)
        memset((void *)(uintptr_t)b, 0, 16);
    return 0;
}

static int64_t lc_nanosleep(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    struct LINUX_TIMESPEC req;
    memset(&req, 0, sizeof(req));
    if (b && !user_ptr_ok(r, b, sizeof(req), 0))
        return -LINUX_EFAULT;
    if (b)
        memcpy(&req, (const void *)(uintptr_t)b, sizeof(req));
    int64_t ms = req.tv_sec * 1000 + req.tv_nsec / 1000000;
    mtime_sleep(ms < 0 ? 0 : (uint32_t)ms);
    if (d) {
        if (!user_ptr_ok(r, d, sizeof(struct LINUX_TIMESPEC), 1))
            return -LINUX_EFAULT;
        struct LINUX_TIMESPEC rem;
        memset(&rem, 0, sizeof(rem));
        memcpy((void *)(uintptr_t)d, &rem, sizeof(rem));
    }
    return 0;
}

static int64_t lc_clock_gettime(struct X86_REGS *r, uint64_t a, uint64_t b,
                                uint64_t c, uint64_t d, uint64_t e,
                                uint64_t f) {
    struct LINUX_TIMESPEC ts;
    if ((int32_t)a == 0) {
        ts.tv_sec = (int64_t)rtc_unix_time();
        ts.tv_nsec = 0;
    } else {
        ts.tv_sec = (int64_t)(tick / PIT_HZ);
        ts.tv_nsec = (int64_t)((tick % PIT_HZ) * (1000000000 / PIT_HZ));
    }
    if (b && !user_ptr_ok(r, b, sizeof(ts), 1))
        return -LINUX_EFAULT;
    if (b)
        memcpy((void *)(uintptr_t)b, &ts, sizeof(ts));
    return 0;
}

static int64_t lc_clock_getres(struct X86_REGS *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_TIMESPEC), 1))
        return -LINUX_EFAULT;
    if (b) {
        struct LINUX_TIMESPEC res;
        res.tv_sec = 0;
        res.tv_nsec = 1000000000 / PIT_HZ;
        memcpy((void *)(uintptr_t)b, &res, sizeof(res));
    }
    return 0;
}

static int64_t lc_mprotect(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_mprotect(a, b, c);
}

static int64_t lc_rt_sigaction(struct X86_REGS *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    int32_t sig = (int32_t)a;
    if (sig <= 0 || sig >= 32)
        return -LINUX_EINVAL;
    struct LINUX_SIGACTION lsa;
    memset(&lsa, 0, sizeof(lsa));
    if (b && !user_ptr_ok(r, b, sizeof(lsa), 0)) {
        return -LINUX_EFAULT;
    }
    if (b)
        memcpy(&lsa, (const void *)(uintptr_t)b, sizeof(lsa));
    struct SYS_SIGACTION nat;
    memset(&nat, 0, sizeof(nat));
    nat.sa_handler = (void (*)(int))(uintptr_t)lsa.sa_handler;
    nat.sa_mask = (uint32_t)(lsa.sa_mask << 1);
    nat.sa_flags = (uint32_t)lsa.sa_flags;
    nat.sa_restorer = (void *)(uintptr_t)lsa.sa_restorer;
    if (d != 8)
        return -LINUX_EINVAL;
    struct SYS_SIGACTION oldnat;
    memset(&oldnat, 0, sizeof(oldnat));
    int rr = sys_sigaction(sig, b ? &nat : NULL, &oldnat);
    if (rr < 0)
        return rr;
    if (c) {
        if (!user_ptr_ok(r, c, sizeof(struct LINUX_SIGACTION), 1))
            return -LINUX_EFAULT;
        struct LINUX_SIGACTION oldl;
        memset(&oldl, 0, sizeof(oldl));
        oldl.sa_handler = (uint64_t)(uintptr_t)oldnat.sa_handler;
        oldl.sa_mask = (uint64_t)oldnat.sa_mask >> 1;
        oldl.sa_flags = oldnat.sa_flags;
        oldl.sa_restorer = (uint64_t)(uintptr_t)oldnat.sa_restorer;
        memcpy((void *)(uintptr_t)c, &oldl, sizeof(oldl));
    }
    return 0;
}

static int64_t lc_rt_sigprocmask(struct X86_REGS *r, uint64_t a, uint64_t b,
                                 uint64_t c, uint64_t d, uint64_t e,
                                 uint64_t f) {
    sigset_t kset = 0;
    if (b && !user_ptr_ok(r, b, 8, 0))
        return -LINUX_EFAULT;
    if (c && !user_ptr_ok(r, c, 8, 1))
        return -LINUX_EFAULT;
    if (b) {
        uint8_t in[8];
        memset(in, 0, sizeof(in));
        memcpy(in, (const void *)(uintptr_t)b, 8);
        memcpy(&kset, in, sizeof(kset));
        kset <<= 1;
    }
    sigset_t oset = 0;
    int32_t rr =
        sys_sigprocmask((int32_t)a, b ? &kset : NULL, c ? &oset : NULL);
    if (rr < 0)
        return rr;
    if (c) {
        uint8_t out[8];
        sigset_t mout = (sigset_t)oset >> 1;
        memset(out, 0, sizeof(out));
        memcpy(out, &mout, sizeof(mout));
        memcpy((void *)(uintptr_t)c, out, 8);
    }
    return 0;
}

static int64_t lc_getdents64(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return compat_getdents64((int32_t)a, (void *)(uintptr_t)b, (uint32_t)c);
}

static int64_t lc_ioctl(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_ioctl((int32_t)a, (uint32_t)b, c);
}

static int64_t lc_readv(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    return compat_readv((int32_t)a, (struct LINUX_IOVEC *)(uintptr_t)b,
                        (int32_t)c);
}

static int64_t lc_wait4(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (b && !user_ptr_ok(r, b, 4, 1))
        return -LINUX_EFAULT;
    return compat_wait4((int32_t *)b);
}

static int64_t lc_uname(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, sizeof(struct LINUX_UTSNAME), 1))
        return -LINUX_EFAULT;
    return compat_uname((void *)a);
}

static int64_t lc_sysinfo(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, sizeof(struct LINUX_SYSINFO), 1))
        return -LINUX_EFAULT;
    return compat_sysinfo((void *)a);
}

static int64_t lc_times(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (a && !user_ptr_ok(r, a, sizeof(struct LINUX_TMS), 1))
        return -LINUX_EFAULT;
    return (int64_t)(uint32_t)compat_times((void *)a);
}

static int64_t lc_ftruncate(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_ftruncate((int32_t)a, (int32_t)c);
}

static int64_t lc_rt_sigreturn(struct X86_REGS *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    return (int64_t)sys_sigreturn(r);
}

static int64_t lc_setpgid(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_setpgid(a, b);
}

static int64_t lc_getpgid(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_getpgid(a);
}

static int64_t lc_arch_prctl(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    const uint64_t MSR_FS_BASE = 0xC0000100;
    const uint64_t MSR_GS_BASE = 0xC0000101;
    struct TASK *cur = current;
    switch (a) {
    case 0x1002u:
        cur->tls_base = (uint32_t)b;
        cur->tls_msr = 1;
        asm_wrmsr(MSR_FS_BASE, b);
        return 0;
    case 0x1001u:
        cur->tls_base = (uint32_t)b;
        asm_wrmsr(MSR_GS_BASE, b);
        return 0;
    case 0x1003u:
        return (int64_t)(uint32_t)asm_rdmsr(MSR_FS_BASE);
    case 0x1004u:
        return (int64_t)(uint32_t)asm_rdmsr(MSR_GS_BASE);
    default:
        return -LINUX_EINVAL;
    }
}

static int64_t lc_sched_yield(struct X86_REGS *r, uint64_t a, uint64_t b,
                              uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return 0;
}

static int64_t lc_execve(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_execve(kpath, (const char **)(uintptr_t)b,
                      (const char **)(uintptr_t)c, r);
}

static int64_t lc_fork(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    return sys_fork(r);
}

static int64_t lc_clone(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)d;
    (void)f;
    uint32_t flags = (uint32_t)a;
    if ((flags & CLONE_VM) == 0)
        return sys_clone_ex(flags, (uint32_t)b, (uint32_t)e, r);
    if ((flags & CLONE_THREAD) != 0) {
        int64_t tpid = (int64_t)sys_clone_ex(flags, (uint32_t)b, (uint32_t)e, r);
        if (tpid > 0 && (flags & CLONE_PARENT_SETTID) != 0 && c != 0) {
            if (user_ptr_ok(r, c, 4, 1))
                *(volatile int32_t *)(uintptr_t)c = (int32_t)tpid;
        }
        return tpid;
    }
    int64_t pid = (int64_t)sys_clone_ex(flags & ~(uint32_t)CLONE_VM,
                                        (uint32_t)b, (uint32_t)e, r);
    if (pid > 0 && (flags & CLONE_PARENT_SETTID) != 0 && c != 0) {
        if (user_ptr_ok(r, c, 4, 1))
            *(volatile int32_t *)(uintptr_t)c = (int32_t)pid;
    }
    return pid;
}

static int64_t lc_pipe(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    if (a == 0 || !user_ptr_ok(r, a, 8, 1))
        return -LINUX_EFAULT;
    return sys_pipe((int32_t *)(uintptr_t)a);
}

static int64_t lc_pipe2(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    uint32_t flags = (uint32_t)b;
    if ((flags & ~(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK)) != 0)
        return -LINUX_EINVAL;
    int64_t rc = lc_pipe(r, a, 0, 0, 0, 0, 0);
    if (rc < 0)
        return rc;
    if (flags == 0)
        return 0;
    int32_t fds[2];
    if (!user_ptr_ok(r, a, 8, 0))
        return -LINUX_EFAULT;
    memcpy(fds, (const void *)(uintptr_t)a, sizeof(fds));
    for (int k = 0; k < 2; k++) {
        if (fds[k] < 0 || fds[k] >= (int32_t)MAX_FILES_OPEN_PER_PROC)
            continue;
        if ((flags & LINUX_O_CLOEXEC) != 0)
            current->fd_cloexec |= (1ull << fds[k]);
        if ((flags & LINUX_O_NONBLOCK) != 0) {
            struct FILE *pf = file_get(fd_local2global((uint32_t)fds[k]));
            if (pf != NULL)
                pf->fd_nonblock = 1;
        }
    }
    return 0;
}

static int64_t lc_dup(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                      uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_dup((int32_t)a);
}

static int64_t lc_dup2(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_dup2((int32_t)a, (int32_t)b);
}

static int64_t lc_dup3(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    if ((int32_t)a == (int32_t)b || d != 0)
        return -LINUX_EINVAL;
    return sys_dup2((int32_t)a, (int32_t)b);
}

static int64_t lc_open(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return compat_openat(LINUX_AT_FDCWD, kpath, (uint32_t)b);
}

static int64_t lc_openat(struct X86_REGS *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return compat_openat((int32_t)a, kpath, (uint32_t)c);
}

static int64_t lc_newfstatat(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, c, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    if ((b == 0 || *(const char *)(uintptr_t)b == 0) &&
        (d & LINUX_AT_EMPTY_PATH))
        return compat_fstat_linux((int32_t)a, c);
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return compat_stat_linux(kpath, c);
}

static int64_t lc_unlinkat(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return (d & LINUX_AT_REMOVEDIR) ? sys_rmdir(kpath) : sys_unlink(kpath);
}

static int64_t lc_mkdirat(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_mkdir(kpath);
}

static int64_t lc_renameat(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    char kpath2[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b) || !copy_user_str(r, kpath2, d))
        return -LINUX_EFAULT;
    return sys_rename(kpath, kpath2);
}

static int64_t lc_renameat2(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (e != 0)
        return -LINUX_EINVAL;
    return lc_renameat(r, b, d, 0, 0, 0, 0);
}

static int64_t lc_readlinkat(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b) || !user_ptr_ok(r, c, (uint32_t)d, 1))
        return -LINUX_EFAULT;
    return sys_readlink(kpath, (char *)(uintptr_t)c, d);
}

static int64_t lc_faccessat(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_access(kpath, (int32_t)c);
}

static int64_t lc_getrandom(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (b == 0)
        return 0;
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_EFAULT;
    uint8_t *p = (uint8_t *)(uintptr_t)a;
    uint32_t n = (uint32_t)b;
    while (n >= 8) {
        uint64_t v = rand_u64();
        memcpy(p, &v, 8);
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t v = rand_u64();
        memcpy(p, &v, n);
    }
    return (int64_t)b;
}

static int64_t lc0_open(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return open_file(kpath, (uint8_t)b);
}

#define EVFD_BASE 0x800
#define EVFD_MAX 16
#define TFD_BASE 0x880
#define TFD_MAX 8
#define EPFD_BASE 0xB00
#define EPFD_MAX 8
#define EP_MAX_ITEMS 32
#define LC_POLL_MAX 256
#define LC_SEL_MAX_BYTES 128u
#define LC_SEL_MAX_FDS (LC_SEL_MAX_BYTES * 8u)

struct LC_EPITEM {
    int32_t fd;
    uint32_t events;
    uint64_t data;
};

static struct {
    uint8_t active;
    uint8_t nonblock;
    uint8_t sema;
    uint8_t pad;
    uint64_t count;
} u_evfd[EVFD_MAX];

static struct {
    uint8_t active;
    uint8_t nonblock;
    int32_t clkid;
    uint64_t next_ms;
    uint64_t interval_ms;
    uint64_t expirations;
} u_tfd[TFD_MAX];

static struct {
    uint8_t active;
    uint8_t nonblock;
    int n;
    struct LC_EPITEM items[EP_MAX_ITEMS];
} u_ep[EPFD_MAX];

static uint64_t lc_now_ms(void) {
    return (uint64_t)tick * (uint64_t)(1000u / PIT_HZ);
}

static int evfd_slot(int fd) {
    int i = fd - EVFD_BASE;
    return (i >= 0 && i < EVFD_MAX && u_evfd[i].active) ? i : -1;
}

static int tfd_slot(int fd) {
    int i = fd - TFD_BASE;
    return (i >= 0 && i < TFD_MAX && u_tfd[i].active) ? i : -1;
}

static int ep_slot(int fd) {
    int i = fd - EPFD_BASE;
    return (i >= 0 && i < EPFD_MAX && u_ep[i].active) ? i : -1;
}

static int tfd_expired(int i) {
    if (u_tfd[i].next_ms == 0)
        return 0;
    return lc_now_ms() >= u_tfd[i].next_ms;
}

static int lc_close_extra(int32_t fd) {
    int i = evfd_slot(fd);
    if (i >= 0) {
        u_evfd[i].active = 0;
        return 1;
    }
    i = tfd_slot(fd);
    if (i >= 0) {
        u_tfd[i].active = 0;
        return 1;
    }
    i = ep_slot(fd);
    if (i >= 0) {
        u_ep[i].active = 0;
        u_ep[i].n = 0;
        return 1;
    }
    return 0;
}

static uint8_t *lc_nonblock_slot(int fd) {
    int i = evfd_slot(fd);
    if (i >= 0)
        return &u_evfd[i].nonblock;
    i = tfd_slot(fd);
    if (i >= 0)
        return &u_tfd[i].nonblock;
    i = ep_slot(fd);
    if (i >= 0)
        return &u_ep[i].nonblock;
    i = unix_fd_slot((uint64_t)fd);
    if (i >= 0)
        return &u_unix[i].nonblock;
    return 0;
}

static int io_is_file_fd(int fd) {
    if (fd < 0 || fd >= (int)MAX_FILES_OPEN_PER_PROC)
        return 0;
    uint32_t g = current->fd_table[fd];
    if (g == (uint32_t)-1)
        return 0;
    if (fd < 3 && g == (uint32_t)fd)
        return 0;
    return 1;
}

static int io_fd_events(int fd, int want_read, int want_write) {
    int rv = 0;
    int i;
    int want_err = want_read;

    if (fd < 0)
        return LINUX_POLLNVAL;

    i = evfd_slot(fd);
    if (i >= 0) {
        if (want_read && u_evfd[i].count > 0)
            rv |= LINUX_POLLIN;
        if (want_write && u_evfd[i].count < 0xFFFFFFFFFFFFFFFEull)
            rv |= LINUX_POLLOUT;
        return rv;
    }
    i = tfd_slot(fd);
    if (i >= 0) {
        if (want_read && tfd_expired(i))
            rv |= LINUX_POLLIN;
        if (want_write)
            rv |= LINUX_POLLOUT;
        return rv;
    }
    i = ep_slot(fd);
    if (i >= 0)
        return rv;
    i = unix_fd_slot((uint64_t)fd);
    if (i >= 0) {
        if (want_read && u_unix[i].count > 0)
            rv |= LINUX_POLLIN;
        if (u_unix[i].connected && !unix_peer_alive(i))
            rv |= LINUX_POLLHUP;
        if (want_write && unix_peer_alive(i))
            rv |= LINUX_POLLOUT;
        return rv;
    }
    if (fd < 3) {
        if (fd == 0) {
            if (want_read && TTY.avail() > 0)
                rv |= LINUX_POLLIN;
            if (want_write)
                rv |= LINUX_POLLOUT;
        } else {
            if (want_read)
                rv |= LINUX_POLLIN;
            if (want_write)
                rv |= LINUX_POLLOUT;
        }
        return rv;
    }
    if (io_is_file_fd(fd)) {
        if (is_pipe(fd)) {
            struct FILE *f = file_get(fd_local2global((uint32_t)fd));
            if (f != NULL && f->fd_inode != NULL) {
                uint32_t len = ioq_length((struct TTY_IOQUEUE *)f->fd_inode);
                if (want_read && len > 0)
                    rv |= LINUX_POLLIN;
                if (want_write && len < BUFSIZE)
                    rv |= LINUX_POLLOUT;
                if (len == 0 && want_read &&
                    ((current->pipe_wr_mask >> (uint32_t)fd) & 1u) == 0 &&
                    !pipe_has_writer(fd_local2global((uint32_t)fd)))
                    rv |= LINUX_POLLHUP;
            }
            return rv;
        }
        if (want_read)
            rv |= LINUX_POLLIN;
        if (want_write)
            rv |= LINUX_POLLOUT;
        return rv;
    }
    if (net_is_socket(fd)) {
        int nr = net_poll_ready(fd, want_read, want_write);
        if (want_err && (nr & LINUX_POLLERR))
            rv |= LINUX_POLLERR;
        rv |= nr & (LINUX_POLLIN | LINUX_POLLOUT | LINUX_POLLHUP | LINUX_POLLNVAL);
        return rv;
    }
    return LINUX_POLLNVAL;
}

static int io_wait(struct LINUX_POLLFD *fds, uint32_t n, int64_t timeout_ms) {
    uint64_t deadline = timeout_ms >= 0 ? lc_now_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (fds[i].fd < 0) {
                fds[i].revents = 0;
                continue;
            }
            int want_r = (fds[i].events & (LINUX_POLLIN | LINUX_POLLPRI)) != 0;
            int want_w = (fds[i].events & LINUX_POLLOUT) != 0;
            int rv = io_fd_events(fds[i].fd, want_r, want_w);
            fds[i].revents = (int16_t)rv;
            if (rv != 0)
                ready++;
        }
        if (ready > 0)
            return ready;
        if (timeout_ms >= 0 && lc_now_ms() >= deadline)
            return 0;
        mtime_sleep(1);
    }
}

static int64_t lc_poll_common(struct X86_REGS *r, uint64_t ufds, int32_t nfds,
                              int64_t timeout_ms) {
    static struct LINUX_POLLFD pf[LC_POLL_MAX];
    if (nfds < 0 || nfds > LC_POLL_MAX)
        return -LINUX_EINVAL;
    if (nfds == 0) {
        if (timeout_ms > 0)
            mtime_sleep((uint32_t)timeout_ms);
        return 0;
    }
    uint32_t bytes = (uint32_t)nfds * (uint32_t)sizeof(struct LINUX_POLLFD);
    if (!user_ptr_ok(r, ufds, bytes, 1))
        return -LINUX_EFAULT;
    memcpy(pf, (const void *)(uintptr_t)ufds, bytes);
    int rc = io_wait(pf, (uint32_t)nfds, timeout_ms);
    memcpy((void *)(uintptr_t)ufds, pf, bytes);
    return rc;
}

static int64_t lc_timespec_to_ms(struct X86_REGS *r, uint64_t ptr,
                                 int64_t *out) {
    if (ptr == 0) {
        *out = -1;
        return 0;
    }
    struct LINUX_TIMESPEC ts;
    if (!user_ptr_ok(r, ptr, sizeof(ts), 0))
        return -LINUX_EFAULT;
    memcpy(&ts, (const void *)(uintptr_t)ptr, sizeof(ts));
    *out = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (*out < 0)
        *out = 0;
    return 0;
}

static int64_t lc_poll(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)d; (void)e; (void)f;
    return lc_poll_common(r, a, (int32_t)b, (int32_t)c);
}

static int64_t lc_ppoll(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)d; (void)e; (void)f;
    int64_t tmo = -1;
    int64_t rc = lc_timespec_to_ms(r, c, &tmo);
    if (rc != 0)
        return rc;
    return lc_poll_common(r, a, (int32_t)b, tmo);
}

static int lc_fdset_test(const uint8_t *set, int fd) {
    if (fd < 0 || (uint32_t)fd >= LC_SEL_MAX_FDS)
        return 0;
    return (set[fd / 8] >> (fd % 8)) & 1;
}

static void lc_fdset_set(uint8_t *set, int fd) {
    if (fd < 0 || (uint32_t)fd >= LC_SEL_MAX_FDS)
        return;
    set[fd / 8] |= (uint8_t)(1u << (fd % 8));
}

static void lc_fdset_clear_high(uint8_t *set, uint32_t bytes, int nfds) {
    uint32_t bit = (uint32_t)nfds;
    if (bytes == 0)
        return;
    if (bit % 8) {
        set[bit / 8] &= (uint8_t)((1u << (bit % 8)) - 1u);
        bit = (bit + 7) / 8 * 8;
    }
    for (uint32_t b = bit / 8; b < bytes; b++)
        set[b] = 0;
}

static int64_t lc_select_common(struct X86_REGS *r, int32_t nfds, uint64_t rd,
                                uint64_t wr, uint64_t ex, int64_t timeout_ms) {
    static uint8_t sets[3][LC_SEL_MAX_BYTES];
    static struct LINUX_POLLFD pf[LC_SEL_MAX_FDS];
    if (nfds < 0 || nfds > (int32_t)LC_SEL_MAX_FDS)
        return -LINUX_EINVAL;
    uint32_t bytes = ((uint32_t)nfds + 7u) / 8u;
    uint64_t ptrs[3];
    ptrs[0] = rd;
    ptrs[1] = wr;
    ptrs[2] = ex;
    for (int k = 0; k < 3; k++) {
        memset(sets[k], 0, LC_SEL_MAX_BYTES);
        if (ptrs[k] == 0)
            continue;
        if (bytes != 0) {
            if (!user_ptr_ok(r, ptrs[k], bytes, 1))
                return -LINUX_EFAULT;
            memcpy(sets[k], (const void *)(uintptr_t)ptrs[k], bytes);
            lc_fdset_clear_high(sets[k], bytes, nfds);
        }
    }
    int n = 0;
    for (int fd = 0; fd < nfds && n < (int)LC_SEL_MAX_FDS; fd++) {
        int ev = 0;
        if (lc_fdset_test(sets[0], fd))
            ev |= LINUX_POLLIN;
        if (lc_fdset_test(sets[1], fd))
            ev |= LINUX_POLLOUT;
        if (lc_fdset_test(sets[2], fd))
            ev |= LINUX_POLLIN | LINUX_POLLPRI;
        if (ev == 0)
            continue;
        pf[n].fd = fd;
        pf[n].events = (int16_t)ev;
        pf[n].revents = 0;
        n++;
    }
    int rc = io_wait(pf, (uint32_t)n, timeout_ms);
    if (rc < 0)
        return rc;
    for (int k = 0; k < 3; k++) {
        if (ptrs[k] != 0 && bytes != 0)
            memset(sets[k], 0, bytes);
    }
    int badfd = 0;
    for (int i = 0; i < n; i++) {
        int fd = pf[i].fd;
        int rv = pf[i].revents;
        if (rv & LINUX_POLLNVAL)
            badfd = 1;
        if (rv & LINUX_POLLIN)
            lc_fdset_set(sets[0], fd);
        if (rv & LINUX_POLLOUT)
            lc_fdset_set(sets[1], fd);
        if (rv & (LINUX_POLLERR | LINUX_POLLHUP))
            lc_fdset_set(sets[2], fd);
    }
    for (int k = 0; k < 3; k++) {
        if (ptrs[k] != 0 && bytes != 0)
            memcpy((void *)(uintptr_t)ptrs[k], sets[k], bytes);
    }
    if (badfd && rc == 0)
        return -LINUX_EBADF;
    return rc;
}

static int64_t lc_select(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                         uint64_t d, uint64_t e, uint64_t f) {
    (void)f;
    int64_t timeout_ms = -1;
    if (e != 0) {
        struct LINUX_TIMEVAL tv;
        if (!user_ptr_ok(r, e, sizeof(tv), 0))
            return -LINUX_EFAULT;
        memcpy(&tv, (const void *)(uintptr_t)e, sizeof(tv));
        timeout_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (timeout_ms < 0)
            timeout_ms = 0;
    }
    return lc_select_common(r, (int32_t)a, b, c, d, timeout_ms);
}

static int64_t lc_pselect6(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)f;
    int64_t timeout_ms = -1;
    int64_t rc = lc_timespec_to_ms(r, e, &timeout_ms);
    if (rc != 0)
        return rc;
    return lc_select_common(r, (int32_t)a, b, c, d, timeout_ms);
}

static int64_t lc_epoll_create1(struct X86_REGS *r, uint64_t a, uint64_t b,
                                uint64_t c, uint64_t d, uint64_t e,
                                uint64_t f) {
    (void)r; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    for (int i = 0; i < EPFD_MAX; i++) {
        if (u_ep[i].active)
            continue;
        memset(&u_ep[i], 0, sizeof(u_ep[i]));
        u_ep[i].active = 1;
        return EPFD_BASE + i;
    }
    return -LINUX_ENFILE;
}

static int64_t lc_epoll_ctl(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)e; (void)f;
    int ep = ep_slot((int)a);
    if (ep < 0)
        return -LINUX_EBADF;
    int op = (int)b;
    int fd = (int)c;
    if (op == LINUX_EPOLL_CTL_DEL) {
        for (int i = 0; i < u_ep[ep].n; i++) {
            if (u_ep[ep].items[i].fd != fd)
                continue;
            u_ep[ep].items[i] = u_ep[ep].items[u_ep[ep].n - 1];
            u_ep[ep].n--;
            return 0;
        }
        return -LINUX_ENOENT;
    }
    struct LINUX_EPOLL_EVENT ev;
    if (d == 0 || !user_ptr_ok(r, d, sizeof(ev), 0))
        return -LINUX_EFAULT;
    memcpy(&ev, (const void *)(uintptr_t)d, sizeof(ev));
    if (op == LINUX_EPOLL_CTL_ADD) {
        for (int i = 0; i < u_ep[ep].n; i++) {
            if (u_ep[ep].items[i].fd == fd)
                return -LINUX_EEXIST;
        }
        if (u_ep[ep].n >= EP_MAX_ITEMS)
            return -LINUX_ENOSPC;
        u_ep[ep].items[u_ep[ep].n].fd = fd;
        u_ep[ep].items[u_ep[ep].n].events = ev.events;
        u_ep[ep].items[u_ep[ep].n].data = ev.data;
        u_ep[ep].n++;
        return 0;
    }
    if (op == LINUX_EPOLL_CTL_MOD) {
        for (int i = 0; i < u_ep[ep].n; i++) {
            if (u_ep[ep].items[i].fd != fd)
                continue;
            u_ep[ep].items[i].events = ev.events;
            u_ep[ep].items[i].data = ev.data;
            return 0;
        }
        return -LINUX_ENOENT;
    }
    return -LINUX_EINVAL;
}

static int64_t lc_epoll_wait_common(struct X86_REGS *r, uint64_t a, uint64_t b,
                                    uint64_t c, int64_t timeout_ms) {
    int ep = ep_slot((int)a);
    if (ep < 0)
        return -LINUX_EBADF;
    int32_t maxevents = (int32_t)c;
    if (maxevents <= 0)
        return -LINUX_EINVAL;
    if (b == 0 || !user_ptr_ok(r, b, (uint32_t)maxevents *
                                        (uint32_t)sizeof(struct LINUX_EPOLL_EVENT), 1))
        return -LINUX_EFAULT;
    uint64_t deadline = timeout_ms >= 0 ? lc_now_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        int out = 0;
        int n = u_ep[ep].n;
        for (int i = 0; i < n && out < maxevents; i++) {
            struct LC_EPITEM *it = &u_ep[ep].items[i];
            int want_r = (it->events & (LINUX_POLLIN | LINUX_POLLPRI)) != 0;
            int want_w = (it->events & LINUX_POLLOUT) != 0;
            int rv = io_fd_events(it->fd, want_r, want_w);
            if (rv == 0)
                continue;
            struct LINUX_EPOLL_EVENT ev;
            ev.events = (uint32_t)(rv & (it->events |
                                         LINUX_POLLERR | LINUX_POLLHUP));
            ev.data = it->data;
            memcpy((void *)(uintptr_t)(b +
                                       (uint64_t)out * sizeof(ev)),
                   &ev, sizeof(ev));
            out++;
        }
        if (out > 0)
            return out;
        if (timeout_ms >= 0 && lc_now_ms() >= deadline)
            return 0;
        mtime_sleep(1);
    }
}

static int64_t lc_epoll_wait(struct X86_REGS *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)e; (void)f;
    return lc_epoll_wait_common(r, a, b, c, (int32_t)d);
}

static int64_t lc_epoll_pwait(struct X86_REGS *r, uint64_t a, uint64_t b,
                              uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)e; (void)f;
    return lc_epoll_wait_common(r, a, b, c, (int32_t)d);
}

static int64_t lc_eventfd2(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    for (int i = 0; i < EVFD_MAX; i++) {
        if (u_evfd[i].active)
            continue;
        memset(&u_evfd[i], 0, sizeof(u_evfd[i]));
        u_evfd[i].active = 1;
        u_evfd[i].count = a;
        u_evfd[i].sema = (b & LINUX_EFD_SEMAPHORE) ? 1 : 0;
        u_evfd[i].nonblock = (b & LINUX_EFD_NONBLOCK) ? 1 : 0;
        return EVFD_BASE + i;
    }
    return -LINUX_ENFILE;
}

static int64_t lc_eventfd(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)b; (void)c; (void)d; (void)e; (void)f;
    return lc_eventfd2(r, a, 0, 0, 0, 0, 0);
}

static int64_t lc_timerfd_create(struct X86_REGS *r, uint64_t a, uint64_t b,
                                 uint64_t c, uint64_t d, uint64_t e,
                                 uint64_t f) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    for (int i = 0; i < TFD_MAX; i++) {
        if (u_tfd[i].active)
            continue;
        memset(&u_tfd[i], 0, sizeof(u_tfd[i]));
        u_tfd[i].active = 1;
        u_tfd[i].clkid = (int32_t)a;
        u_tfd[i].nonblock = (b & LINUX_TFD_NONBLOCK) ? 1 : 0;
        return TFD_BASE + i;
    }
    return -LINUX_ENFILE;
}

static int64_t lc_timerfd_settime(struct X86_REGS *r, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f) {
    (void)e; (void)f;
    int i = tfd_slot((int)a);
    if (i < 0)
        return -LINUX_EBADF;
    struct LINUX_ITIMERSPEC its;
    if (c == 0 || !user_ptr_ok(r, c, sizeof(its), 0))
        return -LINUX_EFAULT;
    memcpy(&its, (const void *)(uintptr_t)c, sizeof(its));
    uint64_t interval = (uint64_t)its.it_interval.tv_sec * 1000u +
                        (uint64_t)(its.it_interval.tv_nsec / 1000000);
    uint64_t value = (uint64_t)its.it_value.tv_sec * 1000u +
                     (uint64_t)(its.it_value.tv_nsec / 1000000);
    if (d != 0) {
        if (!user_ptr_ok(r, d, sizeof(its), 1))
            return -LINUX_EFAULT;
        struct LINUX_ITIMERSPEC old;
        memset(&old, 0, sizeof(old));
        if (u_tfd[i].next_ms != 0) {
            uint64_t now = lc_now_ms();
            uint64_t left = u_tfd[i].next_ms > now ? u_tfd[i].next_ms - now : 0;
            old.it_value.tv_sec = (int64_t)(left / 1000u);
            old.it_value.tv_nsec = (int64_t)((left % 1000u) * 1000000u);
        }
        old.it_interval.tv_sec = (int64_t)(u_tfd[i].interval_ms / 1000u);
        old.it_interval.tv_nsec =
            (int64_t)((u_tfd[i].interval_ms % 1000u) * 1000000u);
        memcpy((void *)(uintptr_t)d, &old, sizeof(old));
    }
    u_tfd[i].interval_ms = interval;
    u_tfd[i].expirations = 0;
    if (value == 0) {
        u_tfd[i].next_ms = 0;
    } else if (b & LINUX_TFD_TIMER_ABSTIME) {
        u_tfd[i].next_ms = value;
    } else {
        u_tfd[i].next_ms = lc_now_ms() + value;
    }
    return 0;
}

static int64_t lc_timerfd_gettime(struct X86_REGS *r, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    int i = tfd_slot((int)a);
    if (i < 0)
        return -LINUX_EBADF;
    if (b == 0 || !user_ptr_ok(r, b, sizeof(struct LINUX_ITIMERSPEC), 1))
        return -LINUX_EFAULT;
    struct LINUX_ITIMERSPEC its;
    memset(&its, 0, sizeof(its));
    if (u_tfd[i].next_ms != 0) {
        uint64_t now = lc_now_ms();
        uint64_t left = u_tfd[i].next_ms > now ? u_tfd[i].next_ms - now : 0;
        its.it_value.tv_sec = (int64_t)(left / 1000u);
        its.it_value.tv_nsec = (int64_t)((left % 1000u) * 1000000u);
    }
    its.it_interval.tv_sec = (int64_t)(u_tfd[i].interval_ms / 1000u);
    its.it_interval.tv_nsec =
        (int64_t)((u_tfd[i].interval_ms % 1000u) * 1000000u);
    memcpy((void *)(uintptr_t)b, &its, sizeof(its));
    return 0;
}

static int64_t lc_timerfd_tick(int i) {
    if (u_tfd[i].next_ms == 0 || lc_now_ms() < u_tfd[i].next_ms)
        return 0;
    uint64_t now = lc_now_ms();
    if (u_tfd[i].interval_ms == 0) {
        u_tfd[i].next_ms = 0;
        u_tfd[i].expirations++;
        return 1;
    }
    uint64_t missed = (now - u_tfd[i].next_ms) / u_tfd[i].interval_ms + 1;
    u_tfd[i].expirations += missed;
    u_tfd[i].next_ms += missed * u_tfd[i].interval_ms;
    return 1;
}

static int64_t lc_eventfd_read(int i, void *buf, uint32_t count) {
    if (count < 8)
        return -LINUX_EINVAL;
    while (u_evfd[i].count == 0) {
        if (u_evfd[i].nonblock)
            return -LINUX_EAGAIN;
        mtime_sleep(1);
    }
    uint64_t v = 1;
    if (!u_evfd[i].sema)
        v = u_evfd[i].count;
    u_evfd[i].count -= v;
    memcpy(buf, &v, 8);
    return 8;
}

static int64_t lc_eventfd_write(int i, const void *buf, uint32_t count) {
    if (count < 8)
        return -LINUX_EINVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == 0xFFFFFFFFFFFFFFFFull)
        return -LINUX_EINVAL;
    if (u_evfd[i].count > 0xFFFFFFFFFFFFFFFEull - v)
        return -LINUX_EAGAIN;
    u_evfd[i].count += v;
    return 8;
}

static int64_t lc_timerfd_read(int i, void *buf, uint32_t count) {
    if (count < 8)
        return -LINUX_EINVAL;
    while (!lc_timerfd_tick(i)) {
        if (u_tfd[i].nonblock)
            return -LINUX_EAGAIN;
        mtime_sleep(1);
    }
    uint64_t v = u_tfd[i].expirations;
    u_tfd[i].expirations = 0;
    memcpy(buf, &v, 8);
    return 8;
}

static void lc_fill_rlimit(uint64_t res, struct LINUX_RLIMIT *rl) {
    switch (res) {
    case LINUX_RLIMIT_NOFILE:
        rl->rlim_cur = 1024;
        rl->rlim_max = 4096;
        break;
    case LINUX_RLIMIT_STACK:
        rl->rlim_cur = 8u * 1024u * 1024u;
        rl->rlim_max = 8u * 1024u * 1024u;
        break;
    case LINUX_RLIMIT_DATA:
    case LINUX_RLIMIT_AS:
        rl->rlim_cur = 1ull << 32;
        rl->rlim_max = 1ull << 32;
        break;
    case LINUX_RLIMIT_NPROC:
        rl->rlim_cur = MAX_TASKS;
        rl->rlim_max = MAX_TASKS;
        break;
    default:
        rl->rlim_cur = 0xFFFFFFFFFFFFFFFFull;
        rl->rlim_max = 0xFFFFFFFFFFFFFFFFull;
        break;
    }
}

static int64_t lc_getrlimit(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    if (b == 0 || !user_ptr_ok(r, b, sizeof(struct LINUX_RLIMIT), 1))
        return -LINUX_EFAULT;
    struct LINUX_RLIMIT rl;
    lc_fill_rlimit(a, &rl);
    memcpy((void *)(uintptr_t)b, &rl, sizeof(rl));
    return 0;
}

static int64_t lc_setrlimit(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a; (void)c; (void)d; (void)e; (void)f;
    if (b == 0 || !user_ptr_ok(r, b, sizeof(struct LINUX_RLIMIT), 0))
        return -LINUX_EFAULT;
    return 0;
}

static int64_t lc_prlimit64(struct X86_REGS *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)e; (void)f;
    if (a != 0 && (int32_t)a != (int32_t)current->pid && (int32_t)a != -1)
        return -LINUX_EPERM;
    if (d != 0) {
        if (!user_ptr_ok(r, d, sizeof(struct LINUX_RLIMIT), 1))
            return -LINUX_EFAULT;
        struct LINUX_RLIMIT rl;
        lc_fill_rlimit(b, &rl);
        memcpy((void *)(uintptr_t)d, &rl, sizeof(rl));
    }
    return 0;
}

static int64_t lc_madvise(struct X86_REGS *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    if (a == 0)
        return -LINUX_EINVAL;
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_ENOMEM;
    return 0;
}

static int64_t lc_fsync(struct X86_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    if (evfd_slot((int)a) >= 0 || tfd_slot((int)a) >= 0 || ep_slot((int)a) >= 0)
        return -LINUX_EINVAL;
    if (a < 3)
        return 0;
    if (!io_is_file_fd((int)a))
        return -LINUX_EBADF;
    if (is_pipe((int)a))
        return -LINUX_EINVAL;
    return 0;
}

static int64_t lc_truncate(struct X86_REGS *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    if ((int64_t)b < 0)
        return -LINUX_EINVAL;
    return sys_truncate(kpath, (int32_t)b);
}

#define LC_TABLE_SIZE 320

static const LcFn LC_TABLE[LC_TABLE_SIZE] = {
    [SYS_LINUX_read] = lc_read,
    [SYS_LINUX_pread64] = lc_pread64,
    [SYS_LINUX_write] = lc_write,
    [SYS_LINUX_open] = lc_open,
    [SYS_LINUX_close] = lc_close,
    [SYS_LINUX_exit] = lc_exit,
    [SYS_LINUX_exit_group] = lc_exit_group,
    [SYS_LINUX_brk] = lc_brk,
    [SYS_LINUX_mmap] = lc_mmap,
    [SYS_LINUX_munmap] = lc_munmap,
    [SYS_LINUX_set_thread_area] = lc_set_thread_area,
    [SYS_LINUX_set_tid_address] = lc_set_tid_address,
    [SYS_LINUX_writev] = lc_writev,
    [SYS_LINUX_getpid] = lc_getpid,
    [SYS_LINUX_getppid] = lc_getppid,
    [SYS_LINUX_fstat] = lc_fstat,
    [SYS_LINUX_stat] = lc_stat,
    [SYS_LINUX_lstat] = lc_stat,
    [SYS_LINUX_lseek] = lc_lseek,
    [SYS_LINUX_fcntl] = lc_fcntl,
    [SYS_LINUX_readlink] = lc_readlink,
    [SYS_LINUX_chdir] = lc_chdir,
    [SYS_LINUX_getcwd] = lc_getcwd,
    [SYS_LINUX_mkdir] = lc_mkdir,
    [SYS_LINUX_rmdir] = lc_rmdir,
    [SYS_LINUX_unlink] = lc_unlink,
    [SYS_LINUX_rename] = lc_rename,
    [SYS_LINUX_chmod] = lc_chmod,
    [SYS_LINUX_fchmod] = lc_fchmod,
    [SYS_LINUX_fchmodat] = lc_fchmodat,
    [SYS_LINUX_chown] = lc_chown,
    [SYS_LINUX_fchown] = lc_fchown,
    [SYS_LINUX_lchown] = lc_lchown,
    [SYS_LINUX_fchownat] = lc_fchownat,
    [SYS_LINUX_getuid] = lc_getuid,
    [SYS_LINUX_getgid] = lc_getgid,
    [SYS_LINUX_geteuid] = lc_geteuid,
    [SYS_LINUX_getegid] = lc_getegid,
    [SYS_LINUX_setuid] = lc_setuid,
    [SYS_LINUX_setgid] = lc_setgid,
    [SYS_LINUX_setreuid] = lc_setreuid,
    [SYS_LINUX_setregid] = lc_setregid,
    [SYS_LINUX_setresuid] = lc_setresuid,
    [SYS_LINUX_setresgid] = lc_setresgid,
    [SYS_LINUX_getresuid] = lc_getresuid,
    [SYS_LINUX_getresgid] = lc_getresgid,
    [SYS_LINUX_getgroups] = lc_getgroups,
    [SYS_LINUX_setgroups] = lc_setgroups,
    [SYS_LINUX_symlink] = lc_symlink,
    [SYS_LINUX_symlinkat] = lc_symlinkat,
    [SYS_LINUX_mknod] = lc_mknod,
    [SYS_LINUX_mknodat] = lc_mknodat,
    [SYS_LINUX_access] = lc_access,
    [SYS_LINUX_kill] = lc_kill,
    [SYS_LINUX_futex] = lc_futex,
    [SYS_LINUX_gettimeofday] = lc_gettimeofday,
    [SYS_LINUX_nanosleep] = lc_nanosleep,
    [SYS_LINUX_clock_gettime] = lc_clock_gettime,
    [SYS_LINUX_clock_getres] = lc_clock_getres,
    [SYS_LINUX_mprotect] = lc_mprotect,
    [SYS_LINUX_rt_sigaction] = lc_rt_sigaction,
    [SYS_LINUX_rt_sigprocmask] = lc_rt_sigprocmask,
    [SYS_LINUX_getdents64] = lc_getdents64,
    [SYS_LINUX_ioctl] = lc_ioctl,
    [SYS_LINUX_readv] = lc_readv,
    [SYS_LINUX_wait4] = lc_wait4,
    [SYS_LINUX_uname] = lc_uname,
    [SYS_LINUX_sysinfo] = lc_sysinfo,
    [SYS_LINUX_times] = lc_times,
    [SYS_LINUX_ftruncate] = lc_ftruncate,
    [SYS_LINUX_rt_sigreturn] = lc_rt_sigreturn,
    [SYS_LINUX_setpgid] = lc_setpgid,
    [SYS_LINUX_setsid] = lc_setsid,
    [SYS_LINUX_sigaltstack] = lc_sigaltstack,
    [SYS_LINUX_rt_sigsuspend] = lc_sigsuspend,
    [SYS_LINUX_getitimer] = lc_getitimer,
    [SYS_LINUX_setitimer] = lc_setitimer,
    [SYS_LINUX_getrusage] = lc_getrusage,
    [SYS_LINUX_statfs] = lc_statfs,
    [SYS_LINUX_fstatfs] = lc_fstatfs,
    [SYS_LINUX_waitid] = lc_waitid,
    [SYS_LINUX_socket] = lc_socket,
    [SYS_LINUX_connect] = lc_connect,
    [SYS_LINUX_accept] = lc_accept,
    [SYS_LINUX_sendto] = lc_sendto,
    [SYS_LINUX_recvfrom] = lc_recvfrom,
    [SYS_LINUX_sendmsg] = lc_sendmsg,
    [SYS_LINUX_recvmsg] = lc_recvmsg,
    [SYS_LINUX_shutdown] = lc_shutdown,
    [SYS_LINUX_bind] = lc_bind,
    [SYS_LINUX_listen] = lc_listen,
    [SYS_LINUX_getsockname] = lc_getsockname,
    [SYS_LINUX_getpeername] = lc_getpeername,
    [SYS_LINUX_socketpair] = lc_socketpair,
    [SYS_LINUX_setsockopt] = lc_setsockopt,
    [SYS_LINUX_getsockopt] = lc_getsockopt,
    [SYS_LINUX_tkill] = lc_tkill,
    [SYS_LINUX_umask] = lc_umask,
    [SYS_LINUX_tgkill] = lc_tgkill,
    [SYS_LINUX_getsid] = lc_getsid,
    [SYS_LINUX_getpgid] = lc_getpgid,
    [SYS_LINUX_arch_prctl] = lc_arch_prctl,
    [SYS_LINUX_sched_yield] = lc_sched_yield,
    [SYS_LINUX_execve] = lc_execve,
    [SYS_LINUX_fork] = lc_fork,
    [SYS_LINUX_vfork] = lc_fork,
    [SYS_LINUX_clone] = lc_clone,
    [SYS_LINUX_pipe] = lc_pipe,
    [SYS_LINUX_pipe2] = lc_pipe2,
    [SYS_LINUX_dup] = lc_dup,
    [SYS_LINUX_dup2] = lc_dup2,
    [SYS_LINUX_dup3] = lc_dup3,
    [SYS_LINUX_openat] = lc_openat,
    [SYS_LINUX_newfstatat] = lc_newfstatat,
    [SYS_LINUX_unlinkat] = lc_unlinkat,
    [SYS_LINUX_mkdirat] = lc_mkdirat,
    [SYS_LINUX_renameat] = lc_renameat,
    [SYS_LINUX_renameat2] = lc_renameat2,
    [SYS_LINUX_readlinkat] = lc_readlinkat,
    [SYS_LINUX_faccessat] = lc_faccessat,
    [SYS_LINUX_getrandom] = lc_getrandom,
    [SYS_LINUX_poll] = lc_poll,
    [SYS_LINUX_ppoll] = lc_ppoll,
    [SYS_LINUX_select] = lc_select,
    [SYS_LINUX_pselect6] = lc_pselect6,
    [SYS_LINUX_epoll_create] = lc_epoll_create1,
    [SYS_LINUX_epoll_create1] = lc_epoll_create1,
    [SYS_LINUX_epoll_ctl] = lc_epoll_ctl,
    [SYS_LINUX_epoll_wait] = lc_epoll_wait,
    [SYS_LINUX_epoll_pwait] = lc_epoll_pwait,
    [SYS_LINUX_eventfd] = lc_eventfd,
    [SYS_LINUX_eventfd2] = lc_eventfd2,
    [SYS_LINUX_timerfd_create] = lc_timerfd_create,
    [SYS_LINUX_timerfd_settime] = lc_timerfd_settime,
    [SYS_LINUX_timerfd_gettime] = lc_timerfd_gettime,
    [SYS_LINUX_getrlimit] = lc_getrlimit,
    [SYS_LINUX_setrlimit] = lc_setrlimit,
    [SYS_LINUX_prlimit64] = lc_prlimit64,
    [SYS_LINUX_madvise] = lc_madvise,
    [SYS_LINUX_fsync] = lc_fsync,
    [SYS_LINUX_fdatasync] = lc_fsync,
    [SYS_LINUX_truncate] = lc_truncate,
};

int64_t linux_compat_handler(struct X86_REGS *r) {
    struct TASK *cur = current;
    uint32_t nr = r->eax;
    int64_t ret = -LINUX_ENOSYS;
    int32_t saved_errno = cur->errno;

    cur->errno = 0;
    if (nr >= COMPAT_SYSCALL_BASE) {
        static const LcFn LC0_TABLE[] = {
            [0] = lc_getpid,  [1] = lc_write, [2] = lc_read,
            [3] = lc_exit,    [4] = lc_brk,   [5] = lc0_open,
            [6] = lc_close,   [7] = lc_mmap,  [8] = lc_set_thread_area,
            [9] = lc0_writev,
        };
        uint32_t idx = nr - COMPAT_SYSCALL_BASE;
        if (idx < sizeof(LC0_TABLE) / sizeof(LC0_TABLE[0]) && LC0_TABLE[idx])
            ret = LC0_TABLE[idx](r, r->rbx, r->rcx, r->rdx, r->rsi, r->rdi,
                                 r->rbp);
    } else if (nr < LC_TABLE_SIZE && LC_TABLE[nr]) {
        ret = LC_TABLE[nr](r, r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
    }

    if (ret == -1 && cur->errno > 0)
        ret = -(int64_t)cur->errno;
    lc_seterrno(cur, ret < 0 ? (int32_t)-ret : saved_errno);
    return ret;
}
