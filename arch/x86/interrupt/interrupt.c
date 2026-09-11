#include "arch/x86/interrupt/interrupt.h"
#include "drivers/block/ide.h"
#include "drivers/char/console/io.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/mouse.h"
#include "kernel/asm/stub.h"
#include "kernel/asmFunc.h"
#include "kernel/init/apic/apic.h"
#include "kernel/init/pic/pic.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/signal.h"
#include "kernel/userprog/process.h"
#include "lib/str/str.h"
volatile uint32_t tick = 0;
static const char *exc_names[32] = {"Divide Error",
                                    "Debug",
                                    "Non-Maskable Interrupt",
                                    "Breakpoint",
                                    "Overflow",
                                    "BOUND Range Exceeded",
                                    "Invalid Opcode",
                                    "Device Not Available",
                                    "Double Fault",
                                    "Coprocessor Seg Overrun",
                                    "Invalid TSS",
                                    "Segment Not Present",
                                    "Stack-Segment Fault",
                                    "General Protection",
                                    "Page Fault",
                                    "(Reserved)",
                                    "x87 FP Error",
                                    "Alignment Check",
                                    "Machine Check",
                                    "SIMD FP Exception",
                                    "Virtualization Exception",
                                    "Control Protection",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)"};
#define INT_NO_UNREGISTERED 0xFFFFu
static int handle_cow_fault(uint32_t fault_addr, uint32_t error_code) {
    if (fault_addr < USER_VADDR_START || fault_addr >= 0xc0000000) {
        return 0;
    }
    if (!(error_code & 0x2)) {
        return 0;
    }
    if (current == 0 || current->pml4_phys == 0) {
        return 0;
    }
    uint64_t *pte = pte_ptr(fault_addr);
    if (!(*pte & 1) || !(*pte & COW_FLAG)) {
        return 0;
    }
    return page_cow_resolve(fault_addr, *pte);
}
void isr_handler(struct Registers *r) {
    uint32_t n = r->int_no;
    if (n == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (handle_cow_fault((uint32_t)cr2, r->err_code)) {
            return;
        }
    }
    if ((r->cs & 3) == 3) {
        int sig = exception_to_signal((int)n);
        if (sig > 0) {
            if (sig == SIGSEGV) {
                uint64_t cr2u;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2u));
                kprintf("[user-segv] rip=%x cr2=%x err=%x name=%s\n",
                        (uint32_t)r->rip, (uint32_t)cr2u,
                        (uint32_t)r->err_code, current->name);
            }
            current->signal_pending |= (1u << sig);
            check_pending_signals(r);
            return;
        }
        set_text_color(12);
        kprintf("\n*** UNHANDLED USER EXCEPTION ***\n");
        if (n < 32) {
            kprintf("  %s (vector %d)  eip = 0x%x\n", exc_names[n], (int)n,
                    r->eip);
        }
        signal_terminate(current, SIGSEGV);
    }
    if (n == 14 && current != NULL && current->pml4_phys != 0) {
        uint64_t cr2v;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2v));
        uint32_t fa = (uint32_t)cr2v;
        if (fa >= USER_VADDR_START && fa < 0xc0000000u &&
            (fa < 0x40000000u || fa >= 0x80200000u)) {
            set_text_color(14);
            kprintf("[pf] kernel touched unmapped user addr 0x%x, killing "
                    "pid %d (%s) eip=0x%x err=%x mapped=%d\n",
                    fa, current->pid, current->name, (uint32_t)r->eip,
                    (uint32_t)r->err_code, page_is_mapped(fa));
            signal_terminate(current, SIGSEGV);
            return;
        }
    }
    set_text_color(12);
    kprintf("\n*** EXCEPTION ***\n");
    if (n < 32) {
        kprintf("  %s (vector %d)\n", exc_names[n], (int)n);
        kprintf("  err_code = 0x%x\n", r->err_code);
    } else {
        kprintf("  Unregistered interrupt (vector %d)\n", (int)n);
    }
    kprintf("  eip = 0x%x  cs = 0x%x  eflags = 0x%x\n", r->eip, r->cs,
            r->eflags);
    kprintf("  eax = 0x%x  ebx = 0x%x  ecx = 0x%x  edx = 0x%x\n", r->eax,
            r->ebx, r->ecx, r->edx);
    kprintf("  esi = 0x%x  edi = 0x%x  ebp = 0x%x  rflags = 0x%x\n", r->esi,
            r->edi, r->ebp, r->rflags);
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("  cr2 (fault addr) = 0x%x\n", (uint32_t)cr2);

    kprintf("  efer = 0x%x (NXE=%d)  cr4 = 0x%x (PAE=%d SMEP=%d SMAP=%d)\n",
            (uint32_t)asm_rdmsr(0xC0000080u),
            (int)((asm_rdmsr(0xC0000080u) >> 11) & 1), (uint32_t)asm_read_cr4(),
            (int)((asm_read_cr4() >> 5) & 1), (int)((asm_read_cr4() >> 20) & 1),
            (int)((asm_read_cr4() >> 21) & 1));
    if (n == 14) {
        page_table_dump((uint32_t)cr2);
    }
    if (cr2 >= 0xC1000000 && cr2 < 0xC1400000) {
        kprintf("  (note: fault is inside kernel virtual pool "
                "0xC1000000..0xC1400000)\n");
    } else if (cr2 >= 0xE0000000) {
        kprintf("  (note: fault is inside VRAM region 0xE0000000+)\n");
    }

    unsigned long *rbp;
    unsigned long ret_addr;
    int frame = 1;

    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    while (rbp) {
        ret_addr = *(rbp + 1);
        kprintf("[#%d] 0x%x\n", frame++, ret_addr);

        if ((unsigned long)rbp < 0x1000)
            break;
        rbp = (unsigned long *)*rbp;
    }

    asm_cli();
    for (;;) {
        asm_hlt();
    }
}

static void irq_eoi(uint32_t irq) {
    if (apic_active()) {
        lapic_eoi();
    } else {
        pic_send_eoi((uint8_t)irq);
    }
}

void irq_handler(struct Registers *r) {
    uint32_t irq = r->int_no - 32;
    if (irq == 0) {
        irq_eoi(irq);
        tick++;
        itimer_tick();
        thread_timer_wake();
        if (current != 0) {
            check_pending_signals(r);
            schedule();
        }
        return;
    }
    if (irq == 1) {
        keyboard_handler();
    }
    if (irq == 12) {
        mouse_handler();
    }
    if (irq == 14) {
        intr_hd_handler((uint8_t)r->int_no);
    }
    irq_eoi(irq);
}
