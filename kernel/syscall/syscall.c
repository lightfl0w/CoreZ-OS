#include "kernel/syscall/syscall.h"
#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/tty.h"
#include "drivers/net/net.h"
#include "drivers/net/socket.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/gui/gui.h"
#include "kernel/init/acpi/acpi.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/shell/pipe.h"
#include "kernel/syscall/file_syscall.h"
#include "kernel/syscall/futex.h"
#include "kernel/syscall/linux_compat.h"
#include "kernel/syscall/mmap.h"
#include "kernel/userprog/clone.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/str/str.h"
#include "libc/user/syscall.h"
static uint32_t sys_getpid(void) {
    return current->pid;
}
int32_t sys_clock_gettime(int32_t clk_id, struct timespec *tp) {
    if (tp == NULL) {
        return -1;
    }
    (void)clk_id;
    memset(tp, 0, sizeof(*tp));
    tp->tv_sec = (int32_t)(tick / PIT_HZ);
    tp->tv_nsec = (int32_t)((tick % PIT_HZ) * (1000u * 1000u * 1000u / PIT_HZ));
    return 0;
}
int32_t sys_gettimeofday(struct timeval *tv, void *tz) {
    if (tv == NULL) {
        return -1;
    }
    (void)tz;
    memset(tv, 0, sizeof(*tv));
    tv->tv_sec = (int32_t)(tick / PIT_HZ);
    tv->tv_usec = (int32_t)((tick % PIT_HZ) * (1000u * 1000u / PIT_HZ));
    return 0;
}
int32_t sys_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0) {
        return -1;
    }
    uint32_t sec = (uint32_t)req->tv_sec;
    uint32_t ms;
    if (sec > 0x1fffff) {
        ms = 0x7fffffffU;
    } else {
        ms = sec * 1000u;
    }
    ms += (uint32_t)req->tv_nsec / 1000000u;
    if (ms > 0x7fffffffU) {
        ms = 0x7fffffffU;
    }
    mtime_sleep(ms);
    if (rem != NULL) {
        memset(rem, 0, sizeof(*rem));
    }
    return 0;
}
static uint32_t sys_getid(void) {
    return 0;
}
static void sys_exit_group(int32_t status) {
    sys_exit(status);
    for (;;) {
    }
}
static uint32_t sys_shutdown(void) {
    kprintf("[shutdown] shutting down system...\n");
    acpi_shutdown();
    return 0;
}
static uint32_t sys_write(int32_t fd, char *str, uint32_t count) {
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC) {
        return (uint32_t)-1;
    }
    if (is_pipe(fd)) {
        return pipe_write(fd, str, count);
    }
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct file *wf = file_get(gfd);
    if (gfd >= 3 && wf != NULL && wf->fd_inode != NULL &&
        wf->fd_flag != PIPE_FLAG) {
        return write_file(fd, str, count);
    }
    TTY.write(str, count);
    return count;
}
static uint32_t sys_putchar(char c) {
    console_putc(c);
    return (uint32_t)(unsigned char)c;
}
static uint32_t sys_clear(void) {
    io_clear_screen();
    return 0;
}
static int32_t sys_read(int32_t fd, void *buf, uint32_t count) {
    if (fd == 1 || fd == 2)
        return -1;
    if (is_pipe(fd)) {
        return (int32_t)pipe_read(fd, buf, count);
    }
    if (fd == 0) {
        return TTY.read((char *)buf, count);
    }
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -1;
    if (count == 0)
        return 0;
    int32_t r = (int32_t)read_file(fd, buf, count);
    return r;
}
static const char *task_status_str(enum task_status s) {
    static const char *names[] = {"RUNNING", "READY",  "BLOCKED",
                                  "WAITING", "HANGING", "DIED"};
    return (s >= TASK_RUNNING && s <= TASK_DIED)
               ? names[__builtin_ctz(s)]
               : "?";
}
static int ps_action(struct task_struct *t, void *arg) {
    (void)arg;
    char buf[80];
    const char *parent = (t->parent_pid == -1) ? "(none)" : "?";
    if (t->parent_pid >= 0) {
        u32_to_dec((uint32_t)t->parent_pid, buf);
        parent = buf;
    }
    kprintf("PID=%u PPID=%s STAT=%s TICKS=%u NAME=%s\n", t->pid, parent,
            task_status_str(t->status), t->elapsed_ticks, t->name);
    return 0;
}
static uint32_t sys_ps(void) {
    kprintf("=== ps ===\n");
    thread_traverse_all(ps_action, NULL);
    return 0;
}
uint32_t sys_brk(uint32_t addr) {
    struct task_struct *cur = current;
    uint32_t base = (cur->brk_base != 0) ? cur->brk_base : USER_HEAP_BASE;
    if (cur->user_brk == 0) {
        cur->user_brk = base;
    }

    uint32_t limit = (base < USER_LOW_CEILING) ? USER_LOW_CEILING
                                               : USER_HEAP_LIMIT;
    uint32_t cur_brk = cur->user_brk;
    if (addr == 0) {
        return cur_brk;
    }
    uint32_t new_brk = addr;
    if (new_brk < base)
        new_brk = base;
    if (new_brk > limit)
        new_brk = limit;
    uint32_t old_page = (cur_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_page > old_page) {
        for (uint32_t page = old_page; page < new_page; page += PAGE_SIZE) {
            if (page_is_mapped(page)) {
                kprintf("[brk] collision at 0x%x, keep 0x%x\n", page, cur_brk);
                return cur_brk;
            }
            if (get_a_page(page) == 0) {
                kprintf("[brk] OOM, keep 0x%x\n", cur_brk);
                return cur_brk;
            }
        }
    } else if (new_page < old_page) {
        for (uint32_t page = new_page; page < old_page; page += PAGE_SIZE) {
            free_user_page(page);
        }
    }
    cur->user_brk = new_brk;
    return new_brk;
}
static uint32_t sys_set_thread_area(struct Registers *r, uint32_t base) {
    if (base == 0 || !user_range_writable(base, sizeof(int32_t)))
        return (uint32_t)-1;
    current->tls_base = base;
    current->tls_selector = SELECTOR_TLS;
    current->tls_msr = 0;
    tls_desc_set_base(base);
    current->errno = 0;
    *(volatile int32_t *)base = 0;
    return 0;
}

static int kern_call(struct Registers *r) {
    return (r->cs & 3) == 0;
}

static int ok_read(struct Registers *r, uint32_t p, uint32_t n) {
    return kern_call(r) || access_ok((const void *)p, (size_t)n, 0);
}

static int ok_write(struct Registers *r, uint32_t p, uint32_t n) {
    return kern_call(r) || access_ok((const void *)p, (size_t)n, 1);
}

static const char *path_arg(struct Registers *r, char *kbuf, uint32_t cap) {
    if (kern_call(r)) {
        return (const char *)r->ebx;
    }
    if (copy_str_from_user(kbuf, (const char *)r->ebx, cap) != 0) {
        return NULL;
    }
    return kbuf;
}

static int64_t nsys_getpid(struct Registers *r) {
    (void)r;
    return sys_getpid();
}

static int64_t nsys_write(struct Registers *r) {
    if (!ok_read(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return sys_write((int32_t)r->ebx, (char *)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_putchar(struct Registers *r) {
    return sys_putchar((char)r->ebx);
}

static int64_t nsys_clear(struct Registers *r) {
    (void)r;
    return sys_clear();
}

static int64_t nsys_read(struct Registers *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_read((int32_t)r->ebx, (void *)r->ecx,
                              (uint32_t)r->edx);
}

static int64_t nsys_fork(struct Registers *r) {
    return (uint32_t)sys_fork(r);
}

static int64_t nsys_getcwd(struct Registers *r) {
    if (!ok_write(r, r->ebx, r->ecx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_getcwd((char *)r->ebx, (uint32_t)r->ecx);
}

static int64_t nsys_chdir(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_chdir(p);
}

static int64_t nsys_mkdir(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_mkdir(p);
}

static int64_t nsys_rmdir(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_rmdir(p);
}

static int64_t nsys_open(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)open_file(p, (uint8_t)r->ecx);
}

static int64_t nsys_close(struct Registers *r) {
    return (uint32_t)close_file((int)r->ebx);
}

static int64_t nsys_lseek(struct Registers *r) {
    return (uint32_t)sys_lseek((int32_t)r->ebx, (int32_t)r->ecx,
                               (uint8_t)r->edx);
}

static int64_t nsys_unlink(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_unlink(p);
}

static int64_t nsys_opendir(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_opendir(p);
}

static int64_t nsys_closedir(struct Registers *r) {
    return (uint32_t)sys_closedir((struct dir *)r->ebx);
}

static int64_t nsys_readdir(struct Registers *r) {
    return (uint32_t)sys_readdir((struct dir *)r->ebx);
}

static int64_t nsys_rewinddir(struct Registers *r) {
    sys_rewinddir((struct dir *)r->ebx);
    return 0;
}

static int64_t nsys_stat(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL || !ok_write(r, r->ecx, sizeof(struct stat))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_stat(p, (struct stat *)r->ecx);
}

static int64_t nsys_ps(struct Registers *r) {
    (void)r;
    sys_ps();
    return 0;
}

static int64_t nsys_execv(struct Registers *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_execv(p, (const char **)r->ecx, r);
}

static int64_t nsys_exit(struct Registers *r) {
    sys_exit((int32_t)r->ebx);
    return 0;
}

static int64_t nsys_wait(struct Registers *r) {
    if (!ok_write(r, r->ebx, sizeof(int32_t))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_wait((int32_t *)r->ebx);
}

static int64_t nsys_pipe(struct Registers *r) {
    if (!ok_write(r, r->ebx, 2 * sizeof(int32_t))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_pipe((int32_t *)r->ebx);
}

static int64_t nsys_fd_redirect(struct Registers *r) {
    sys_fd_redirect((uint32_t)r->ebx, (uint32_t)r->ecx);
    return 0;
}

static int64_t nsys_gui(struct Registers *r) {
    (void)r;
    return (uint32_t)gui_session_run();
}

static int64_t nsys_brk(struct Registers *r) {
    return (uint32_t)sys_brk((uint32_t)r->ebx);
}

static int64_t nsys_sigaction(struct Registers *r) {
    if ((r->ecx && !ok_read(r, r->ecx, sizeof(struct sigaction))) ||
        (r->edx && !ok_write(r, r->edx, sizeof(struct sigaction)))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_sigaction((int)r->ebx,
                                   (const struct sigaction *)r->ecx,
                                   (struct sigaction *)r->edx);
}

static int64_t nsys_kill(struct Registers *r) {
    return (uint32_t)sys_kill((int)r->ebx, (int)r->ecx);
}

static int64_t nsys_sigreturn(struct Registers *r) {
    return sys_sigreturn(r);
}

static int64_t nsys_sigprocmask(struct Registers *r) {
    if ((r->ecx && !ok_read(r, r->ecx, sizeof(sigset_t))) ||
        (r->edx && !ok_write(r, r->edx, sizeof(sigset_t)))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_sigprocmask((int)r->ebx, (const sigset_t *)r->ecx,
                                     (sigset_t *)r->edx);
}

static int64_t nsys_set_thread_area(struct Registers *r) {
    if (!ok_write(r, r->ebx, sizeof(int32_t))) {
        return (uint32_t)-1;
    }
    return sys_set_thread_area(r, (uint32_t)r->ebx);
}

static int64_t nsys_mmap(struct Registers *r) {
    if (!ok_read(r, r->ebx, sizeof(struct mmap_args))) {
        return (uint32_t)-1;
    }
    uint32_t ret = sys_mmap((const struct mmap_args *)r->ebx);
    return ret > (uint32_t)-4096 ? (uint32_t)-1 : ret;
}

static int64_t nsys_munmap(struct Registers *r) {
    return (uint32_t)sys_munmap((uint32_t)r->ebx, (uint32_t)r->ecx);
}

static int64_t nsys_mmap2(struct Registers *r) {
    uint32_t ret = sys_mmap2((uint32_t)r->ebx, (uint32_t)r->ecx,
                             (uint32_t)r->edx, (uint32_t)r->esi,
                             (uint32_t)r->edi, (uint32_t)r->r10);
    return ret > (uint32_t)-4096 ? (uint32_t)-1 : ret;
}

static int64_t nsys_mprotect(struct Registers *r) {
    return (uint32_t)sys_mprotect((uint32_t)r->ebx, (uint32_t)r->ecx,
                                  (uint32_t)r->edx);
}

static int64_t nsys_futex(struct Registers *r) {
    if (!ok_read(r, r->ebx, 4)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_futex((uint32_t)r->ebx, (uint32_t)r->ecx,
                               (uint32_t)r->edx, (uint32_t)r->esi);
}

static int64_t nsys_clone(struct Registers *r) {
    return (uint32_t)sys_clone(r);
}

static int64_t nsys_fstat(struct Registers *r) {
    if (!ok_write(r, r->ecx, sizeof(struct stat))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_fstat((int32_t)r->ebx, (void *)r->ecx);
}

static int64_t nsys_dup(struct Registers *r) {
    return (uint32_t)sys_dup((int32_t)r->ebx);
}

static int64_t nsys_dup2(struct Registers *r) {
    return (uint32_t)sys_dup2((int32_t)r->ebx, (int32_t)r->ecx);
}

static int64_t nsys_fcntl(struct Registers *r) {
    return (uint32_t)sys_fcntl((int32_t)r->ebx, (int32_t)r->ecx,
                               (uint32_t)r->edx);
}

static int64_t nsys_getdents(struct Registers *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_getdents((int32_t)r->ebx, (void *)r->ecx,
                                  (uint32_t)r->edx);
}

static int64_t nsys_readlink(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1) || !ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_readlink((const char *)r->ebx, (char *)r->ecx,
                                  (uint32_t)r->edx);
}

static int64_t nsys_access(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_access((const char *)r->ebx, (int32_t)r->ecx);
}

static int64_t nsys_rename(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1) || !ok_read(r, r->ecx, 1)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_rename((const char *)r->ebx, (const char *)r->ecx);
}

static int64_t nsys_truncate(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_truncate((const char *)r->ebx, (int32_t)r->ecx);
}

static int64_t nsys_chmod(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_chmod((const char *)r->ebx, (uint32_t)r->ecx);
}
static int64_t nsys_symlink(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1) || !ok_read(r, r->ecx, 1)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_symlink((const char *)r->ebx, (const char *)r->ecx);
}
static int64_t nsys_mknod(struct Registers *r) {
    if (!ok_read(r, r->ebx, 1)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_mknod((const char *)r->ebx, (uint32_t)r->ecx,
                               (uint32_t)r->edx);
}

static int64_t nsys_clock_gettime(struct Registers *r) {
    if (!ok_write(r, r->ecx, sizeof(struct timespec))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_clock_gettime((int32_t)r->ebx,
                                       (struct timespec *)r->ecx);
}

static int64_t nsys_gettimeofday(struct Registers *r) {
    if (!ok_write(r, r->ebx, sizeof(struct timeval))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_gettimeofday((struct timeval *)r->ebx,
                                      (void *)r->ecx);
}

static int64_t nsys_nanosleep(struct Registers *r) {
    if (!ok_read(r, r->ebx, sizeof(struct timespec))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_nanosleep((const struct timespec *)r->ebx,
                                   (struct timespec *)r->ecx);
}

static int64_t nsys_getid(struct Registers *r) {
    (void)r;
    return sys_getid();
}

static int64_t nsys_exit_group(struct Registers *r) {
    sys_exit_group((int32_t)r->ebx);
    return 0;
}

static int64_t nsys_icmp_send(struct Registers *r) {
    return (uint32_t)nt_icmp_send((uint32_t)r->ebx, (uint16_t)r->ecx,
                                  (uint16_t)r->edx);
}

static int64_t nsys_icmp_recv(struct Registers *r) {
    if (!ok_write(r, r->ebx, sizeof(struct nt_ping_reply))) {
        return (uint32_t)-1;
    }
    return (uint32_t)nt_icmp_recv((struct nt_ping_reply *)r->ebx,
                                  (int)r->ecx);
}

static int64_t nsys_shutdown(struct Registers *r) {
    (void)r;
    return sys_shutdown();
}

static int64_t nsys_socket(struct Registers *r) {
    return (uint32_t)net_socket((int)r->ebx, (int)r->ecx, (int)r->edx);
}

static int64_t nsys_bind(struct Registers *r) {
    return (uint32_t)net_bind((int)r->ebx, (uint32_t)r->ecx,
                              (uint16_t)r->edx);
}

static int64_t nsys_listen(struct Registers *r) {
    return (uint32_t)net_listen((int)r->ebx, (int)r->ecx);
}

static int64_t nsys_connect(struct Registers *r) {
    return (uint32_t)net_connect((int)r->ebx, (uint32_t)r->ecx,
                                 (uint16_t)r->edx);
}

static int64_t nsys_send(struct Registers *r) {
    if (!ok_read(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_send((int)r->ebx, (const void *)r->ecx,
                              (uint32_t)r->edx);
}

static int64_t nsys_recv(struct Registers *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_recv((int)r->ebx, (void *)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_sendto(struct Registers *r) {
    if (!ok_read(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_sendto((int)r->ebx, (const void *)r->ecx,
                                (uint32_t)r->edx, (uint32_t)r->esi,
                                (uint16_t)r->edi);
}

static int64_t nsys_recvfrom(struct Registers *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_recvfrom((int)r->ebx, (void *)r->ecx,
                                  (uint32_t)r->edx, (uint32_t *)r->esi,
                                  (uint16_t *)r->edi);
}

static int64_t nsys_accept(struct Registers *r) {
    return (uint32_t)net_accept((int)r->ebx);
}

static int64_t nsys_close_socket(struct Registers *r) {
    return (uint32_t)net_close((int)r->ebx);
}

static int64_t nsys_sock_shutdown(struct Registers *r) {
    return (uint32_t)net_shutdown((int)r->ebx, (int)r->ecx);
}

static int64_t nsys_getsockname(struct Registers *r) {
    if (!ok_write(r, r->ecx, 4) || !ok_write(r, r->edx, 2)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_getsockname((int)r->ebx, (uint32_t *)r->ecx,
                                     (uint16_t *)r->edx);
}

static int64_t nsys_getpeername(struct Registers *r) {
    if (!ok_write(r, r->ecx, 4) || !ok_write(r, r->edx, 2)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_getpeername((int)r->ebx, (uint32_t *)r->ecx,
                                     (uint16_t *)r->edx);
}

static int64_t nsys_getsockopt(struct Registers *r) {
    if (!ok_write(r, r->esi, 4) || !ok_write(r, r->edi, 4)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_getsockopt((int)r->ebx, (int)r->ecx, (int)r->edx,
                                    (void *)r->esi, (uint32_t *)r->edi);
}

static int64_t nsys_setsockopt(struct Registers *r) {
    if (!ok_write(r, r->esi, 4)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_setsockopt((int)r->ebx, (int)r->ecx, (int)r->edx,
                                    (const void *)r->esi, (uint32_t)r->edi);
}

static int64_t nsys_sock_fcntl(struct Registers *r) {
    return (uint32_t)net_fcntl((int)r->ebx, (int)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_select(struct Registers *r) {
    if ((r->ecx && !ok_write(r, r->ecx, 8)) ||
        (r->edx && !ok_write(r, r->edx, 8)) ||
        (r->esi && !ok_write(r, r->esi, 8))) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_select((int)r->ebx, (uint32_t *)r->ecx,
                                (uint32_t *)r->edx, (uint32_t *)r->esi,
                                (int)r->edi);
}

typedef int64_t (*nsys_fn)(struct Registers *r);

static const nsys_fn nsys_table[] = {
    [SYS_GETPID] = nsys_getpid,       [SYS_WRITE] = nsys_write,
    [SYS_READ] = nsys_read,           [SYS_PUTCHAR] = nsys_putchar,
    [SYS_CLEAR] = nsys_clear,         [SYS_FORK] = nsys_fork,
    [SYS_GETCWD] = nsys_getcwd,       [SYS_CHDIR] = nsys_chdir,
    [SYS_MKDIR] = nsys_mkdir,         [SYS_RMDIR] = nsys_rmdir,
    [SYS_OPEN] = nsys_open,           [SYS_CLOSE] = nsys_close,
    [SYS_LSEEK] = nsys_lseek,         [SYS_UNLINK] = nsys_unlink,
    [SYS_OPENDIR] = nsys_opendir,     [SYS_CLOSEDIR] = nsys_closedir,
    [SYS_READDIR] = nsys_readdir,     [SYS_REWINDDIR] = nsys_rewinddir,
    [SYS_STAT] = nsys_stat,           [SYS_PS] = nsys_ps,
    [SYS_EXECV] = nsys_execv,         [SYS_EXIT] = nsys_exit,
    [SYS_WAIT] = nsys_wait,           [SYS_PIPE] = nsys_pipe,
    [SYS_FD_REDIRECT] = nsys_fd_redirect, [SYS_BRK] = nsys_brk,
    [SYS_GUI] = nsys_gui,             [SYS_SIGACTION] = nsys_sigaction,
    [SYS_KILL] = nsys_kill,           [SYS_SIGRETURN] = nsys_sigreturn,
    [SYS_SIGPROCMASK] = nsys_sigprocmask, [SYS_SET_THREAD_AREA] =
        nsys_set_thread_area,
    [SYS_MMAP] = nsys_mmap,           [SYS_MUNMAP] = nsys_munmap,
    [SYS_MPROTECT] = nsys_mprotect,   [SYS_FUTEX] = nsys_futex,
    [SYS_CLONE] = nsys_clone,         [SYS_FSTAT] = nsys_fstat,
    [SYS_DUP] = nsys_dup,             [SYS_DUP2] = nsys_dup2,
    [SYS_FCNTL] = nsys_fcntl,         [SYS_GETDENTS] = nsys_getdents,
    [SYS_READLINK] = nsys_readlink,   [SYS_ACCESS] = nsys_access,
    [SYS_RENAME] = nsys_rename,       [SYS_TRUNCATE] = nsys_truncate,
    [SYS_CHMOD] = nsys_chmod,         [SYS_CLOCK_GETTIME] = nsys_clock_gettime,
    [SYS_GETTIMEOFDAY] = nsys_gettimeofday, [SYS_NANOSLEEP] = nsys_nanosleep,
    [SYS_GETUID] = nsys_getid,        [SYS_GETGID] = nsys_getid,
    [SYS_GETEUID] = nsys_getid,       [SYS_GETEGID] = nsys_getid,
    [SYS_EXIT_GROUP] = nsys_exit_group, [SYS_MMAP2] = nsys_mmap2,
    [SYS_ICMP_SEND] = nsys_icmp_send, [SYS_ICMP_RECV] = nsys_icmp_recv,
    [SYS_SHUTDOWN] = nsys_shutdown,   [SYS_SOCKET] = nsys_socket,
    [SYS_BIND] = nsys_bind,           [SYS_LISTEN] = nsys_listen,
    [SYS_CONNECT] = nsys_connect,     [SYS_SEND] = nsys_send,
    [SYS_RECV] = nsys_recv,           [SYS_SENDTO] = nsys_sendto,
    [SYS_RECVFROM] = nsys_recvfrom,   [SYS_ACCEPT] = nsys_accept,
    [SYS_CLOSE_SOCKET] = nsys_close_socket, [SYS_SOCK_SHUTDOWN] =
        nsys_sock_shutdown,
    [SYS_GETSOCKNAME] = nsys_getsockname, [SYS_GETPEERNAME] =
        nsys_getpeername,
    [SYS_GETSOCKOPT] = nsys_getsockopt, [SYS_SETSOCKOPT] = nsys_setsockopt,
    [SYS_SOCK_FCNTL] = nsys_sock_fcntl, [SYS_SELECT] = nsys_select,
    [SYS_MKNOD] = nsys_mknod,         [SYS_SYMLINK] = nsys_symlink,
};

uint64_t syscall_handler(struct Registers *r) {
    uint32_t nr = r->eax;
    uint64_t ret = (uint32_t)-1;
    if (r->int_no == 0x81 || current->compat || nr >= COMPAT_SYSCALL_BASE) {
        check_pending_signals(r);
        if (r->int_no == 0x80 && nr < COMPAT_SYSCALL_BASE) {
            r->rdi = r->rbx;
            r->rsi = r->rcx;
            r->r10 = r->esi;
            r->r8 = r->edi;
            r->r9 = r->ebp;
        }
        return linux_compat_handler(r);
    }
    if (nr < sizeof(nsys_table) / sizeof(nsys_table[0]) && nsys_table[nr]) {
        ret = (uint64_t)nsys_table[nr](r);
    }
    r->rax = ret;
    check_pending_signals(r);
    return ret;
}

void syscall_init(void) {
    extern void syscall_entry(void);
    extern uint64_t syscall_kstack_top_data;
    const uint64_t MSR_STAR = 0xC0000081;
    const uint64_t MSR_LSTAR = 0xC0000082;
    const uint64_t MSR_FMASK = 0xC0000084;
    const uint64_t MSR_EFER = 0xC0000080;

    uint64_t star = ((uint64_t)0x33 << 48) | ((uint64_t)0x08 << 32);
    asm_wrmsr(MSR_STAR, star);
    asm_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    asm_wrmsr(MSR_FMASK, 0x5700);
    uint64_t efer = asm_rdmsr(MSR_EFER);
    asm_wrmsr(MSR_EFER, efer | 1);
    syscall_kstack_top_data = 0;

    kprintf("[OK] syscall init, 0x80 full table + syscall/sysret entry\n");
}
