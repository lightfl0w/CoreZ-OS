#include "libc/user/syscall.h"

#include "kernel/syscall_nr.h"
#include "libc/user/signal.h"

static inline uint32_t syscall0(uint32_t nr) {
    uint32_t retval;
    __asm__ volatile("int $0x80" : "=a"(retval) : "a"(nr) : "memory");
    return retval;
}

static inline uint32_t syscall1(uint64_t nr, uint64_t arg1) {
    uint32_t retval;
    __asm__ volatile("int $0x80"
                     : "=a"(retval)
                     : "a"(nr), "b"(arg1)
                     : "memory");
    return retval;
}

static inline uint32_t syscall2(uint64_t nr, uint64_t arg1, uint64_t arg2) {
    uint32_t retval;
    __asm__ volatile("int $0x80"
                     : "=a"(retval)
                     : "a"(nr), "b"(arg1), "c"(arg2)
                     : "memory");
    return retval;
}

static inline uint32_t syscall3(uint64_t nr, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3) {
    uint32_t retval;
    __asm__ volatile("int $0x80"
                     : "=a"(retval)
                     : "a"(nr), "b"(arg1), "c"(arg2), "d"(arg3)
                     : "memory");
    return retval;
}

static inline uint32_t syscall4(uint64_t nr, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4) {
    uint32_t retval;
    __asm__ volatile("int $0x80"
                     : "=a"(retval)
                     : "a"(nr), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4)
                     : "memory");
    return retval;
}

uint32_t syscall5(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5) {
    uint32_t retval;
    __asm__ volatile("int $0x80"
                     : "=a"(retval)
                     : "a"(nr), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4),
                       "D"(arg5)
                     : "memory");
    return retval;
}

static inline uint64_t syscall6(uint64_t nr, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4, uint64_t arg5,
                                uint64_t arg6) {
    register uint64_t x6 asm("r10") = arg6;
    uint64_t retval;
    __asm__ volatile("int $0x80"
                     : "=a"(retval)
                     : "a"(nr), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4),
                       "D"(arg5), "r"(x6)
                     : "memory");
    return retval;
}

uint32_t getpid(void) {
    return syscall0(SYS_GETPID);
}
int32_t write(int32_t fd, const void *buf, uint32_t count) {
    return (int32_t)syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                             count);
}
int32_t read(int32_t fd, void *buf, uint32_t count) {
    return (int32_t)syscall3(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                             count);
}
void putchar(char c) {
    write(1, &c, 1);
}
void clear(void) {
    syscall0(SYS_CLEAR);
}
int32_t fork(void) {
    return (int32_t)syscall0(SYS_FORK);
}
int32_t open(const char *pathname, uint8_t flag) {
    return (int32_t)syscall2(SYS_OPEN, (uint64_t)(uintptr_t)pathname,
                             (uint64_t)flag);
}
int32_t close(int32_t fd) {
    return (int32_t)syscall1(SYS_CLOSE, (uint64_t)fd);
}
int32_t lseek(int32_t fd, int32_t offset, uint8_t whence) {
    return (int32_t)syscall3(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset,
                             (uint64_t)whence);
}
int32_t unlink(const char *pathname) {
    return (int32_t)syscall1(SYS_UNLINK, (uint64_t)(uintptr_t)pathname);
}
int32_t mkdir(const char *pathname) {
    return (int32_t)syscall1(SYS_MKDIR, (uint64_t)(uintptr_t)pathname);
}
int32_t rmdir(const char *pathname) {
    return (int32_t)syscall1(SYS_RMDIR, (uint64_t)(uintptr_t)pathname);
}
int32_t chdir(const char *path) {
    return (int32_t)syscall1(SYS_CHDIR, (uint64_t)(uintptr_t)path);
}
char *getcwd(char *buf, uint32_t size) {
    return (char *)syscall2(SYS_GETCWD, (uint64_t)(uintptr_t)buf,
                            (uint64_t)size);
}
int32_t stat(const char *path, struct stat *buf) {
    return (int32_t)syscall2(SYS_STAT, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)buf);
}
struct dir *opendir(const char *name) {
    return (struct dir *)syscall1(SYS_OPENDIR, (uint64_t)(uintptr_t)name);
}
int32_t closedir(struct dir *dir) {
    return (int32_t)syscall1(SYS_CLOSEDIR, (uint64_t)(uintptr_t)dir);
}
struct dir_entry *readdir(struct dir *dir) {
    return (struct dir_entry *)syscall1(SYS_READDIR, (uint64_t)(uintptr_t)dir);
}
void rewinddir(struct dir *dir) {
    syscall1(SYS_REWINDDIR, (uint64_t)(uintptr_t)dir);
}
void ps(void) {
    syscall0(SYS_PS);
}
int32_t execv(const char *path, const char *argv[]) {
    return (int32_t)syscall2(SYS_EXECV, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)argv);
}
void exit(int32_t status) {
    syscall1(SYS_EXIT, (uint64_t)status);
    for (;;) {
    }
}
int32_t wait(int32_t *status) {
    return (int32_t)syscall1(SYS_WAIT, (uint64_t)(uintptr_t)status);
}
int32_t pipe(int32_t pipefd[2]) {
    return (int32_t)syscall1(SYS_PIPE, (uint64_t)(uintptr_t)pipefd);
}
void fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd) {
    syscall2(SYS_FD_REDIRECT, (uint64_t)old_local_fd, (uint64_t)new_local_fd);
}
int32_t gui_start(void) {
    return (int32_t)syscall0(SYS_GUI);
}
void *brk(void *addr) {
    return (void *)syscall1(SYS_BRK, (uint64_t)(uintptr_t)addr);
}
void *sbrk(intptr_t inc) {
    uint32_t ob = (uint32_t)syscall1(SYS_BRK, 0);
    if (inc == 0) {
        return (void *)ob;
    }
    uint32_t nb = (uint32_t)syscall1(SYS_BRK, ob + (uint32_t)inc);
    if (nb == ob && inc > 0) {
        return (void *)-1;
    }
    return (void *)ob;
}

struct _mmap_args {
    uint32_t addr;
    uint32_t len;
    uint32_t prot;
    uint32_t flags;
    uint32_t fd;
    uint32_t offset;
};

void *mmap(void *addr, uint32_t len, int prot, int flags, int fd,
           uint32_t offset) {
    struct _mmap_args a;
    a.addr = (uint32_t)addr;
    a.len = len;
    a.prot = (uint32_t)prot;
    a.flags = (uint32_t)flags;
    a.fd = (uint32_t)fd;
    a.offset = offset;
    return (void *)(intptr_t)(int32_t)syscall1(SYS_MMAP, (uint32_t)&a);
}

void *mmap2(void *addr, uint32_t len, int prot, int flags, int fd,
            uint32_t offset) {
    return (void *)(intptr_t)(int32_t)syscall6(
        SYS_MMAP2, (uint32_t)addr, len, (uint32_t)prot, (uint32_t)flags,
        (uint32_t)fd, offset);
}

int32_t munmap(void *addr, uint32_t len) {
    return (int32_t)syscall2(SYS_MUNMAP, (uint32_t)addr, len);
}

int32_t mprotect(void *addr, uint32_t len, int prot) {
    return (int32_t)syscall3(SYS_MPROTECT, (uint32_t)addr, len, (uint32_t)prot);
}

int32_t futex(uint32_t uaddr, int op, uint32_t val, void *timeout) {
    return (int32_t)syscall4(SYS_FUTEX, uaddr, (uint32_t)op, val,
                             (uint32_t)timeout);
}

int32_t clone(uint32_t flags, void *child_stack) {
    return (int32_t)syscall2(SYS_CLONE, flags, (uint32_t)child_stack);
}

int32_t fstat(int32_t fd, struct stat *buf) {
    return (int32_t)syscall2(SYS_FSTAT, (uint32_t)fd, (uint32_t)buf);
}
int32_t dup(int32_t oldfd) {
    return (int32_t)syscall1(SYS_DUP, (uint32_t)oldfd);
}
int32_t dup2(int32_t oldfd, int32_t newfd) {
    return (int32_t)syscall2(SYS_DUP2, (uint32_t)oldfd, (uint32_t)newfd);
}
int32_t fcntl(int32_t fd, int32_t cmd, uint32_t arg) {
    return (int32_t)syscall3(SYS_FCNTL, (uint32_t)fd, (uint32_t)cmd, arg);
}
int32_t getdents(int32_t fd, struct linux_dirent *dirp, uint32_t count) {
    return (int32_t)syscall3(SYS_GETDENTS, (uint32_t)fd, (uint32_t)dirp, count);
}
int32_t readlink(const char *path, char *buf, uint32_t bufsiz) {
    return (int32_t)syscall3(SYS_READLINK, (uint32_t)path, (uint32_t)buf,
                             bufsiz);
}
int32_t access(const char *path, int32_t mode) {
    return (int32_t)syscall2(SYS_ACCESS, (uint32_t)path, (uint32_t)mode);
}
int32_t rename(const char *oldpath, const char *newpath) {
    return (int32_t)syscall2(SYS_RENAME, (uint32_t)oldpath, (uint32_t)newpath);
}
int32_t truncate(const char *path, int32_t length) {
    return (int32_t)syscall2(SYS_TRUNCATE, (uint32_t)path, (uint32_t)length);
}
int32_t chmod(const char *path, uint32_t mode) {
    return (int32_t)syscall2(SYS_CHMOD, (uint32_t)path, mode);
}
int32_t clock_gettime(int32_t clk_id, struct timespec *tp) {
    return (int32_t)syscall2(SYS_CLOCK_GETTIME, (uint32_t)clk_id, (uint32_t)tp);
}
int32_t gettimeofday(struct timeval *tv, void *tz) {
    return (int32_t)syscall2(SYS_GETTIMEOFDAY, (uint32_t)tv, (uint32_t)tz);
}
int32_t nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int32_t)syscall2(SYS_NANOSLEEP, (uint32_t)req, (uint32_t)rem);
}
uint32_t getuid(void) {
    return syscall0(SYS_GETUID);
}
uint32_t getgid(void) {
    return syscall0(SYS_GETGID);
}
uint32_t geteuid(void) {
    return syscall0(SYS_GETEUID);
}
uint32_t getegid(void) {
    return syscall0(SYS_GETEGID);
}
void exit_group(int32_t status) {
    syscall1(SYS_EXIT_GROUP, (uint32_t)status);
    for (;;) {
    }
}

int32_t icmp_send(uint32_t dst, uint16_t id, uint16_t seq) {
    return (int32_t)syscall3(SYS_ICMP_SEND, (uint32_t)dst, (uint32_t)id,
                             (uint32_t)seq);
}

int32_t icmp_recv(struct nt_ping_reply *buf, int32_t max) {
    return (int32_t)syscall2(SYS_ICMP_RECV, (uint32_t)buf, (uint32_t)max);
}

int32_t socket(int32_t domain, int32_t type, int32_t proto) {
    return (int32_t)syscall3(SYS_SOCKET, (uint32_t)domain, (uint32_t)type,
                             (uint32_t)proto);
}
int32_t sock_bind(int32_t fd, uint32_t ip, uint16_t port) {
    return (int32_t)syscall3(SYS_BIND, (uint32_t)fd, (uint32_t)ip, port);
}
int32_t sock_listen(int32_t fd, int32_t backlog) {
    return (int32_t)syscall2(SYS_LISTEN, (uint32_t)fd, (uint32_t)backlog);
}
int32_t sock_connect(int32_t fd, uint32_t ip, uint16_t port) {
    return (int32_t)syscall3(SYS_CONNECT, (uint32_t)fd, (uint32_t)ip, port);
}
int32_t sock_send(int32_t fd, const void *buf, uint32_t len) {
    return (int32_t)syscall3(SYS_SEND, (uint32_t)fd, (uint32_t)buf, len);
}
int32_t sock_recv(int32_t fd, void *buf, uint32_t len) {
    return (int32_t)syscall3(SYS_RECV, (uint32_t)fd, (uint32_t)buf, len);
}
int32_t sock_sendto(int32_t fd, const void *buf, uint32_t len, uint32_t daddr,
                    uint16_t dport) {
    return (int32_t)syscall5(SYS_SENDTO, (uint32_t)fd, (uint32_t)buf, len,
                             daddr, dport);
}
int32_t sock_recvfrom(int32_t fd, void *buf, uint32_t len, uint32_t *saddr,
                      uint16_t *sport) {
    return (int32_t)syscall5(SYS_RECVFROM, (uint32_t)fd, (uint32_t)buf, len,
                             (uint32_t)saddr, (uint32_t)sport);
}
int32_t sock_accept(int32_t fd) {
    return (int32_t)syscall1(SYS_ACCEPT, (uint32_t)fd);
}
int32_t sock_close(int32_t fd) {
    return (int32_t)syscall1(SYS_CLOSE_SOCKET, (uint32_t)fd);
}
int32_t sock_shutdown(int32_t fd, int32_t how) {
    return (int32_t)syscall2(SYS_SOCK_SHUTDOWN, (uint32_t)fd, (uint32_t)how);
}
int32_t sock_getsockname(int32_t fd, uint32_t *ip, uint16_t *port) {
    return (int32_t)syscall3(SYS_GETSOCKNAME, (uint32_t)fd, (uint32_t)ip,
                             (uint32_t)port);
}
int32_t sock_getpeername(int32_t fd, uint32_t *ip, uint16_t *port) {
    return (int32_t)syscall3(SYS_GETPEERNAME, (uint32_t)fd, (uint32_t)ip,
                             (uint32_t)port);
}
int32_t sock_getsockopt(int32_t fd, int32_t level, int32_t optname, void *val,
                        uint32_t *len) {
    return (int32_t)syscall5(SYS_GETSOCKOPT, (uint32_t)fd, (uint32_t)level,
                             (uint32_t)optname, (uint32_t)val, (uint32_t)len);
}
int32_t sock_setsockopt(int32_t fd, int32_t level, int32_t optname,
                        const void *val, uint32_t len) {
    return (int32_t)syscall5(SYS_SETSOCKOPT, (uint32_t)fd, (uint32_t)level,
                             (uint32_t)optname, (uint32_t)val, len);
}
int32_t sock_select(int32_t nfds, uint32_t *rfds, uint32_t *wfds,
                    uint32_t *efds, int32_t timeout_ms) {
    return (int32_t)syscall5(SYS_SELECT, (uint32_t)nfds, (uint32_t)rfds,
                             (uint32_t)wfds, (uint32_t)efds,
                             (uint32_t)timeout_ms);
}

__attribute__((naked)) void __restore(void) {
    __asm__ volatile("movl %0, %%eax\n"
                     "int $0x80\n"
                     :
                     : "i"(SYS_SIGRETURN)
                     : "memory");
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    struct sigaction kact;
    const struct sigaction *pact = act;
    if (act) {
        kact = *act;
        kact.sa_restorer = __restore;
        kact.sa_flags |= SA_RESTORER;
        pact = &kact;
    }
    return (int)syscall3(SYS_SIGACTION, (uint32_t)sig, (uint32_t)pact,
                         (uint32_t)old);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return (int)syscall3(SYS_SIGPROCMASK, (uint32_t)how, (uint32_t)set,
                         (uint32_t)oldset);
}

int kill(pid_t pid, int sig) {
    return (int)syscall2(SYS_KILL, (uint32_t)pid, (uint32_t)sig);
}

void (*signal(int sig, void (*handler)(int)))(int) {
    struct sigaction act, old;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = 0;
    act.sa_restorer = __restore;
    act.sa_flags |= SA_RESTORER;
    if (sigaction(sig, &act, &old) < 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}
