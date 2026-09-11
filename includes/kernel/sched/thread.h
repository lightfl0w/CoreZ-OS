#ifndef THREAD_H
#define THREAD_H

#include "kernel/mm/pool/pool.h"
#include "kernel/sched/percpu.h"
#include "kernel/signal.h"
#include "lib/list/list.h"
#include <stdint.h>

#define THREAD_STACK_SIZE 0x4000
#define MAX_TASKS 64
#define STACK_MAGIC 0x19860726

#define RFLAGS_INIT 0x202u
#define MAX_FILES_OPEN_PER_PROC 8
typedef int32_t pid_t;
enum task_status {
    TASK_RUNNING = 1u << 0,
    TASK_READY = 1u << 1,
    TASK_BLOCKED = 1u << 2,
    TASK_WAITING = 1u << 3,
    TASK_HANGING = 1u << 4,
    TASK_DIED = 1u << 5,
    TASK_STOPPED = 1u << 6
};
#define TASK_WAKE_MASK (TASK_BLOCKED | TASK_WAITING | TASK_HANGING)
#define TASK_DEAD_MASK (TASK_DIED | TASK_HANGING)
typedef void (*thread_func)(void *);

struct thread_stack {
    uint64_t rflags;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    void (*rip)(void);
};
struct task_struct {
    uint64_t *self_kstack;
    enum task_status status;
    uint32_t pid;
    char name[16];
    uint8_t priority;
    uint8_t ticks;
    uint32_t elapsed_ticks;
    struct list_elem all_list_tag;
    struct list_elem futex_tag;
    struct list_elem wait_tag;
    uint32_t futex_ready;
    int32_t parent_pid;
    int32_t exit_status;
    uint64_t kernel_stack_top;
    uint32_t pml4_phys;
    struct virtual_addr userprog_v_addr;
    uint32_t user_brk;
    uint32_t brk_base;
    uint32_t stack_bottom;
    uint32_t signal_pending;
    uint32_t signal_mask;
    struct sigaction sigactions[NSIG];
    uint32_t cwd_inode_nr;
    uint32_t fd_table[MAX_FILES_OPEN_PER_PROC];
    uint32_t tls_base;
    uint32_t tls_selector;
    uint8_t tls_msr;
    int32_t errno;
    uint32_t pgid;
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t umask;
    uint32_t sid;
    uint64_t itimer_expire;
    uint64_t itimer_interval;
    uint64_t sigalt_sp;
    uint32_t sigalt_size;
    uint32_t sigalt_flags;
    uint32_t compat;
    uint32_t stack_magic;
    uint64_t fd_cloexec;
    uint8_t slot_used;
};
extern struct task_struct task_table[MAX_TASKS];
extern struct task_struct *idle_thread;
extern struct list thread_all_list;
extern uint32_t foreground_pid;
void thread_init(void);
void cpu_idle_init(void);
void cpu_idle(void);
void kernel_thread(char *name, uint8_t priority, thread_func function,
                   void *arg);
struct task_struct *thread_create(char *name, uint8_t priority,
                                  thread_func function, void *arg);
void schedule(void);
void switch_to(uint64_t **cur_kstack, uint64_t **next_kstack);
void kernel_thread_entry(void);
void thread_block(void);
void thread_unblock(struct task_struct *t);
void thread_sleep_ticks(uint32_t ticks);
void thread_timer_wake(void);
void thread_yield(void);
void thread_block_with_status(enum task_status status);
struct task_struct *pid2thread(int32_t pid);
void thread_exit(struct task_struct *thread_over, int need_schedule);
typedef int (*thread_all_action)(struct task_struct *, void *);
int thread_traverse_all(thread_all_action action, void *arg);
struct task_struct *thread_alloc_slot(const char *name, uint8_t priority);
void thread_ready(struct task_struct *t);
void thread_exit_current(void);
void thread_kill_pid(uint32_t pid);
#endif
