#ifndef NT_FUTEX_H
#define NT_FUTEX_H

#include <stdint.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

#define EAGAIN 11

int32_t sys_futex(uint32_t uaddr, uint32_t op, uint32_t val, uint32_t timeout);
void futex_init(void);

#endif
