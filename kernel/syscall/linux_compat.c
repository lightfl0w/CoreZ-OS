#include "kernel/syscall/linux_compat.h"
#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/tty.h"
#include "kernel/asmFunc.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
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
int32_t sys_clock_gettime(int32_t clk_id, struct timespec *tp);
int32_t sys_gettimeofday(struct timeval *tv, void *tz);
int32_t sys_nanosleep(const struct timespec *req, struct timespec *rem);
int32_t sys_wait(int32_t *status);
int32_t sys_sigaction(int sig, const struct sigaction *act,
                      struct sigaction *old);
int32_t sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
uint64_t sys_sigreturn(struct Registers *r);

static int32_t compat_read(int32_t fd, void *buf, uint32_t count);
static int32_t compat_write(int32_t fd, const void *buf, uint32_t count);


#define DIRF_FLAG 0xFFFEu

static int32_t compat_dir_fd(const char *path) {
    uint32_t ino = 0;
    int is_dir = 0;
    if (ext2_lookup(path, &ino, &is_dir) || !is_dir)
        return -1;
    int gfd = file_table_alloc_slot();
    if (gfd < 0)
        return -1;
    struct file *f = file_get((uint32_t)gfd);
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
    struct file *pf = file_get(gfd);
    return pf != NULL && pf->fd_flag == DIRF_FLAG;
}

static int32_t compat_getdents64(int32_t fd, void *dirp, uint32_t count) {
    if (dirp == NULL || fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    if (!compat_fd_isdir(fd))
        return -LINUX_ENOTDIR;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct file *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    uint32_t pos = pf->fd_pos;
    uint32_t emitted = pos;
    uint32_t written = 0;
    struct dir_entry de;
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
        d->d_type = (de.f_type == FT_DIRECTORY) ? LINUX_DT_DIR : LINUX_DT_REG;
        memcpy(d->d_name, de.filename, nl + 1);
        written += reclen;
        emitted = pos;
    }
    pf->fd_pos = emitted;
    return (int32_t)written;
}

static int32_t compat_ioctl(int32_t fd, uint32_t cmd, uint64_t arg) {
    if (fd >= 0 && fd <= 2) {
        uint32_t native = 0;
        switch (cmd) {
        case LINUX_TCGETS:
            native = TTY_IOCTL_TCGETS;
            break;
        case LINUX_TCSETS:
            native = TTY_IOCTL_TCSETS;
            break;
        case LINUX_TIOCGWINSZ:
            native = TTY_IOCTL_TIOCGWINSZ;
            break;
        case LINUX_FIONREAD:
            native = TTY_IOCTL_FIONREAD;
            break;
        default:
            return -LINUX_ENOTTY;
        }
        int32_t rc = TTY.ioctl(native, arg);
        return rc < 0 ? -LINUX_ENOTTY : rc;
    }
    return -LINUX_ENOTTY;
}

static int32_t compat_setpgid(uint32_t pid, uint32_t pgid) {
    if (pid >= MAX_TASKS || pgid >= MAX_TASKS)
        return -LINUX_EINVAL;
    struct task_struct *t = pid2thread((int32_t)pid);
    if (t == NULL || t->status == TASK_DIED)
        return -LINUX_ESRCH;
    uint32_t want = pgid ? pgid : pid;
    if (want >= MAX_TASKS)
        return -LINUX_EINVAL;
    struct task_struct *g = pid2thread((int32_t)want);
    if (g == NULL)
        return -LINUX_EPERM;
    t->pid = want;
    return 0;
}

static int32_t compat_getpgid(uint32_t pid) {
    if (pid >= MAX_TASKS)
        return -LINUX_EINVAL;
    struct task_struct *t = pid2thread((int32_t)pid);
    if (t == NULL)
        return -LINUX_ESRCH;
    return (int32_t)t->pid;
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
    memcpy(u.version, "#1 NitianOS SMP", 16);
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
    uint32_t used = 0;
    for (uint32_t i = 0; i < kernel_pool.pool_bitmap.btmp_bytes_len * 8; i++) {
        if (bitmap_scan_test(&kernel_pool.pool_bitmap, i))
            used++;
    }
    si.freeram = si.totalram - (uint64_t)used * PAGE_SIZE;
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
    struct file *pf = file_get(gfd);
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
    return LINUX_S_IFREG | 0644u;
}

static void compat_stat_fill(struct LINUX_STAT *ls, uint32_t ino, int64_t size,
                             uint32_t mode) {
    memset(ls, 0, sizeof(*ls));
    ls->st_dev = 0x800u;
    ls->st_ino = ino;
    ls->st_nlink = 1;
    ls->st_mode = mode;
    ls->st_blksize = 512;
    ls->st_blocks = (int64_t)((size + 511) / 512);
    ls->st_size = size;
    ls->st_atim.tv_sec = (int64_t)(tick / PIT_HZ);
    ls->st_mtim = ls->st_atim;
    ls->st_ctim = ls->st_atim;
}

static int32_t compat_stat_linux(const char *path, uint64_t ub) {
    struct stat st;
    struct LINUX_STAT ls;
    if (sys_stat(path, &st) != 0)
        return -LINUX_ENOENT;
    compat_stat_fill(&ls, st.st_ino, (int64_t)st.st_size,
                     compat_mode_native(st.st_filetype));
    memcpy((void *)(uintptr_t)ub, &ls, sizeof(ls));
    return 0;
}

static int32_t compat_fstat_linux(int32_t fd, uint64_t ub) {
    struct LINUX_STAT ls;
    if (fd >= 0 && fd < 3) {
        compat_stat_fill(&ls, 0, 0, LINUX_S_IFCHR | 0600u);
    } else if (compat_fd_isdir(fd)) {
        uint32_t gfd = fd_local2global((uint32_t)fd);
        struct file *pf = file_get(gfd);
        compat_stat_fill(&ls, pf->fd_inode->i_no, (int64_t)pf->fd_inode->i_size,
                         LINUX_S_IFDIR | 0755u);
    } else if (is_pipe(fd)) {
        compat_stat_fill(&ls, 0, 0, LINUX_S_IFIFO | 0600u);
    } else {
        struct stat st;
        if (sys_fstat(fd, &st) != 0)
            return -LINUX_EBADF;
        compat_stat_fill(&ls, st.st_ino, (int64_t)st.st_size,
                         compat_mode_native(st.st_filetype));
    }
    memcpy((void *)(uintptr_t)ub, &ls, sizeof(ls));
    return 0;
}

static int32_t compat_openat(int32_t dirfd, const char *kpath,
                             uint32_t lflags) {
    if (dirfd != LINUX_AT_FDCWD)
        return -LINUX_EINVAL;
    struct stat pst;
    uint32_t ino = 0;
    int is_dir = 0;
    if (kpath[0] == '/' && ext2_lookup(kpath, &ino, &is_dir) == 0 && is_dir) {
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
        return -LINUX_ENOENT;
    if (lflags & LINUX_O_TRUNC)
        compat_ftruncate(fd, 0);
    if (lflags & LINUX_O_APPEND)
        sys_lseek(fd, 0, (uint8_t)SEEK_END);
    return fd;
}


static void lc_seterrno(struct task_struct *cur, int32_t val) {
    cur->errno = val;
    if (cur->tls_selector == SELECTOR_TLS && cur->tls_base != 0) {
        *(volatile int32_t *)cur->tls_base = val;
    }
}

static int32_t compat_write(int32_t fd, const void *buf, uint32_t count) {
    if (fd < 0)
        return -1;
    if (compat_fd_isdir(fd))
        return -LINUX_EISDIR;
    if (is_pipe(fd))
        return (int32_t)pipe_write(fd, buf, count);
    if (fd == 1 || fd == 2)
        return TTY.write((const char *)buf, count);
    const char *s = (const char *)buf;
    for (uint32_t i = 0; i < count; i++) {
        console_putc(s[i]);
    }
    return (int32_t)count;
}

static int32_t compat_read(int32_t fd, void *buf, uint32_t count) {
    if (fd == 0)
        return TTY.read((char *)buf, count);
    if (compat_fd_isdir(fd))
        return -LINUX_EISDIR;
    if (is_pipe(fd))
        return (int32_t)pipe_read(fd, buf, count);
    if (fd >= 0 && fd < 3)
        return -1;
    return (int32_t)read_file(fd, buf, count);
}

static int32_t compat_set_thread_area(uint32_t base) {
    if (base == 0 || !user_range_writable(base, sizeof(int32_t)))
        return -LINUX_EFAULT;
    struct task_struct *cur = current;
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

static int user_ptr_ok(struct Registers *r, uint64_t ptr, uint32_t len,
                       int wr) {
    return (r->cs & 3) != 3 || access_ok((const void *)(uintptr_t)ptr, len, wr);
}

static int copy_user_str(struct Registers *r, char *dst, uint64_t ptr) {
    return (r->cs & 3) != 3 ||
           copy_str_from_user(dst, (const char *)(uintptr_t)ptr,
                              MAX_PATH_LEN) == 0;
}

typedef int64_t (*LcFn)(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f);

static int64_t lc_getpid(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return (int64_t)current->pid;
}

static int64_t lc_getppid(struct Registers *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    struct task_struct *cur = current;
    return cur->parent_pid >= 0 ? (int64_t)cur->parent_pid : 0;
}

static int64_t lc_getid(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return 0;
}

static int64_t lc_write(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c, 0))
        return -LINUX_EFAULT;
    int32_t n = compat_write((int32_t)a, (const void *)b, (uint32_t)c);
    return n < 0 ? -n : n;
}

static int64_t lc_read(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    int32_t n = compat_read((int32_t)a, (void *)b, (uint32_t)c);
    return n < 0 ? -n : n;
}

static int64_t lc_close(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return close_file((int32_t)a);
}

static int64_t lc_exit(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    sys_exit((int32_t)a);
    return 0;
}

__attribute__((noreturn)) static int64_t lc_exit_group(struct Registers *r,
                                                       uint64_t a, uint64_t b,
                                                       uint64_t c, uint64_t d,
                                                       uint64_t e, uint64_t f) {
    (void)r;
    sys_exit((int32_t)a);
    for (;;) {
    }
}

static int64_t lc_brk(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                      uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return (int64_t)sys_brk((uint32_t)a);
}

static int64_t lc_mmap(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    struct mmap_args m = {(uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d,
                          (uint32_t)e, (uint32_t)(f & ~(uint32_t)(PAGE_SIZE - 1u))};
    return (int64_t)sys_mmap(&m);
}

static int64_t lc_munmap(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_munmap((uint32_t)a, (uint32_t)b);
}

static int64_t lc_set_thread_area(struct Registers *r, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f) {
    (void)r;
    return compat_set_thread_area((uint32_t)a);
}

static int64_t lc_set_tid_address(struct Registers *r, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f) {
    (void)r;
    struct task_struct *cur = current;
    if (a)
        *(volatile int32_t *)a = (int32_t)cur->pid;
    return (int64_t)cur->pid;
}

static int64_t lc_writev(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    int32_t n =
        sys_compat_writev((int32_t)a, (struct LINUX_IOVEC *)b, (int32_t)c);
    return n < 0 ? -n : n;
}

static int64_t lc0_writev(struct Registers *r, uint64_t a, uint64_t b,
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

static int64_t lc_fstat(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_fstat_linux((int32_t)a, b);
}

static int64_t lc_stat(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) ||
        !user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_stat_linux(kpath, b);
}

static int64_t lc_lseek(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_lseek((int32_t)a, (int32_t)b, (uint8_t)(c + 1u));
}

static int64_t lc_fcntl(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_fcntl((int32_t)a, (int32_t)b, c);
}

static int64_t lc_readlink(struct Registers *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) || !user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return sys_readlink(kpath, (char *)b, c);
}

static int64_t lc_chdir(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chdir(kpath);
}

static int64_t lc_getcwd(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_EFAULT;
    return sys_getcwd((char *)a, (uint32_t)b) ? (int64_t)a : -LINUX_ENOENT;
}

static int64_t lc_mkdir(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_mkdir(kpath);
}

static int64_t lc_rmdir(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_rmdir(kpath);
}

static int64_t lc_unlink(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_unlink(kpath);
}

static int64_t lc_rename(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    char kpath2[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) || !copy_user_str(r, kpath2, b))
        return -LINUX_EFAULT;
    return sys_rename(kpath, kpath2);
}

static int64_t lc_chmod(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chmod(kpath, b);
}

static int64_t lc_access(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_access(kpath, (int32_t)b);
}

static int64_t lc_kill(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_kill((int)a, (int)b);
}

static int64_t lc_futex(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, a, 4, 0))
        return -LINUX_EFAULT;
    return sys_futex(a, b, c, d);
}

static int64_t lc_gettimeofday(struct Registers *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    struct LINUX_TIMEVAL tv;
    tv.tv_sec = (int64_t)(tick / PIT_HZ);
    tv.tv_usec = (int64_t)((tick % PIT_HZ) * (1000000 / PIT_HZ));
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

static int64_t lc_nanosleep(struct Registers *r, uint64_t a, uint64_t b,
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

static int64_t lc_clock_gettime(struct Registers *r, uint64_t a, uint64_t b,
                                uint64_t c, uint64_t d, uint64_t e,
                                uint64_t f) {
    struct LINUX_TIMESPEC ts;
    ts.tv_sec = (int64_t)(tick / PIT_HZ);
    ts.tv_nsec = (int64_t)((tick % PIT_HZ) * (1000000000 / PIT_HZ));
    if (b && !user_ptr_ok(r, b, sizeof(ts), 1))
        return -LINUX_EFAULT;
    if (b)
        memcpy((void *)(uintptr_t)b, &ts, sizeof(ts));
    return 0;
}

static int64_t lc_clock_getres(struct Registers *r, uint64_t a, uint64_t b,
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

static int64_t lc_mprotect(struct Registers *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_mprotect(a, b, c);
}

static int64_t lc_rt_sigaction(struct Registers *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    int32_t sig = (int32_t)a;
    if (sig <= 0 || sig >= 32)
        return -LINUX_EINVAL;
    struct LINUX_SIGACTION lsa;
    memset(&lsa, 0, sizeof(lsa));
    if (b && !user_ptr_ok(r, b, sizeof(lsa), 0))
        return -LINUX_EFAULT;
    if (b)
        memcpy(&lsa, (const void *)(uintptr_t)b, sizeof(lsa));
    struct sigaction nat;
    memset(&nat, 0, sizeof(nat));
    nat.sa_handler = (void (*)(int))(uintptr_t)lsa.sa_handler;
    nat.sa_mask = (uint32_t)lsa.sa_mask;
    nat.sa_flags = (uint32_t)lsa.sa_flags;
    nat.sa_restorer = (void *)(uintptr_t)lsa.sa_restorer;
    struct sigaction oldnat;
    memset(&oldnat, 0, sizeof(oldnat));
    int rr = sys_sigaction(sig, b ? &nat : NULL, &oldnat);
    if (rr < 0)
        return rr;
    if (d) {
        if (!user_ptr_ok(r, d, sizeof(struct LINUX_SIGACTION), 1))
            return -LINUX_EFAULT;
        struct LINUX_SIGACTION oldl;
        memset(&oldl, 0, sizeof(oldl));
        oldl.sa_handler = (uint64_t)(uintptr_t)oldnat.sa_handler;
        oldl.sa_flags = oldnat.sa_flags;
        oldl.sa_restorer = (uint64_t)(uintptr_t)oldnat.sa_restorer;
        oldl.sa_mask = oldnat.sa_mask;
        memcpy((void *)(uintptr_t)d, &oldl, sizeof(oldl));
    }
    return 0;
}

static int64_t lc_rt_sigprocmask(struct Registers *r, uint64_t a, uint64_t b,
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
    }
    sigset_t oset = 0;
    int32_t rr =
        sys_sigprocmask((int32_t)a, b ? &kset : NULL, c ? &oset : NULL);
    if (rr < 0)
        return rr;
    if (c) {
        uint8_t out[8];
        memset(out, 0, sizeof(out));
        memcpy(out, &oset, sizeof(oset));
        memcpy((void *)(uintptr_t)c, out, 8);
    }
    return 0;
}

static int64_t lc_getdents64(struct Registers *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return compat_getdents64((int32_t)a, (void *)(uintptr_t)b, (uint32_t)c);
}

static int64_t lc_ioctl(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_ioctl((int32_t)a, (uint32_t)b, c);
}

static int64_t lc_readv(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (!user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    return compat_readv((int32_t)a, (struct LINUX_IOVEC *)(uintptr_t)b,
                        (int32_t)c);
}

static int64_t lc_wait4(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (b && !user_ptr_ok(r, b, 4, 1))
        return -LINUX_EFAULT;
    return compat_wait4((int32_t *)b);
}

static int64_t lc_uname(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_uname((void *)a);
}

static int64_t lc_sysinfo(struct Registers *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_sysinfo((void *)a);
}

static int64_t lc_times(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return (int64_t)(uint32_t)compat_times((void *)a);
}

static int64_t lc_ftruncate(struct Registers *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_ftruncate((int32_t)a, (int32_t)c);
}

static int64_t lc_rt_sigreturn(struct Registers *r, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    return (int64_t)sys_sigreturn(r);
}

static int64_t lc_setpgid(struct Registers *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_setpgid(a, b);
}

static int64_t lc_getpgid(struct Registers *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return compat_getpgid(a);
}

static int64_t lc_arch_prctl(struct Registers *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    const uint64_t MSR_FS_BASE = 0xC0000100;
    const uint64_t MSR_GS_BASE = 0xC0000101;
    struct task_struct *cur = current;
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

static int64_t lc_sched_yield(struct Registers *r, uint64_t a, uint64_t b,
                              uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return 0;
}

static int64_t lc_execve(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_execve(kpath, (const char **)(uintptr_t)b,
                      (const char **)(uintptr_t)c, r);
}

static int64_t lc_fork(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    return sys_fork(r);
}

static int64_t lc_clone(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (a & CLONE_VM)
        return -LINUX_ENOSYS;
    return sys_fork(r);
}

static int64_t lc_pipe(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    if (a == 0 || !user_ptr_ok(r, a, 8, 1))
        return -LINUX_EFAULT;
    return sys_pipe((int32_t *)(uintptr_t)a);
}

static int64_t lc_pipe2(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    if (b != 0)
        return -LINUX_EINVAL;
    return lc_pipe(r, a, 0, 0, 0, 0, 0);
}

static int64_t lc_dup(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                      uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_dup((int32_t)a);
}

static int64_t lc_dup2(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    (void)r;
    return sys_dup2((int32_t)a, (int32_t)b);
}

static int64_t lc_dup3(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    if ((int32_t)a == (int32_t)b || d != 0)
        return -LINUX_EINVAL;
    return sys_dup2((int32_t)a, (int32_t)b);
}

static int64_t lc_open(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return compat_openat(LINUX_AT_FDCWD, kpath, (uint32_t)b);
}

static int64_t lc_openat(struct Registers *r, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return compat_openat((int32_t)a, kpath, (uint32_t)c);
}

static int64_t lc_newfstatat(struct Registers *r, uint64_t a, uint64_t b,
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

static int64_t lc_unlinkat(struct Registers *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return (d & LINUX_AT_REMOVEDIR) ? sys_rmdir(kpath) : sys_unlink(kpath);
}

static int64_t lc_mkdirat(struct Registers *r, uint64_t a, uint64_t b,
                          uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_mkdir(kpath);
}

static int64_t lc_renameat(struct Registers *r, uint64_t a, uint64_t b,
                           uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    char kpath2[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b) || !copy_user_str(r, kpath2, d))
        return -LINUX_EFAULT;
    return sys_rename(kpath, kpath2);
}

static int64_t lc_renameat2(struct Registers *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    if (e != 0)
        return -LINUX_EINVAL;
    return lc_renameat(r, b, d, 0, 0, 0, 0);
}

static int64_t lc_readlinkat(struct Registers *r, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b) || !user_ptr_ok(r, c, (uint32_t)d, 1))
        return -LINUX_EFAULT;
    return sys_readlink(kpath, (char *)(uintptr_t)c, d);
}

static int64_t lc_faccessat(struct Registers *r, uint64_t a, uint64_t b,
                            uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_access(kpath, (int32_t)c);
}

static int64_t lc_getrandom(struct Registers *r, uint64_t a, uint64_t b,
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

static int64_t lc0_open(struct Registers *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return open_file(kpath, (uint8_t)b);
}

#define LC_TABLE_SIZE 320

static const LcFn LC_TABLE[LC_TABLE_SIZE] = {
    [SYS_LINUX_read] = lc_read,
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
    [SYS_LINUX_getuid] = lc_getid,
    [SYS_LINUX_getgid] = lc_getid,
    [SYS_LINUX_geteuid] = lc_getid,
    [SYS_LINUX_getegid] = lc_getid,
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
};

uint32_t linux_compat_handler(struct Registers *r) {
    struct task_struct *cur = current;
    uint32_t nr = r->eax;
    int64_t ret = -LINUX_ENOSYS;

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

    lc_seterrno(cur, ret < 0 ? (int32_t)-ret : 0);
    return ret < 0 ? (uint32_t)-1 : (uint32_t)ret;
}
