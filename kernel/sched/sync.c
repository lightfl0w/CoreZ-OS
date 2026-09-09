#include "kernel/sched/sync.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "kernel/sched/thread.h"
void spinlock_init(struct spinlock *s) {
    s->locked = 0;
}
void spinlock_acquire(struct spinlock *s) {
    while (asm_xchg(&s->locked, 1) != 0) {
        while (s->locked != 0) {
            asm_pause();
        }
    }
    __asm__ volatile("" : : : "memory");
}
void spinlock_release(struct spinlock *s) {
    __asm__ volatile("" : : : "memory");
    s->locked = 0;
}
void sema_init(struct semaphore *psema, uint8_t value) {
    psema->value = value;
    list_init(&psema->waiters);
    spinlock_init(&psema->lock);
}
void sema_down(struct semaphore *psema) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&psema->lock);
    while (psema->value == 0) {
        list_append(&psema->waiters, &current->wait_tag);
        spinlock_release(&psema->lock);
        thread_block();
        spinlock_acquire(&psema->lock);
    }
    psema->value--;
    spinlock_release(&psema->lock);
    asm_restore_eflags(old);
}
void sema_up(struct semaphore *psema) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&psema->lock);
    if (!list_empty(&psema->waiters)) {
        struct list_elem *e = list_pop_front(&psema->waiters);
        struct task_struct *w = list_entry(e, struct task_struct, wait_tag);
        thread_unblock(w);
    }
    psema->value++;
    spinlock_release(&psema->lock);
    asm_restore_eflags(old);
}
void lock_init(struct lock *plock) {
    plock->holder = 0;
    plock->holder_repeat_nr = 0;
    sema_init(&plock->semaphore, 1);
}

void lock_acquire(struct lock *plock) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    if (plock->holder == current) {
        plock->holder_repeat_nr++;
        asm_restore_eflags(old);
        return;
    }
    asm_restore_eflags(old);

    sema_down(&plock->semaphore);

    spinlock_acquire(&plock->semaphore.lock);
    if (plock->holder != 0) {
        kprintf("[lock] corrupt lock=%p holder=%p cur=%p\n", (void *)plock,
                (void *)plock->holder, (void *)current);
    }
    ASSERT(plock->holder == 0);
    ASSERT(plock->holder_repeat_nr == 0);
    plock->holder = current;
    plock->holder_repeat_nr = 1;
    spinlock_release(&plock->semaphore.lock);
}

void lock_release(struct lock *plock) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    ASSERT(plock->holder == current);

    if (plock->holder_repeat_nr > 1) {
        plock->holder_repeat_nr--;
        asm_restore_eflags(old);
        return;
    }

    spinlock_acquire(&plock->semaphore.lock);
    plock->holder = 0;
    plock->holder_repeat_nr = 0;
    spinlock_release(&plock->semaphore.lock);
    asm_restore_eflags(old);

    sema_up(&plock->semaphore);
}
