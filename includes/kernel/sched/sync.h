#ifndef SYNC_H
#define SYNC_H

#include "lib/list/list.h"
#include <stdint.h>

struct SCHED_SPINLOCK {
    volatile uint32_t locked;
};

struct SCHED_SEMAPHORE {
    uint8_t value;
    struct LIST waiters;

    struct SCHED_SPINLOCK lock;
};

struct SCHED_LOCK {
    struct TASK *holder;
    struct SCHED_SEMAPHORE semaphore;
    uint32_t holder_repeat_nr;
};

void spinlock_init(struct SCHED_SPINLOCK *s);
void spinlock_acquire(struct SCHED_SPINLOCK *s);
void spinlock_release(struct SCHED_SPINLOCK *s);

void sema_init(struct SCHED_SEMAPHORE *psema, uint8_t value);
void sema_down(struct SCHED_SEMAPHORE *psema);
void sema_up(struct SCHED_SEMAPHORE *psema);
void lock_init(struct SCHED_LOCK *plock);
void lock_acquire(struct SCHED_LOCK *plock);
void lock_release(struct SCHED_LOCK *plock);

#endif
