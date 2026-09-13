#ifndef DRIVERS_CHAR_TTY_H
#define DRIVERS_CHAR_TTY_H

#include <stdint.h>

#define TTY_FLAG 0x5454u

#define TTY_IECHO 0x1u
#define TTY_ICANON 0x2u

struct TTY_OPS {
    int (*read)(char *buf, uint32_t n);
    int (*write)(const char *buf, uint32_t n);
    int (*ioctl)(uint32_t cmd, uint64_t arg);
    uint32_t (*avail)(void);
};

extern const struct TTY_OPS TTY;

void tty_init(void);
int tty_open(void);
uint32_t tty_pgid_of(uint32_t pid);
void tty_sigint_foreground(void);

#define TTY_IOCTL_TCGETS 0x5401u
#define TTY_IOCTL_TCSETS 0x5402u
#define TTY_IOCTL_TIOCGWINSZ 0x5413u
#define TTY_IOCTL_FIONREAD 0x541bu

#endif /* DRIVERS_CHAR_TTY_H */
