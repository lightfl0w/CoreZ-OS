#include "kernel/init/smp/smp.h"

#include <stdint.h>

#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/percpu.h"
#include "kernel/init/apic/apic.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/tss/tss.h"
#include "kernel/init/pit/pit.h"

struct ap_boot_info {
    uint32_t gdtr;
    uint32_t stack_top;
    uint32_t ap_main;
    uint32_t index;
};

static struct gdt_desc ap_gdt[NR_CPU][GDT_ENTRIES];

struct cmp_gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
static struct cmp_gdtr ap_gdtr[NR_CPU];

static uint32_t ap_ready[NR_CPU];

static void ap_desc_init(struct gdt_desc *d, uint32_t base, uint32_t limit,
                         uint8_t attr_low, uint8_t attr_high) {
    d->limit_low = limit & 0xFFFF;
    d->base_low = base & 0xFFFF;
    d->base_mid = (base >> 16) & 0xFF;
    d->attr_low = attr_low;
    d->limit_high_attr_high = ((limit >> 16) & 0x0F) | attr_high;
    d->base_high = (base >> 24) & 0xFF;
}

static void ap_build_gdt(uint32_t idx, uint32_t percpu_base) {
    struct gdt_desc *g = ap_gdt[idx];
    memset(g, 0, sizeof(ap_gdt[idx]));

    ap_desc_init(&g[1], 0, 0, 0x9A, 0x20);
    ap_desc_init(&g[2], 0, 0xFFFFF, 0x92, 0xCF);
    ap_desc_init(&g[GDT_PER_CPU_INDEX], percpu_base, 0xFFF, 0x92, 0x40);
    tss_desc_init(&g[GDT_TSS_INDEX], (uint64_t)tss_cpu(idx),
                  sizeof(struct tss) - 1);

    ap_gdtr[idx].limit = (uint16_t)(sizeof(ap_gdt[idx]) - 1);
    ap_gdtr[idx].base = (uint64_t)g;
}

static void ap_main(uint32_t idx) {
    set_cpu_id(idx);
    set_current((struct task_struct *)0);
    __atomic_store_n(&ap_ready[idx], 1u, __ATOMIC_RELEASE);
    asm_cli();
    for (;;) {
        asm_hlt();
    }
}

static void wakeup_ap(uint32_t idx) {
    volatile struct ap_boot_info *info =
        (volatile struct ap_boot_info *)AP_BOOT_INFO_ADDR;

    uint32_t percpu_base = PER_CPU_BASE + idx * PAGE_SIZE;
    uint32_t stack = (uint32_t)palloc(&kernel_pool);
    if (stack == 0) {
        kprintf("[SMP] cpu%u: no page for stack, skip\n", idx);
        return;
    }

    ap_build_gdt(idx, percpu_base);
    tss_ap_init(idx, stack + PAGE_SIZE);
    __atomic_store_n(&ap_ready[idx], 0u, __ATOMIC_RELAXED);

    info->gdtr = (uint32_t)(uintptr_t)&ap_gdtr[idx];
    info->stack_top = stack + PAGE_SIZE;
    info->ap_main = (uint32_t)(uintptr_t)ap_main;
    info->index = idx;
    __asm__ volatile("mfence" ::: "memory");

    lapic_send_ipi_init((uint32_t)idx);
    mtime_sleep(10);
    lapic_send_ipi_sipi((uint32_t)idx, AP_TRAMPOLINE_VECTOR);
    mtime_sleep(10);
    lapic_send_ipi_sipi((uint32_t)idx, AP_TRAMPOLINE_VECTOR);

    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if (__atomic_load_n(&ap_ready[idx], __ATOMIC_ACQUIRE)) {
            return;
        }
        asm_pause();
    }
}

void smp_init(void) {
    uint32_t bsp_id = lapic_get_id();
    kprintf("[SMP] BSP LAPIC id=0x%x\n", (unsigned)bsp_id);

    uint8_t *src = _binary_ap_trampoline_bin_start;
    uint32_t size = (uint32_t)(_binary_ap_trampoline_bin_end -
                               _binary_ap_trampoline_bin_start);
    if (size == 0 || size > 0x700) {
        kprintf("[SMP] no valid trampoline(%u), APs disabled\n",
                (unsigned)size);
        return;
    }
    memcpy((void *)AP_TRAMPOLINE_ADDR, src, size);

    uint32_t online = 1;
    for (uint32_t i = 1; i < NR_CPU; i++) {
        wakeup_ap(i);
        if (__atomic_load_n(&ap_ready[i], __ATOMIC_ACQUIRE)) {
            online++;
            kprintf("[SMP] cpu%u online (apic id=0x%x)\n", i, (unsigned)i);
        } else {
            kprintf("[SMP] cpu%u no response, skip\n", i);
        }
    }

    kprintf("[SMP] %u/%u CPU online\n", (unsigned)online, (unsigned)NR_CPU);
}
