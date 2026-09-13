#ifndef FORK_H
#define FORK_H

#include "kernel/asm/stub.h"
#include "kernel/sched/thread.h"
#include <stdint.h>

pid_t sys_fork(struct X86_REGS *r);

#endif
