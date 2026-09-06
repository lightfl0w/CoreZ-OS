#include "arch/x86/interrupt/interrupt.h"
#include "kernel/sched/thread.h"
#include "drivers/char/console/io.h"
#include "drivers/char/keyboard.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/list/list.h"
#include "lib/str/str.h"

static uint64_t ready_bitmap;
static uint64_t slot_inuse;
static uint32_t rr_cursor;
static uint64_t sleep_bitmap;
static uint32_t wake_tick[MAX_TASKS];
static uint32_t wake_pid[MAX_TASKS];

struct task_struct task_table[MAX_TASKS];
static uint32_t pid_alloc = 0;
static uint32_t died_pending = 0;
struct list thread_all_list;
struct task_struct *idle_thread;
uint32_t foreground_pid = (uint32_t)-1;
static volatile uint32_t idle_monitor;
static int mwait_ok;

static inline uint32_t task_slot(struct task_struct *t) {
    return (uint32_t)(t - task_table);
}

static void ready_enqueue(struct task_struct *t) {
    uint64_t bit = 1ULL << task_slot(t);
    if (ready_bitmap & bit)
        return;
    ready_bitmap |= bit;
    t->status = TASK_READY;
}

static void ready_remove(struct task_struct *t) {
    ready_bitmap &= ~(1ULL << task_slot(t));
}

void cpu_idle_init(void) {
    mwait_ok = asm_mwait_supported();
    if (mwait_ok)
        kprintf("[idle] Enable MONITOR/MWAIT\n");
    else
        kprintf("[idle] Enable HLT\n");
}

void cpu_idle(void) {
    if (mwait_ok)
        asm_sti_mwait((uint64_t)(uintptr_t)&idle_monitor);
    else
        asm_stihlt();
}

static void idle(void *arg) {
    for (;;) {
        thread_block();
        cpu_idle();
    }
}

void kernel_thread_entry_c(thread_func function, void *arg) {
    function(arg);
    thread_exit_current();
}

static void init_fd_table(struct task_struct *t) {
    t->fd_table[0] = 0;
    t->fd_table[1] = 1;
    t->fd_table[2] = 2;
    for (uint32_t fd_idx = 3; fd_idx < MAX_FILES_OPEN_PER_PROC; fd_idx++)
        t->fd_table[fd_idx] = (uint32_t)-1;
    t->cwd_inode_nr = 0;
}

static void init_task_struct_basic(struct task_struct *t, int32_t parent_pid) {
    t->status = TASK_READY;
    t->pid = pid_alloc++;
    t->elapsed_ticks = 0;
    t->kernel_stack_top = 0;
    t->pml4_phys = 0;
    init_fd_table(t);
    t->parent_pid = parent_pid;
    t->stack_magic = STACK_MAGIC;
    t->fd_cloexec = 0;
    t->tls_base = 0;
    t->tls_selector = 0;
    t->tls_msr = 0;
    t->errno = 0;
    t->pgid = 0;
    t->umask = 0o022;
    t->sid = 0;
    t->itimer_expire = 0;
    t->itimer_interval = 0;
    t->sigalt_sp = 0;
    t->sigalt_size = 0;
    t->sigalt_flags = 0;
    t->compat = 0;
    init_signal_state(t);

    t->all_list_tag.prev = t->all_list_tag.next = NULL;
    t->futex_tag.prev = t->futex_tag.next = NULL;
    t->futex_ready = 0;
}

static void reap_died_threads(void);

struct task_struct *thread_create(char *name, uint8_t priority,
                                  thread_func function, void *arg) {
    struct task_struct *t = thread_alloc_slot(name, priority);
    if (t == NULL) {
        return NULL;
    }
    struct thread_stack *ts =
        (struct thread_stack *)(t->kernel_stack_top -
                                sizeof(struct thread_stack));
    ts->rflags = RFLAGS_INIT;
    ts->r15 = (uint64_t)function;
    ts->r14 = (uint64_t)arg;
    ts->r13 = ts->r12 = ts->rbx = ts->rbp = 0;
    ts->rip = kernel_thread_entry;
    ready_enqueue(t);
    return t;
}

void thread_init(void) {
    cpu_idle_init();
    list_init(&thread_all_list);
    slot_inuse = 1;

    set_current(&task_table[0]);
    task_table[0].self_kstack = 0;
    task_table[0].status = TASK_RUNNING;
    task_table[0].pid = pid_alloc++;
    strcpy(task_table[0].name, "main");
    task_table[0].priority = 5;
    task_table[0].ticks = 5;
    task_table[0].elapsed_ticks = 0;
    task_table[0].kernel_stack_top = 0;
    task_table[0].pml4_phys = 0;
    init_fd_table(&task_table[0]);
    task_table[0].parent_pid = -1;
    task_table[0].stack_magic = STACK_MAGIC;
    task_table[0].fd_cloexec = 0;
    task_table[0].tls_base = 0;
    task_table[0].tls_selector = 0;
    task_table[0].tls_msr = 0;
    task_table[0].errno = 0;
    task_table[0].compat = 0;
    task_table[0].futex_tag.prev = task_table[0].futex_tag.next = NULL;
    task_table[0].futex_ready = 0;
    task_table[0].slot_used = 1;
    list_append(&thread_all_list, &task_table[0].all_list_tag);

    idle_thread = thread_create("idle", 10, idle, 0);
}

struct task_struct *thread_alloc_slot(const char *name, uint8_t priority) {
    uint64_t free = ~slot_inuse;
    if (free == 0) {
        kprintf("[thread] no free task slot (MAX_TASKS=%d)\n", MAX_TASKS);
        return NULL;
    }
    uint32_t i = (uint32_t)__builtin_ctzll(free);
    struct task_struct *t = &task_table[i];
    uint64_t stack = (uint64_t)get_kernel_pages(THREAD_STACK_SIZE / PAGE_SIZE);
    if (stack == 0) {
        kprintf("[thread] no kernel pages for stack (free pages: %d)\n",
                (int)kernel_pool_free_count());
        return NULL;
    }
    t->slot_used = 1;
    slot_inuse |= 1ULL << i;
    struct thread_stack *ts =
        (struct thread_stack *)(stack + THREAD_STACK_SIZE -
                                sizeof(struct thread_stack));
    ts->rflags = RFLAGS_INIT;
    ts->r15 = ts->r14 = ts->r13 = ts->r12 = ts->rbx = ts->rbp = 0;
    ts->rip = 0;
    t->self_kstack = (uint64_t *)ts;
    init_task_struct_basic(t, -1);
    strcpy(t->name, name);
    t->priority = priority;
    t->ticks = priority;
    t->kernel_stack_top = stack + THREAD_STACK_SIZE;
    list_append(&thread_all_list, &t->all_list_tag);

    return t;
}

void thread_ready(struct task_struct *t) {
    if (t == NULL)
        return;
    uint32_t old = asm_save_eflags();
    asm_cli();
    ready_enqueue(t);
    asm_restore_eflags(old);
}

void kernel_thread(char *name, uint8_t priority, thread_func function,
                   void *arg) {
    thread_create(name, priority, function, arg);
}

void thread_block_with_status(enum task_status status) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    ready_remove(current);
    current->status = status;
    schedule();
    asm_restore_eflags(old);
}

void thread_block(void) {
    thread_block_with_status(TASK_BLOCKED);
}

void thread_unblock(struct task_struct *t) {
    ASSERT(t != NULL);
    ASSERT(t->status & TASK_WAKE_MASK);
    thread_ready(t);
}

void thread_sleep_ticks(uint32_t ticks) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    uint32_t slot = task_slot(current);
    wake_tick[slot] = tick + ticks;
    wake_pid[slot] = current->pid;
    sleep_bitmap |= 1ULL << slot;
    ready_remove(current);
    current->status = TASK_BLOCKED;
    schedule();
    asm_restore_eflags(old);
}

void thread_timer_wake(void) {
    uint64_t m = sleep_bitmap;
    while (m) {
        uint32_t slot = (uint32_t)__builtin_ctzll(m);
        m &= m - 1;
        if ((int32_t)(tick - wake_tick[slot]) < 0) {
            continue;
        }
        sleep_bitmap &= ~(1ULL << slot);
        struct task_struct *t = &task_table[slot];
        if (t->slot_used && t->pid == wake_pid[slot] &&
            (t->status & TASK_WAKE_MASK)) {
            thread_unblock(t);
        }
    }
}

void thread_yield(void) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    ready_enqueue(current);
    current->ticks = current->priority;
    schedule();
    asm_restore_eflags(old);
}

void schedule(void) {
    ASSERT((asm_save_eflags() & 0x200) == 0);

    if (current->status == TASK_RUNNING) {
        ready_enqueue(current);
        current->ticks = current->priority;
    }

    if (died_pending > 0)
        reap_died_threads();

    if (ready_bitmap == 0)
        ready_enqueue(idle_thread);

    uint64_t avail = ready_bitmap;
    if (rr_cursor < 63)
        avail &= ~((1ULL << (rr_cursor + 1)) - 1);
    if (avail == 0)
        avail = ready_bitmap;
    uint32_t slot = (uint32_t)__builtin_ctzll(avail);
    ready_bitmap &= ~(1ULL << slot);
    struct task_struct *next = &task_table[slot];
    rr_cursor = slot;
    next->status = TASK_RUNNING;

    struct task_struct *prev = current;
    set_current(next);
    process_activate(next);
    switch_to(&prev->self_kstack, &next->self_kstack);
}

int thread_traverse_all(thread_all_action action, void *arg) {
    int stopped = 0;
    struct list_elem *e = thread_all_list.head.next;
    while (e != &thread_all_list.tail) {
        struct task_struct *t = list_entry(e, struct task_struct, all_list_tag);
        struct list_elem *next = e->next;
        int r = action(t, arg);
        if (r) {
            stopped = 1;
            break;
        }
        e = next;
    }
    return stopped;
}

void thread_exit_current(void) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    current->status = TASK_DIED;
    if (ready_bitmap & (1ULL << task_slot(current)))
        ready_remove(current);
    died_pending++;
    schedule();
    asm_restore_eflags(old);
}

void thread_kill_pid(uint32_t pid) {
    struct task_struct *t = NULL;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].slot_used && task_table[i].pid == pid) {
            t = &task_table[i];
            break;
        }
    }
    if (t == NULL || (t->status & TASK_DEAD_MASK))
        return;
    if (t->pml4_phys == 0)
        return;

    uint32_t old = asm_save_eflags();
    asm_cli();
    t->exit_status = -1;
    t->status = TASK_HANGING;
    if (ready_bitmap & (1ULL << task_slot(t)))
        ready_remove(t);

    kill_orphan_children((int32_t)t->pid);
    if (keyboard_ioq.consumer == t)
        keyboard_ioq.consumer = 0;
    if (keyboard_ioq.producer == t)
        keyboard_ioq.producer = 0;

    struct task_struct *parent = pid2thread(t->parent_pid);
    if (parent && parent->status == TASK_WAITING)
        thread_unblock(parent);

    if (t == current)
        schedule();
    asm_restore_eflags(old);
}

struct task_struct *pid2thread(int32_t pid) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].slot_used && (int32_t)task_table[i].pid == pid)
            return &task_table[i];
    }
    return NULL;
}

void thread_exit(struct task_struct *thread_over, int need_schedule) {
    (void)need_schedule;
    uint32_t old = asm_save_eflags();
    asm_cli();
    if (thread_over->status == TASK_DIED) {
        asm_restore_eflags(old);
        return;
    }
    thread_over->status = TASK_DIED;
    if (ready_bitmap & (1ULL << task_slot(thread_over)))
        ready_remove(thread_over);
    died_pending++;
    asm_restore_eflags(old);
}

static void reap_died_threads(void) {
    struct list_elem *e = thread_all_list.head.next;
    while (e != &thread_all_list.tail) {
        struct task_struct *t = list_entry(e, struct task_struct, all_list_tag);
        struct list_elem *next = e->next;
        if (t->status == TASK_DIED && t != current) {
            if (t->pml4_phys) {
                pfree(&kernel_pool, t->pml4_phys);
                t->pml4_phys = 0;
            }
            if (t->kernel_stack_top) {
                uint8_t *stack_base =
                    (uint8_t *)t->kernel_stack_top - THREAD_STACK_SIZE;
                for (uint32_t i = 0; i < THREAD_STACK_SIZE / PAGE_SIZE; i++)
                    free_kernel_page((uint32_t)(stack_base + i * PAGE_SIZE));
                t->kernel_stack_top = 0;
            }
            list_remove(&t->all_list_tag);
            t->slot_used = 0;
            slot_inuse &= ~(1ULL << task_slot(t));
            died_pending--;
        }
        e = next;
    }
}
