#include "kernel/userprog/process.h"
#include "kernel/asm/stub.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "kernel/init/gdt/gdt.h"
#include "drivers/char/console/io.h"
#include "kernel/init/tss/tss.h"
#include "lib/str/str.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/userprog/exec.h"
#define EFLAGS_MBS (1 << 1)
#define EFLAGS_IF_1 (1 << 9)
#define EFLAGS_IOPL_0 0
#define MSR_FS_BASE 0xC0000100ull

void start_process(void *arg) {
    char *path = (char *)arg;
    if (current->pml4_phys != 0) {
        process_activate(current);
    }
    const char *argv[] = {path, (const char *)0};
    if (sys_execv(path, argv, NULL) == -1) {
        kprintf("[exec] process_execute: load '%s' failed\n", path);
    }
    thread_exit_current();
}

void page_dir_activate(struct TASK *task) {
    if (task->pml4_phys == 0)
        return;
    asm_write_cr3((uint64_t)task->pml4_phys);
}

void process_activate(struct TASK *task) {
    if (task->pml4_phys != 0) {
        page_dir_activate(task);
        if (task->tls_msr) {
            asm_wrmsr(MSR_FS_BASE, (uint64_t)task->tls_base);
        } else if (task->tls_selector != 0) {
            tls_desc_set_base(task->tls_base);
        }
    } else {
        asm_write_cr3(kernel_pml4);
    }
    tss_update_rsp0(task);
}

uint32_t *create_page_dir(void) {
    uint64_t pml4_phys = palloc_pages(&kernel_pool, 1);
    if (pml4_phys == 0) {
        return 0;
    }
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    memset(pml4, 0, PAGE_SIZE);

    uint64_t pdp_phys = palloc_pages(&kernel_pool, 1);
    if (pdp_phys == 0) {
        pfree(&kernel_pool, (uint32_t)pml4_phys);
        return 0;
    }
    uint64_t *pdp = phys_to_virt(pdp_phys);
    memset(pdp, 0, PAGE_SIZE);
    pml4[0] = pdp_phys | 7;

    {
        uint64_t pd0_phys = palloc_pages(&kernel_pool, 1);
        if (pd0_phys == 0) {
            pfree(&kernel_pool, (uint32_t)pdp_phys);
            pfree(&kernel_pool, (uint32_t)pml4_phys);
            return 0;
        }
        uint64_t *pd0 = phys_to_virt(pd0_phys);
        memset(pd0, 0, PAGE_SIZE);
        uint64_t *loader_pd_low = phys_to_virt(0x92000);
        pd0[0] = loader_pd_low[0] & ~(uint64_t)PTE_U;
        pdp[0] = pd0_phys | 7;
    }

    {
        uint64_t *kpml4 = phys_to_virt(kernel_pml4);
        uint64_t kpdp0_phys = kpml4[0] & ~0xFFFull;
        uint64_t *kpdp0 = phys_to_virt(kpdp0_phys);
        pdp[1] = kpdp0[1];
    }

    uint64_t pd2_phys = palloc_pages(&kernel_pool, 1);
    if (pd2_phys == 0) {
        pfree(&kernel_pool, (uint32_t)PTE_PHYS(pdp[0]));
        pfree(&kernel_pool, (uint32_t)pdp_phys);
        pfree(&kernel_pool, (uint32_t)pml4_phys);
        return 0;
    }
    uint64_t *pd2 = phys_to_virt(pd2_phys);
    memset(pd2, 0, PAGE_SIZE);
    {
        uint64_t *loader_pd_lfb = phys_to_virt(0x94000);
        for (uint32_t i = 0; i < 512; i++) {
            if (loader_pd_lfb[i] & PTE_P)
                pd2[i] = loader_pd_lfb[i];
        }
    }
    pdp[2] = pd2_phys | 7;

    {
        uint64_t pd3_phys = palloc_pages(&kernel_pool, 1);
        if (pd3_phys == 0) {
            pfree(&kernel_pool, (uint32_t)PTE_PHYS(pdp[2]));
            pfree(&kernel_pool, (uint32_t)PTE_PHYS(pdp[0]));
            pfree(&kernel_pool, (uint32_t)pdp_phys);
            pfree(&kernel_pool, (uint32_t)pml4_phys);
            return 0;
        }
        uint64_t *pd3 = phys_to_virt(pd3_phys);
        memset(pd3, 0, PAGE_SIZE);
        memcpy(pd3, phys_to_virt(0x98000), PAGE_SIZE);
        pdp[3] = pd3_phys | 7;
    }

    return (uint32_t *)(uintptr_t)pml4_phys;
}

void free_user_space(struct TASK *t, uint32_t pml4_phys) {
    if (t == NULL) {
        return;
    }
    if (pml4_phys != 0) {
        uint64_t *pml4 = phys_to_virt(pml4_phys);
        uint64_t pml4e = pml4[0];
        if (pml4e & 1) {
            uint64_t *pdp = phys_to_virt(PTE_PHYS(pml4e));
            for (uint32_t pdp_idx = 0; pdp_idx < 3; pdp_idx++) {
                uint64_t pdp_e = pdp[pdp_idx];
                if (!(pdp_e & 1) || (pdp_e & 0x80)) {
                    continue;
                }
                uint64_t *pd = phys_to_virt(PTE_PHYS(pdp_e));
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
                    uint64_t *pt = phys_to_virt(PTE_PHYS(pd_e));
                    uint32_t pt_remaining = 0;
                    for (uint32_t pte_idx = 0; pte_idx < 512; pte_idx++) {
                        if (!(pt[pte_idx] & 1)) {
                            continue;
                        }
                        uint64_t vaddr =
                            ((uint64_t)pdp_idx << 30) +
                            ((uint64_t)pd_idx << 21) + ((uint64_t)pte_idx << 12);
                        uint32_t bit =
                            (uint32_t)((vaddr - USER_VADDR_START) / PAGE_SIZE);
                        if (vaddr < USER_VADDR_START || vaddr >= 0xc0000000u ||
                            bit >= t->userprog_v_addr.vaddr_bitmap
                                       .btmp_bytes_len *
                                       8 ||
                            bitmap_scan_test(&t->userprog_v_addr.vaddr_bitmap,
                                             bit) != 1) {
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
        pfree(&kernel_pool, pml4_phys);
    }
    if (t->userprog_v_addr.vaddr_bitmap.bits != NULL) {
        uint32_t bytes = t->userprog_v_addr.vaddr_bitmap.btmp_bytes_len;
        uint32_t pg_cnt = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint32_t i = 0; i < pg_cnt; i++) {
            free_kernel_page(
                (uint32_t)t->userprog_v_addr.vaddr_bitmap.bits + i * PAGE_SIZE);
        }
        t->userprog_v_addr.vaddr_bitmap.bits = NULL;
    }
    t->pml4_phys = 0;
}

void create_user_vaddr_bitmap(struct TASK *user_prog) {
    user_prog->userprog_v_addr.vaddr_start = USER_VADDR_START;
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP(
        (0xc0000000 - USER_VADDR_START) / PAGE_SIZE / 8, PAGE_SIZE);
    user_prog->userprog_v_addr.vaddr_bitmap.bits =
        (uint8_t *)get_kernel_pages(bitmap_pg_cnt);
    user_prog->userprog_v_addr.vaddr_bitmap.btmp_bytes_len =
        (0xc0000000 - USER_VADDR_START) / PAGE_SIZE / 8;
    bitmap_init(&user_prog->userprog_v_addr.vaddr_bitmap);
}

void process_execute(char *path, char *name) {
    struct TASK *thread = thread_alloc_slot(name, DEFAULT_PRIO);
    struct TASK_STACK *ts =
        (struct TASK_STACK *)(thread->kernel_stack_top -
                                sizeof(struct TASK_STACK));
    ts->rflags = RFLAGS_INIT;
    ts->r15 = (uint64_t)start_process;
    ts->r14 = (uint64_t)path;
    ts->r13 = 0;
    ts->r12 = 0;
    ts->rbx = 0;
    ts->rbp = 0;
    ts->rip = kernel_thread_entry;
    create_user_vaddr_bitmap(thread);
    thread->pml4_phys = (uint32_t)create_page_dir();
    thread->user_brk = 0;
    kprintf("[procexec] '%s' pid=%d pml4_phys=0x%x\n", path, thread->pid,
            thread->pml4_phys);
    thread_ready(thread);
}
