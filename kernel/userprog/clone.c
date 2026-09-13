#include "kernel/userprog/clone.h"
#include "kernel/fs/file.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/shell/pipe.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"

extern void intr_exit(void);

static void build_clone_stack(struct TASK *child,
                              struct X86_REGS *parent_frame,
                              uint32_t user_stack) {
    uint32_t stack_top = (uint32_t)child->kernel_stack_top;
    struct X86_REGS *child_frame =
        (struct X86_REGS *)(stack_top - sizeof(struct X86_REGS));
    memcpy(child_frame, parent_frame, sizeof(struct X86_REGS));
    child_frame->eax = 0;
    child_frame->user_esp = user_stack;
    struct TASK_STACK *ts =
        (struct TASK_STACK *)((uint8_t *)child_frame -
                                sizeof(struct TASK_STACK));
    memset(ts, 0, sizeof(struct TASK_STACK));
    ts->rflags = RFLAGS_INIT;
    ts->rip = (void (*)(void))intr_exit;
    child->self_kstack = (uint64_t *)ts;
}

/*
 * clone 语义（对照 Linux）：
 *   CLONE_VM    —— 与父共享地址空间（PML4 + vaddr 位图），引用计数管理，
 *                  两者谁最后退出谁释放；
 *   无 CLONE_VM —— 与 fork 相同，COW 复制一份独立地址空间；
 *   CLONE_FS / CLONE_FILES / CLONE_SIGHAND —— 内核没有可共享的 cwd/fd 表/
 *                  信号处理结构，按"带引用计数的拷贝"处理（FILE 对象本身
 *                  由 file_table_ref 计数，语义与 Linux 不带对应标志一致）；
 *   CLONE_THREAD —— 线程不进入父进程的父子树（parent_pid = -1）：父进程
 *                  退出时不会被 kill_orphan_children 连坐（线程本就共享其
 *                  地址空间），也不会被 wait/waitid 误收尸，退出后由调度器
 *                  回收内核栈与任务槽；
 *   CLONE_SETTLS —— TLS 基址取第 5 个参数（x86-64 ABI：rbp 传入），否则
 *                  继承父的 TLS；
 *   其余标志位   —— 与 Linux 一致地忽略。
 */
pid_t sys_clone(struct X86_REGS *r) {
    uint32_t flags = r->ebx;
    uint32_t child_user_stack = r->ecx;
    struct TASK *parent = current;
    struct TASK *child =
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
    child->tls_base = parent->tls_base;
    child->tls_selector = parent->tls_selector;
    child->tls_msr = parent->tls_msr;
    child->compat = parent->compat;
    child->pgid = parent->pgid ? parent->pgid : parent->pid;
    child->sid = parent->sid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
    if (flags & CLONE_SETTLS) {
        child->tls_base = (uint32_t)r->ebp;
        child->tls_selector = 0;
        child->tls_msr = 1;
    }
    if (flags & CLONE_THREAD) {
        child->parent_pid = -1;
    }

    if (flags & CLONE_VM) {
        if (parent->pml4_phys == 0) {
            goto clone_fail;
        }
        child->pml4_phys = parent->pml4_phys;
        child->userprog_v_addr = parent->userprog_v_addr;
        space_ref(child->pml4_phys);
    } else {
        create_user_vaddr_bitmap(child);
        child->pml4_phys = (uint32_t)create_page_dir();
        if (child->pml4_phys == 0) {
            goto clone_fail;
        }
        space_ref(child->pml4_phys);
        if (copy_user_space(parent, child) != 0) {
            goto clone_fail;
        }
    }

    if (child_user_stack == 0) {
        if (flags & CLONE_VM) {
            /* 共享地址空间：在共享位图里找一页空闲虚拟页做新栈 */
            for (uint32_t v = USER_STACK_BOTTOM - PAGE_SIZE;
                 v > USER_VADDR_START; v -= PAGE_SIZE) {
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
                goto clone_fail;
            }
        } else {
            /* 独立地址空间且未指定栈：沿用父当前的栈位置（COW 私有副本） */
            child_user_stack = (uint32_t)r->user_esp;
        }
    }
    build_clone_stack(child, r, child_user_stack);
    child->status = TASK_BLOCKED;
    thread_ready(child);
    return (pid_t)child->pid;

clone_fail:
    free_user_space(child, child->pml4_phys);
    for (uint32_t i = 0; i < MAX_FILES_OPEN_PER_PROC; i++) {
        uint32_t g = child->fd_table[i];
        if (g != (uint32_t)-1 && g < MAX_FILE_OPEN) {
            if (file_table[g].ref_cnt > 0) {
                file_table_unref(g);
            }
        }
    }
    thread_exit(child, 0);
    return -1;
}
