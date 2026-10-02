#include "kernel/sched/percpu.h"
#include "kernel/asm_func.h"
#include "kernel/init/gdt/gdt.h"
#include "lib/str/str.h"

#define MSR_GS_BASE 0xC0000101ull
#define MSR_KERNEL_GS_BASE 0xC0000102ull

void percpu_init(void) {
    for (uint32_t i = 0; i < NR_CPU; i++) {
        memset((void *)(uintptr_t)(PER_CPU_BASE + i * PERCPU_BLOCK), 0, PERCPU_BLOCK);
    }
    asm_wrmsr(MSR_GS_BASE, PER_CPU_BASE);
    asm_wrmsr(MSR_KERNEL_GS_BASE, PER_CPU_BASE);
    uint16_t sel = SELECTOR_PER_CPU;
    __asm__ volatile("movw %0, %%gs" : : "r"(sel) : "memory");
    set_cpu_id(0);
    set_current((struct TASK *)0);
}