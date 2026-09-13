#include "kernel/syscall/futex.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "lib/list/list.h"
#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#define FUTEX_BUCKETS 64
struct SYS_FUTEX_BUCKET {
    struct LIST waiters;
    struct SCHED_SPINLOCK lock;
};
static struct SYS_FUTEX_BUCKET futex_buckets[FUTEX_BUCKETS];
static int futex_inited = 0;
void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        list_init(&futex_buckets[i].waiters);
        spinlock_init(&futex_buckets[i].lock);
    }
    futex_inited = 1;
}

static struct SYS_FUTEX_BUCKET *futex_bucket_for(uint32_t uaddr, uint32_t pml4_phys) {
    if (!futex_inited) {
        futex_init();
    }
    uint32_t h = (pml4_phys ^ (uaddr >> 2)) % FUTEX_BUCKETS;
    return &futex_buckets[h];
}

static int32_t sys_futex_wait(uint32_t uaddr, uint32_t val, uint32_t timeout) {
    struct SYS_FUTEX_BUCKET *b = futex_bucket_for(uaddr, current->pml4_phys);
    current->futex_ready = 0;
    (void)timeout;
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&b->lock);
    if (*(volatile uint32_t *)uaddr != val) {
        spinlock_release(&b->lock);
        asm_restore_eflags(old);
        return -EAGAIN;
    }
    list_append(&b->waiters, &current->futex_tag);
    spinlock_release(&b->lock);
    current->status = TASK_BLOCKED;
    schedule();
    asm_restore_eflags(old);
    if (!current->futex_ready) {
        spinlock_acquire(&b->lock);
        if (elem_find(&b->waiters, &current->futex_tag)) {
            list_remove(&current->futex_tag);
        }
        spinlock_release(&b->lock);
    } else {
        current->futex_ready = 0;
    }
    return 0;
}

static int32_t sys_futex_wake(uint32_t uaddr, uint32_t nr) {
    struct SYS_FUTEX_BUCKET *b = futex_bucket_for(uaddr, current->pml4_phys);
    int32_t woken = 0;
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&b->lock);
    while (woken < (int32_t)nr && !list_empty(&b->waiters)) {
        struct LIST_ELEM *e = list_pop_front(&b->waiters);
        struct TASK *t = list_entry(e, struct TASK, futex_tag);
        t->futex_ready = 1;
        thread_unblock(t);
        woken++;
    }
    spinlock_release(&b->lock);
    asm_restore_eflags(old);
    return woken;
}

int32_t sys_futex(uint32_t uaddr, uint32_t op, uint32_t val, uint32_t timeout) {
    if (uaddr == 0) {
        return -1;
    }
    uint32_t real_op = op & 0x7f;
    switch (real_op) {
    case FUTEX_WAIT:
        return sys_futex_wait(uaddr, val, timeout);
    case FUTEX_WAKE:
        return sys_futex_wake(uaddr, val);
    default:
        return -1;
    }
}
