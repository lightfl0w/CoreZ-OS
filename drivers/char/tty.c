#include "drivers/char/tty.h"

#include "drivers/driver_ops.h"

#include "drivers/char/console/io.h"
#include "arch/cpu.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/file.h"
#include "kernel/signal.h"

#define TTY_WINSZ_ROW 25
#define TTY_WINSZ_COL 80

static struct {
    uint32_t iflag;
    uint8_t cc_vmin;
    uint8_t cc_vtime;
} tty_termios;


int tty_write(const char *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        console_putc(buf[i]);
    return (int)n;
}

static void tty_echo(char c) {
    if (!(tty_termios.iflag & TTY_IECHO))
        return;
    if (c == '\r')
        return;
    console_putc(c);
}

static char ioq_getchar_sync(struct ioqueue *q) {
    uint32_t f = cpu_eflags();
    cpu_cli();
    char c = ioq_getchar(q);
    cpu_set_eflags(f);
    return c;
}

static int tty_read_line(char *buf, uint32_t n) {
    uint32_t got = 0;
    for (;;) {
        char c = ioq_getchar_sync(&keyboard_ioq);
        if (c == '\r')
            c = '\n';
        if (c == '\n') {
            tty_echo(c);
            buf[got] = '\n';
            return (int)(got + 1 < n ? got + 1 : n);
        }
        if (c == '\b' || c == 0x7f) {
            if (got == 0) {
                tty_echo('\a');
                continue;
            }
            got--;
            tty_echo('\b');
            tty_echo(' ');
            tty_echo('\b');
            continue;
        }
        if (got + 1 >= n) {
            tty_echo('\a');
            continue;
        }
        buf[got++] = c;
        tty_echo(c);
    }
}

static int tty_read_raw(char *buf, uint32_t n) {
    uint32_t wait = tty_termios.cc_vmin;
    uint32_t got = 0;
    while (got < n) {
        buf[got++] = ioq_getchar_sync(&keyboard_ioq);
        if (got >= wait)
            break;
    }
    return (int)got;
}

int tty_read(char *buf, uint32_t n) {
    if (n == 0)
        return 0;
    if (tty_termios.iflag & TTY_ICANON)
        return tty_read_line(buf, n);
    return tty_read_raw(buf, n);
}

uint32_t tty_avail(void) {
    return ioq_length(&keyboard_ioq);
}

int tty_ioctl(uint32_t cmd, uint64_t arg) {
    (void)arg;
    switch (cmd) {
    case TTY_IOCTL_TCGETS:
        if (arg) {
            uint32_t *p = (uint32_t *)(uintptr_t)arg;
            p[0] = tty_termios.iflag;
        }
        return 0;
    case TTY_IOCTL_TCSETS:
        if (arg)
            tty_termios.iflag = *(uint32_t *)(uintptr_t)arg;
        return 0;
    case TTY_IOCTL_TIOCGWINSZ:
        if (arg) {
            uint16_t *w = (uint16_t *)(uintptr_t)arg;
            w[0] = TTY_WINSZ_ROW;
            w[1] = TTY_WINSZ_COL;
            w[2] = 0;
            w[3] = 0;
        }
        return 0;
    case TTY_IOCTL_FIONREAD:
        if (arg) {
            uint32_t *p = (uint32_t *)(uintptr_t)arg;
            *p = ioq_length(&keyboard_ioq);
        }
        return 0;
    default:
        return -1;
    }
}

int tty_open(void) {
    return fd_install(0);
}

uint32_t tty_pgid_of(uint32_t pid) {
    (void)pid;
    return 0;
}

void tty_sigint_foreground(void) {
    if (foreground_pid == (uint32_t)-1)
        return;
    uint32_t pgid = tty_pgid_of(foreground_pid);
    if (pgid == 0)
        pgid = foreground_pid;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct task_struct *t = &task_table[i];
        if (!t->slot_used || t->status == TASK_DIED)
            continue;
        if (t->pid != pgid)
            continue;
        sys_kill((int)t->pid, 2);
    }
}

const struct tty_ops TTY = {
    .read = tty_read,
    .write = tty_write,
    .ioctl = tty_ioctl,
    .avail = tty_avail,
};

static int tty_drv(void) {
    tty_init();
    return 0;
}

DRIVER_REGISTER("tty", 12, tty_drv);


void tty_init(void) {
    tty_termios.iflag = TTY_ICANON | TTY_IECHO;
    tty_termios.cc_vmin = 1;
    tty_termios.cc_vtime = 0;
}
