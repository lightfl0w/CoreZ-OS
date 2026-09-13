#include "kernel/sched/percpu.h"
#include "kernel/asm_func.h"
#include "kernel/init/gdt/gdt.h"
void percpu_init(void) {
    uint16_t sel = SELECTOR_PER_CPU;
    __asm__ volatile("movw %0, %%gs" : : "r"(sel) : "memory");
    set_cpu_id(0);
    set_current((struct TASK *)0);
}
