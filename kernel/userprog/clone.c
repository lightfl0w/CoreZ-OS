#include "kernel/userprog/clone.h"
#include "kernel/fs/file.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/shell/pipe.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
extern void intr_exit(void);
static void build_clone_stack(struct task_struct *child,
                              struct Registers *parent_frame,
                              uint32_t user_stack) {
    uint32_t stack_top = (uint32_t)child->kernel_stack_top;
    struct Registers *child_frame =
        (struct Registers *)(stack_top - sizeof(struct Registers));
    memcpy(child_frame, parent_frame, sizeof(struct Registers));
    child_frame->eax = 0;
    child_frame->user_esp = user_stack;
    struct thread_stack *ts =
        (struct thread_stack *)((uint8_t *)child_frame -
                                sizeof(struct thread_stack));
    memset(ts, 0, sizeof(struct thread_stack));
    ts->rflags = RFLAGS_INIT;
    ts->rip = (void (*)(void))intr_exit;
    child->self_kstack = (uint64_t *)ts;
}
pid_t sys_clone(struct Registers *r) {
    uint32_t flags = r->ebx;
    uint32_t child_user_stack = r->ecx;
    struct task_struct *parent = current;
    struct task_struct *child =
        thread_alloc_slot(parent->name, parent->priority);
    if (child == NULL) {
        return -1;
    }
    child->parent_pid = (int32_t)parent->pid;
    child->cwd_inode_nr = parent->cwd_inode_nr;
    child->user_brk = parent->user_brk;
    child->brk_base = parent->brk_base;
    child->stack_bottom = parent->stack_bottom;
    for (uint32_t i = 0; i < MAX_FILES_OPEN_PER_PROC; i++) {
        child->fd_table[i] = parent->fd_table[i];
        if (child->fd_table[i] != (uint32_t)-1 &&
            child->fd_table[i] < MAX_FILE_OPEN) {
            file_table_ref(child->fd_table[i]);
        }
    }
    child->exit_status = 0;
    child->signal_mask = parent->signal_mask;
    child->signal_pending = 0;
    for (int i = 0; i < NSIG; i++) {
        child->sigactions[i] = parent->sigactions[i];
    }
    child->pml4_phys = parent->pml4_phys;
    child->tls_base = parent->tls_base;
    child->tls_selector = parent->tls_selector;
    child->tls_msr = parent->tls_msr;
    child->compat = parent->compat;
    child->pgid = parent->pgid ? parent->pgid : parent->pid;
    child->sid = parent->sid;
    if (child_user_stack == 0) {
        for (uint32_t v = USER_STACK_BOTTOM - PAGE_SIZE; v > USER_VADDR_START;
             v -= PAGE_SIZE) {
            uint64_t *pde = pde_ptr(v);
            uint64_t *pte = pte_ptr(v);
            if (pde != NULL && (*pde & 0x80)) {
                continue;
            }
            if (pte != NULL && (*pte & 1)) {
                continue;
            }
            void *p = get_a_page(v);
            if (p != 0) {
                child_user_stack = v + PAGE_SIZE;
            }
            break;
        }
        if (child_user_stack == 0) {
            return -1;
        }
    }
    build_clone_stack(child, r, child_user_stack);
    child->status = TASK_BLOCKED;
    thread_ready(child);
    (void)flags;
    return (pid_t)child->pid;
}
