#include "kernel/userprog/wait_exit.h"
#include "kernel/signal.h"
#include "kernel/syscall/linux_abi.h"
#include "kernel/assert.h"
#include "kernel/fs/file.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
#include "lib/list/list.h"
static int vaddr_owned_by_current(struct task_struct *t, uint32_t vaddr) {
    if (vaddr < USER_VADDR_START || vaddr >= 0xc0000000) {
        return 0;
    }
    uint32_t bit_idx = (vaddr - USER_VADDR_START) / PAGE_SIZE;
    if (bit_idx >= t->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8) {
        return 0;
    }
    return bitmap_scan_test(&t->userprog_v_addr.vaddr_bitmap, bit_idx) == 1;
}
static void release_prog_resource(struct task_struct *release_thread) {
    if (release_thread->pml4_phys != 0) {
        uint64_t *pml4 = (uint64_t *)VIRT_OF(release_thread->pml4_phys);
        uint64_t pml4e = pml4[0];
        if (pml4e & 1) {
            uint64_t *pdp = (uint64_t *)VIRT_OF(PTE_PHYS(pml4e));
            for (uint32_t pdp_idx = 0; pdp_idx < 3; pdp_idx++) {
                uint64_t pdp_e = pdp[pdp_idx];
                if (!(pdp_e & 1) || (pdp_e & 0x80)) {
                    continue;
                }
                uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(pdp_e));
                uint32_t pd_remaining = 0;
                for (uint32_t pd_idx = 0; pd_idx < 512; pd_idx++) {
                    uint64_t pd_e = pd[pd_idx];
                    if (!(pd_e & 1)) {
                        continue;
                    }
                    if (pd_e & 0x80) {
                        pd_remaining++;
                        continue;
                    }
                    uint64_t *pt = (uint64_t *)VIRT_OF(PTE_PHYS(pd_e));
                    uint32_t pt_remaining = 0;
                    for (uint32_t pte_idx = 0; pte_idx < 512; pte_idx++) {
                        if (!(pt[pte_idx] & 1)) {
                            continue;
                        }
                        uint64_t vaddr = ((uint64_t)pdp_idx << 30) +
                                         ((uint64_t)pd_idx << 21) +
                                         ((uint64_t)pte_idx << 12);
                        if (!vaddr_owned_by_current(release_thread,
                                                    (uint32_t)vaddr)) {
                            pt_remaining++;
                            continue;
                        }
                        page_free_or_decref((uint32_t)PTE_PHYS(pt[pte_idx]));
                        pt[pte_idx] = 0;
                    }
                    if (pt_remaining == 0) {
                        page_free_or_decref((uint32_t)PTE_PHYS(pd_e));
                        pd[pd_idx] = 0;
                    } else {
                        pd_remaining++;
                    }
                }
                if (pd_remaining == 0) {
                    page_free_or_decref((uint32_t)PTE_PHYS(pdp_e));
                    pdp[pdp_idx] = 0;
                }
            }
        }
        if (release_thread->userprog_v_addr.vaddr_bitmap.bits != NULL) {
            uint32_t bitmap_bytes =
                release_thread->userprog_v_addr.vaddr_bitmap.btmp_bytes_len;
            uint32_t bitmap_pg_cnt = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint32_t i = 0; i < bitmap_pg_cnt; i++) {
                free_kernel_page((uint32_t)release_thread->userprog_v_addr
                                     .vaddr_bitmap.bits +
                                 i * PAGE_SIZE);
            }
            release_thread->userprog_v_addr.vaddr_bitmap.bits = NULL;
        }
    }
    for (uint32_t fd_idx = 3; fd_idx < MAX_FILES_OPEN_PER_PROC; fd_idx++) {
        if (release_thread->fd_table[fd_idx] != (uint32_t)-1) {
            close_file((int)fd_idx);
        }
    }
}

void kill_orphan_children(int32_t parent_pid) {
    struct list_elem *e = thread_all_list.head.next;
    while (e != &thread_all_list.tail) {
        struct task_struct *t = list_entry(e, struct task_struct, all_list_tag);
        struct list_elem *next = e->next;
        if (t->parent_pid == parent_pid && t->status != TASK_DIED) {
            if (t->status != TASK_HANGING)
                release_prog_resource(t);
            thread_exit(t, 0);
        }
        e = next;
    }
}

static int find_hanging_child(struct list_elem *pelem, int32_t ppid) {
    struct task_struct *t = list_entry(pelem, struct task_struct, all_list_tag);
    return (t->parent_pid == ppid && t->status == TASK_HANGING);
}
static int find_child(struct list_elem *pelem, int32_t ppid) {
    struct task_struct *t = list_entry(pelem, struct task_struct, all_list_tag);
    return (t->parent_pid == ppid);
}
pid_t sys_wait(int32_t *status) {
    struct task_struct *parent = current;
    int32_t ignored_status;
    if (status == NULL) {
        status = &ignored_status;
    }
    for (;;) {
        struct list_elem *e = thread_all_list.head.next;
        while (e != &thread_all_list.tail) {
            struct list_elem *next = e->next;
            if (find_hanging_child(e, (int32_t)parent->pid)) {
                struct task_struct *child =
                    list_entry(e, struct task_struct, all_list_tag);
                *status = child->exit_status;
                uint32_t child_pid = child->pid;
                thread_exit(child, 0);
                return child_pid;
            }
            e = next;
        }
        struct list_elem *child = thread_all_list.head.next;
        while (child != &thread_all_list.tail) {
            if (find_child(child, (int32_t)parent->pid)) {
                break;
            }
            child = child->next;
        }
        if (child == &thread_all_list.tail) {
            return -1;
        }
        thread_block_with_status(TASK_WAITING);
    }
}
void proc_exit(struct task_struct *cur, int status) {
    cur->exit_status = status;

    kill_orphan_children((int32_t)cur->pid);
    release_prog_resource(cur);
    struct task_struct *parent = pid2thread(cur->parent_pid);
    if (parent && parent->status == TASK_WAITING) {
        thread_unblock(parent);
    }
    thread_block_with_status(TASK_HANGING);
}
void sys_exit(int32_t status) {
    proc_exit(current, status);
}

static int waitid_match(struct task_struct *t, int idtype, int32_t id) {
    if (idtype == LINUX_P_PID) {
        return t->pid == (uint32_t)id;
    }
    if (idtype == LINUX_P_PGID) {
        return (t->pgid ? t->pgid : t->pid) == (uint32_t)id;
    }
    return 1;
}
int sys_waitid(int idtype, int32_t id, struct LINUX_SIGINFO *info,
               uint32_t options) {
    struct task_struct *parent = current;
    for (;;) {
        int any_child = 0;
        struct list_elem *e = thread_all_list.head.next;
        while (e != &thread_all_list.tail) {
            struct task_struct *t =
                list_entry(e, struct task_struct, all_list_tag);
            struct list_elem *next = e->next;
            if (t->parent_pid != (int32_t)parent->pid ||
                t->status == TASK_DIED) {
                e = next;
                continue;
            }
            any_child = 1;
            if (waitid_match(t, idtype, id) && t->status == TASK_HANGING) {
                if (info != NULL) {
                    int sig = t->exit_status >= 128 ? t->exit_status - 128 : 0;
                    info->si_signo = SIGCHLD;
                    info->si_errno = 0;
                    info->si_code =
                        sig ? LINUX_CLD_KILLED : LINUX_CLD_EXITED;
                    info->si_pid = (int32_t)t->pid;
                    info->si_uid = 0;
                    info->si_status = sig ? sig : t->exit_status;
                    info->si_utime = t->elapsed_ticks;
                    info->si_stime = 0;
                }
                if (!(options & LINUX_WNOWAIT)) {
                    thread_exit(t, 0);
                }
                return 0;
            }
            e = next;
        }
        if (!any_child) {
            return -1;
        }
        if (options & LINUX_WNOHANG) {
            if (info != NULL) {
                info->si_signo = 0;
            }
            return 0;
        }
        thread_block_with_status(TASK_WAITING);
    }
}
