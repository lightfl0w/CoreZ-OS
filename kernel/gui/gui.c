#include "kernel/gui/gui.h"

#include "drivers/char/keyboard.h"
#include "drivers/char/mouse.h"
#include "kernel/asmFunc.h"
#include "drivers/char/console/io.h"
#include "kernel/fs/fs.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/gui/clients.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/server.h"
#include "kernel/gui/wm.h"

extern const unsigned char _binary_font_kernel_ttf_start[];
extern const unsigned char _binary_font_kernel_ttf_end[];
#define FONT_DISK_MAX (6u * 1024u * 1024u)
static int running = 0;

static void load_embedded_font(void) {
    int len = (int)(_binary_font_kernel_ttf_end -
                    _binary_font_kernel_ttf_start);
    if (len <= 0) {
        comp_log("font: embedded blob missing");
        return;
    }
    if (font_init(_binary_font_kernel_ttf_start, len))
        comp_log("font: embedded latin subset ready");
    else
        comp_log("font: embedded font init failed");
}
static void load_disk_font(void) {
    struct stat st;
    if (sys_stat("/font_subset.ttf", &st) != 0 || st.st_size == 0 ||
        st.st_size > FONT_DISK_MAX)
        return;
    int fd = open_file("/font_subset.ttf", O_RDONLY);
    if (fd < 0)
        return;
    uint32_t pages = (st.st_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *buf = (uint8_t *)get_kernel_pages(pages);
    if (!buf) {
        close_file(fd);
        return;
    }
    uint32_t got = read_file(fd, buf, st.st_size);
    close_file(fd);
    if (got < 32) {
        comp_log("font: disk font read failed, keeping embedded");
        return;
    }
    if (font_init(buf, (int)got))
        comp_log("font: disk CJK subset loaded (中文可用)");
    else
        comp_log("font: disk font invalid, keeping embedded");
}
int gui_session_run(void) {
    if (running)
        return -1;
    running = 1;

    asm_sti();

    io_set_gui_active(1);
    comp_init();
    if (gfx_fb_bpp() != 32) {
        kprintf("gui: 真彩 GUI 需要 32bpp 线性帧缓冲, 当前 %d bpp\n",
                gfx_fb_bpp());
        io_set_gui_active(0);
        running = 0;
        return -1;
    }
    load_embedded_font();
    load_disk_font();
    wm_init_state();

    keyboard_set_gui_hook(comp_post_key);
    mouse_set_hook(comp_post_mouse);

    comp_log("compositor: session started (32bpp RGBA)");
    clients_spawn_initial();

    comp_run();

    mtime_sleep(150);

    keyboard_set_gui_hook(0);
    mouse_set_hook(0);
    io_set_gui_active(0);
    io_clear_screen();

    running = 0;
    return 0;
}
