#include "kernel/init/pit/pit.h"

#include "kernel/asm_func.h"
#include "kernel/sched/thread.h"
#include "arch/x86/interrupt/interrupt.h"

#define PIT_BASE_FREQ 1193182

#define MIL_SECOND_PER_INTR (1000 / PIT_HZ)

void pit_init(uint32_t hz) {
    uint32_t divisor = PIT_BASE_FREQ / hz;

    outb(PIT_CTRL, 0x34);
    outb(PIT_CNT0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CNT0, (uint8_t)((divisor >> 8) & 0xFF));
}

static void ticks_to_sleep(uint32_t sleep_ticks) {
    thread_sleep_ticks(sleep_ticks);
}

void mtime_sleep(uint32_t m_seconds) {
    uint32_t sleep_ticks =
        (m_seconds + MIL_SECOND_PER_INTR - 1) / MIL_SECOND_PER_INTR;
    if (sleep_ticks == 0) {
        sleep_ticks = 1;
    }
    ticks_to_sleep(sleep_ticks);
}
