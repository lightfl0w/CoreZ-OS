#include "kernel/userprog/fork.h"
#include "kernel/fs/file.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/shell/pipe.h"
#include "kernel/shell/shell.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
extern void intr_exit(void);
extern char *argv[];

static void kthread_fork_exec(void *unused) {
    (void)unused;
    if (sys_execv(final_path, (const char **)argv, NULL) == -1) {
        kprintf("execv %s failed.\n", final_path);
        sys_exit(-1);
    }
    for (;;) {
    }
}

static void mark_child_bitmap(struct TASK *child, uint32_t vaddr) {
    uint32_t bit = (vaddr - USER_VADDR_START) / PAGE_SIZE;
    if (vaddr >= USER_VADDR_START &&
        bit < child->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8) {
        bitmap_set(&child->userprog_v_addr.vaddr_bitmap, bit, 1);
    }
}

static int cow_vaddr_ok(uint32_t vaddr) {
    return vaddr >= USER_VADDR_START &&
           !(vaddr >= KERNEL_VADDR_START &&
             vaddr < KERNEL_VADDR_START + KERNEL_VADDR_SIZE) &&
           vaddr < 0xc0000000;
}

static int alloc_child_page_tables(struct TASK *child, uint64_t *pdp,
                                   uint64_t *child_pdp) {
    for (uint32_t pdp_idx = 0; pdp_idx < 3; pdp_idx++) {
        uint64_t pdp_e = pdp[pdp_idx];
        if (!(pdp_e & PTE_P) || (pdp_e & PTE_PS)) {
            continue;
        }
        uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(pdp_e));
        uint64_t child_pdp_e = child_pdp[pdp_idx];
        if (!(child_pdp_e & PTE_P) || child_pdp_e == pdp_e) {
            continue;
        }
        uint64_t *child_pd = (uint64_t *)VIRT_OF(PTE_PHYS(child_pdp_e));
        for (uint32_t pd_idx = 0; pd_idx < 512; pd_idx++) {
            uint64_t pd_e = pd[pd_idx];
            if (!(pd_e & PTE_P) || (pd_e & PTE_PS)) {
                continue;
            }
            uint32_t child_tbl = (uint32_t)palloc(&kernel_pool);
            if (child_tbl == 0) {
                return -1;
            }
            memset((void *)VIRT_OF(child_tbl), 0, PAGE_SIZE);
            child_pd[pd_idx] = (uint64_t)child_tbl | (pd_e & 0xfff);
        }
    }
    return 0;
}

static void share_user_space_cow(struct TASK *child, uint64_t *pdp,
                                 uint64_t *child_pdp) {
    for (uint32_t pdp_idx = 0; pdp_idx < 3; pdp_idx++) {
        uint64_t pdp_e = pdp[pdp_idx];
        if (!(pdp_e & PTE_P) || (pdp_e & PTE_PS)) {
            continue;
        }
        uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(pdp_e));
        uint64_t child_pdp_e = child_pdp[pdp_idx];
        if (!(child_pdp_e & PTE_P) || child_pdp_e == pdp_e) {
            continue;
        }
        uint64_t *child_pd = (uint64_t *)VIRT_OF(PTE_PHYS(child_pdp_e));
        for (uint32_t pd_idx = 0; pd_idx < 512; pd_idx++) {
            uint64_t pd_e = pd[pd_idx];
            if (!(pd_e & PTE_P) || (pd_e & PTE_PS)) {
                continue;
            }
            uint64_t *pt = (uint64_t *)VIRT_OF(PTE_PHYS(pd_e));
            uint64_t *child_pt =
                (uint64_t *)VIRT_OF(PTE_PHYS(child_pd[pd_idx]));
            for (uint32_t pte_idx = 0; pte_idx < 512; pte_idx++) {
                uint64_t pte = pt[pte_idx];
                if (!(pte & PTE_P)) {
                    continue;
                }
                uint32_t vaddr =
                    (pdp_idx << 30) + (pd_idx << 21) + (pte_idx << 12);
                if (!cow_vaddr_ok(vaddr)) {
                    continue;
                }
                uint32_t src_phy = (uint32_t)PTE_PHYS(pte);
                page_cow_share(src_phy);
                pt[pte_idx] = (pte & ~(uint64_t)PTE_W) | COW_FLAG;
                __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
                mark_child_bitmap(child, vaddr);
                child_pt[pte_idx] = (uint64_t)src_phy |
                                    (pte & (PTE_P | PTE_U | PTE_NX | 0x0f0)) |
                                    COW_FLAG;
            }
        }
    }
}

static void copy_user_space(struct TASK *parent,
                            struct TASK *child) {
    if (parent->pml4_phys == 0) {
        return;
    }

    uint64_t *parent_pml4 = (uint64_t *)VIRT_OF(parent->pml4_phys);
    uint64_t *child_pml4 = (uint64_t *)VIRT_OF(child->pml4_phys);
    uint64_t pml4e = parent_pml4[0];
    if (!(pml4e & PTE_P)) {
        return;
    }
    uint64_t *pdp = (uint64_t *)VIRT_OF(PTE_PHYS(pml4e));
    uint64_t *child_pdp = (uint64_t *)VIRT_OF(PTE_PHYS(child_pml4[0]));
    if (alloc_child_page_tables(child, pdp, child_pdp) != 0) {
        return;
    }
    share_user_space_cow(child, pdp, child_pdp);
}

static void build_child_stack(struct TASK *child,
                              struct X86_REGS *parent_frame) {
    uint32_t stack_top = (uint32_t)child->kernel_stack_top;
    struct X86_REGS *child_frame =
        (struct X86_REGS *)(stack_top - sizeof(struct X86_REGS));
    memcpy(child_frame, parent_frame, sizeof(struct X86_REGS));
    child_frame->eax = 0;
    struct TASK_STACK *ts =
        (struct TASK_STACK *)((uint8_t *)child_frame -
                                sizeof(struct TASK_STACK));
    memset(ts, 0, sizeof(struct TASK_STACK));
    ts->rflags = RFLAGS_INIT;
    ts->rip = (void (*)(void))intr_exit;
    child->self_kstack = (uint64_t *)ts;
}

pid_t sys_fork(struct X86_REGS *r) {
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
    for (int i = 0; i < NSIG; i++) {
        child->sigactions[i] = parent->sigactions[i];
    }
    create_user_vaddr_bitmap(child);
    child->pml4_phys = (uint32_t)create_page_dir();
    if (child->pml4_phys == 0) {
        return -1;
    }
    copy_user_space(parent, child);
    if (parent->pml4_phys == 0) {
        struct TASK_STACK *ts =
            (struct TASK_STACK *)((uint8_t *)child->kernel_stack_top -
                                    sizeof(struct TASK_STACK));
        memset(ts, 0, sizeof(struct TASK_STACK));
        ts->rflags = RFLAGS_INIT;
        ts->r15 = (uint64_t)kthread_fork_exec;
        ts->r14 = 0;
        ts->rip = kernel_thread_entry;
        child->self_kstack = (uint64_t *)ts;
    } else {
        build_child_stack(child, r);
    }
    child->status = TASK_BLOCKED;
    if (foreground_pid == parent->pid) {
        foreground_pid = child->pid;
    }
    thread_ready(child);
    return (pid_t)child->pid;
}
