#include "kernel/init/apic/apic.h"

#include <stdint.h>

#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/init/pit/pit.h"

#define MSR_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1u << 8)

#define LAPIC_SVR 0x0F0
#define LAPIC_EOI 0x0B0
#define LAPIC_LVT_T 0x320
#define LAPIC_LVT_PMC 0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LVIT_MASK (1u << 16)
#define LAPIC_DIV1 0xB

#define IOAPIC_BASE 0xFEC00000u
#define IOAPIC_VER 0x01
#define IOREG_TABLE 0x10
#define IOAPIC_MAX_PINS 23

#define APIC_VADDR_IOAPIC 0x40000000u
#define APIC_VADDR_LAPIC 0x40200000u

#define IO_IR_MASK (1u << 16)
#define IO_IR_TRIGGER (1u << 15)
#define IO_IR_POLARITY (1u << 13)

#define PIN_KEYBOARD 1
#define PIN_MOUSE 12
#define PIN_IDE 14

#define VECTOR_BASE 0x20

static volatile uint32_t *lapic;
static volatile uint32_t *ioapic;
static int s_apic_active;

int apic_active(void) {
    return s_apic_active;
}

static uint32_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return lo;
}

static void wrmsr(uint32_t msr, uint32_t lo) {
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(0), "c"(msr) : "memory");
}

static void lapic_write(uint32_t off, uint32_t v) {
    lapic[off / 4] = v;
}

static uint32_t lapic_read(uint32_t off) {
    return lapic[off / 4];
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

#define LAPIC_ID 0x020
#define LAPIC_ICR 0x300
#define LAPIC_ICR_HIGH 0x310
#define ICR_IRR_MASK 0x00001000

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

static void lapic_send_icr(uint32_t apic_id, uint32_t low) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR, low);

    uint32_t spins = 0;
    while (lapic_read(LAPIC_ICR) & ICR_IRR_MASK) {
        asm_pause();
        if (++spins > 1000000u)
            break;
    }
}

void lapic_send_ipi_init(uint32_t apic_id) {
    lapic_send_icr(apic_id, 0x00000500u);
}

void lapic_send_ipi_sipi(uint32_t apic_id, uint32_t vector) {
    lapic_send_icr(apic_id, 0x00000600u | (vector & 0xFF));
}

static uint32_t ioapic_read(uint32_t reg) {
    ioapic[0] = reg;
    return ioapic[4];
}

static void ioapic_write(uint32_t reg, uint32_t v) {
    ioapic[0] = reg;
    ioapic[4] = v;
}

static void pit_start_oneshot(uint16_t count) {
    outb(0x43, 0x30);
    outb(0x40, (uint8_t)(count & 0xFF));
    outb(0x40, (uint8_t)((count >> 8) & 0xFF));
}

static uint16_t pit_read_count0(void) {
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t lapic_timer_calibrate(void) {
    const uint16_t pit_count = 11932;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV1);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    pit_start_oneshot(pit_count);
    while (pit_read_count0() != 0) {
        asm_pause();
    }
    uint32_t cur = lapic_read(LAPIC_TIMER_CUR);
    return 0xFFFFFFFFu - cur;
}

static void ioapic_route(uint32_t pin, uint32_t vector) {
    uint32_t reg = IOREG_TABLE + 2 * pin;

    ioapic_write(reg, vector);
    ioapic_write(reg + 1, 0);
}

static void ioapic_init(void) {
    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint32_t maxpin = (ver >> 16) & 0xFF;
    if (maxpin > IOAPIC_MAX_PINS) {
        maxpin = IOAPIC_MAX_PINS;
    }

    for (uint32_t pin = 0; pin <= maxpin; pin++) {
        uint32_t reg = IOREG_TABLE + 2 * pin;
        ioapic_write(reg, VECTOR_BASE | IO_IR_TRIGGER | IO_IR_MASK);
        ioapic_write(reg + 1, 0);
    }

    ioapic_route(PIN_KEYBOARD, VECTOR_BASE + PIN_KEYBOARD);
    ioapic_route(PIN_MOUSE, VECTOR_BASE + PIN_MOUSE);
    ioapic_route(PIN_IDE, VECTOR_BASE + PIN_IDE);
}

static void disable_pic(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    outb(0x20, 0x0A);
    outb(0xA0, 0x0A);
}

int apic_init(void) {
    uint32_t base = rdmsr(MSR_APIC_BASE);

    if (!(base & APIC_BASE_ENABLE)) {
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, base);
    }

    lapic = (volatile uint32_t *)APIC_VADDR_LAPIC;
    ioapic = (volatile uint32_t *)APIC_VADDR_IOAPIC;
    if (lapic == 0 || ioapic == 0) {
        return -1;
    }

    lapic_write(LAPIC_LVT_LINT0, LVIT_MASK);
    lapic_write(LAPIC_LVT_LINT1, LVIT_MASK);
    lapic_write(LAPIC_LVT_PMC, LVIT_MASK);

    lapic_write(LAPIC_SVR, (lapic_read(LAPIC_SVR) & ~0xFFu) | 0x100u | 0x2F);

    uint32_t per_tick = lapic_timer_calibrate();
    if (per_tick == 0) {
        per_tick = 1;
    }

    lapic_write(LAPIC_LVT_T, VECTOR_BASE | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV1);
    lapic_write(LAPIC_TIMER_INIT, per_tick);
    kprintf("[APIC] id=%u lvt=0x%x cur=0x%x per_tick=%u\n",
            (unsigned)lapic_get_id(), (unsigned)lapic_read(LAPIC_LVT_T),
            (unsigned)lapic_read(LAPIC_TIMER_CUR), (unsigned)per_tick);

    ioapic_init();
    disable_pic();

    s_apic_active = 1;
    return 0;
}
