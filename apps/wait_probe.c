#include "libc/user/stdio.h"
#include "syscall.h"
#include <stdint.h>

#define L_SYS_write 1
#define L_SYS_nanosleep 35
#define L_SYS_clock_gettime 228
#define L_SYS_wait4 247

#define L_WNOHANG 1

struct ltimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static long lsys(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static long mono_ms(void) {
    struct ltimespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    lsys(L_SYS_clock_gettime, 1, (long)&ts, 0, 0);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int32_t child_pid;
static int32_t st_bss;

int main(void) {
    int32_t pid = fork();
    if (pid == 0) {
        struct ltimespec ts;
        ts.tv_sec = 2;
        ts.tv_nsec = 0;
        lsys(L_SYS_nanosleep, (long)&ts, 0, 0, 0);
        exit(7);
    }
    if (pid < 0) {
        printf("wait_probe: fork failed\n");
        return 1;
    }
    child_pid = pid;

    st_bss = -1;
    long t0 = mono_ms();
    long rb = lsys(L_SYS_wait4, -1, (long)&st_bss, L_WNOHANG, 0);
    long dtb = mono_ms() - t0;

    int32_t st = -1;
    long r = lsys(L_SYS_wait4, -1, (long)&st, L_WNOHANG, 0);
    long dt = mono_ms() - t0;
    printf("wait_probe: bss=%x r=%d dt=%d | sp=%x wnohang r=%d dt=%d pid=%d\n",
           (unsigned)(long)&st_bss, (int)rb, (int)dtb, (unsigned)(long)&st,
           (int)r, (int)dt, (int)child_pid);

    st = -1;
    long got = lsys(L_SYS_wait4, -1, (long)&st, 0, 0);
    long dt2 = mono_ms() - t0;
    printf("wait_probe: blocking got=%d st=%d dt=%d\n", (int)got, (int)st,
           (int)dt2);

    if (r == 0 && dt < 500 && got == (long)child_pid) {
        printf("WAIT_PROBE_PASS\n");
        return 0;
    }
    printf("WAIT_PROBE_FAIL\n");
    return 1;
}
