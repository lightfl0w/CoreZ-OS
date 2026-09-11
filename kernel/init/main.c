#include "drivers/block/ide.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/mouse.h"
#include "kernel/fs/fs.h"
#include "kernel/asmFunc.h"
#include "kernel/assert.h"
#include "kernel/init/acpi/acpi.h"
#include "kernel/init/apic/apic.h"
#include "drivers/driver_ops.h"
#include "kernel/init/gdt/gdt.h"
#include "arch/x86/interrupt/idt.h"
#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "kernel/init/pic/pic.h"
#include "kernel/init/pit/pit.h"
#include "kernel/init/smp/smp.h"
#include "kernel/init/tss/tss.h"
#include "libc/user/stdio.h"
#include "libc/user/syscall.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/ssp.h"
#include "drivers/net/net.h"
#include "kernel/shell/shell.h"
#include "lib/rand/rand.h"
#include "kernel/syscall/futex.h"
#include "kernel/syscall/syscall.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/process.h"
#include "kernel/gui/gfx.h"
#include "kernel/init/mb2.h"

#define VRAM_VIRT 0x80000000UL

void drivers_init(int min_level, int max_level) {
    struct driver_ops table[16];
    int n = 0;
    for (const struct driver_ops *d = __drivers_start;
         d != __drivers_end && n < 16; ++d) {
        if (d->level < min_level || d->level > max_level)
            continue;
        int j = n;
        while (j > 0 && table[j - 1].level > d->level) {
            table[j] = table[j - 1];
            --j;
        }
        table[j] = *d;
        ++n;
    }
    for (int i = 0; i < n; ++i) {
        if (table[i].init)
            table[i].init();
    }
}

void kmain(uint32_t magic, void *mbi_ptr, uint32_t kphys) {
    asm_write_cr4(asm_read_cr4() | 0x600);
    asm_write_cr0(asm_read_cr0() | 0x10000);
    kernel_kphys = kphys;

    stack_canary_init();
    rand_init();

    mb2_init(magic, mbi_ptr);
    const struct mb2_info *mi = mb2_get();
    struct mb2_tag_framebuffer fb = {0};
    if (mi->has_framebuffer)
        fb = mi->framebuffer;
    int fw = (int)fb.framebuffer_width;
    int fh = (int)fb.framebuffer_height;
    int fbpp = (int)fb.framebuffer_bpp;
    if (fbpp <= 0)
        fbpp = 32;
    int fpitch = (int)fb.framebuffer_pitch;
    if (fpitch <= 0)
        fpitch = fw * (fbpp / 8);
    uint32_t bytes = (uint32_t)fpitch * (uint32_t)fh;
    uintptr_t vram_virt =
        (uintptr_t)VRAM_VIRT + ((uintptr_t)fb.framebuffer_addr & 0x1FFFFFUL);
    io_init((uint8_t *)vram_virt, fw, fh, bytes, fpitch, fbpp);
    struct gfx_fb_format fmt = {
        fbpp,
        fb.color_info[0], fb.color_info[1],
        fb.color_info[2], fb.color_info[3],
        fb.color_info[4], fb.color_info[5],
    };
    gfx_set_fb_format(&fmt);

    io_clear_screen();
    mb2_dump();
    kprintf("[diag] magic=%#x mbi=%#x fb: %dx%d bpp=%d pitch=%d addr=%#x\n",
            magic, (uint32_t)(uintptr_t)mbi_ptr, fw, fh, fbpp, fpitch,
            (uint32_t)fb.framebuffer_addr);
    kprintf("[diag] vram virt=%#x rgb masks r=%d/%d g=%d/%d b=%d/%d\n",
            (uint32_t)vram_virt, fmt.r_pos, fmt.r_bits, fmt.g_pos, fmt.g_bits,
            fmt.b_pos, fmt.b_bits);

    kprintf("[init] mm\n");
    mm_init();

    kprintf("[init] gdt\n");
    gdt_init();

    kprintf("[init] percpu\n");
    percpu_init();
    set_current((struct task_struct *)0);

    kprintf("[init] tss\n");
    tss_init();

    kprintf("[init] idt\n");
    idt_init();

    kprintf("[init] syscall\n");
    syscall_init();
    futex_init();

    kprintf("[init] ppmode\n");
    kprintf("[OK] long mode (CR0.PG=1 CR4.PAE=1 EFER.LME=1 CS.L=1)\n");

    kprintf("[init] apic\n");
    if (apic_init() != 0) {
        kprintf("[WARN] apic_init failed, fallback PIC+PIT\n");
        pic_init();
        pit_init(PIT_HZ);
    }

    kprintf("[init] acpi\n");
    acpi_init();

    kprintf("[init] drivers char\n");
    drivers_init(0, 19);

    kprintf("[init] threads\n");
    thread_init();
    kprintf("[OK] kernel threads ready\n");

    set_text_color(10);
    kprintf("[OK] kernel init done, enable IRQs\n");

    drivers_init(20, 99);
    filesys_init();
    smp_init();
    net_check_guards();
    if (net_enable)
        net_init();

    kernel_thread("shell", 4, my_shell, 0);
    for (;;) {
        net_check_guards();
        cpu_idle();
        thread_yield();
    }
}
