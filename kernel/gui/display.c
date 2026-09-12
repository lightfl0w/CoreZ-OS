#include "kernel/gui/display.h"

#include "drivers/char/console/io.h"
#include "kernel/gui/udi.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"

static struct gfx_canvas back;
static uint64_t back_handle;
static int back_ok;

static struct display_ops *active;

void display_register(struct display_ops *ops) {
    active = ops;
}

struct display_ops *display_get(void) {
    return active;
}

static int udi_disp_init(void) {
    uint32_t w = (uint32_t)io_get_scrnx();
    uint32_t h = (uint32_t)io_get_scrny();
    if (w == 0 || h == 0)
        return -1;
    if (udi_init(w, h, 32) != 0)
        return -1;
    struct udi_ops *ops = udi_active();
    struct udi_buffer buf;
    if (ops->alloc_buffer(w, h, 32, &buf) != 0)
        return -1;
    back.pixels = buf.vmem;
    back.pitch = w * 4;
    back.w = w;
    back.h = h;
    back.bytes = buf.size;
    back_handle = buf.handle;
    back_ok = 1;
    return 0;
}

static struct gfx_canvas *udi_disp_surface(int which) {
    (void)which;
    return &back;
}

static int udi_disp_flip(struct gfx_rect *rects, int n) {
    struct udi_ops *ops = udi_active();
    if (ops == 0 || back_ok == 0)
        return -1;
    return ops->commit(back_handle, rects, n);
}

static void udi_disp_wait_vblank(void) {
    struct udi_ops *ops = udi_active();
    if (ops && ops->wait_vblank)
        ops->wait_vblank();
}

static int udi_disp_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    (void)w;
    (void)h;
    (void)bpp;
    return -1;
}

struct display_ops udi_display_ops = {
    "udi", udi_disp_init, udi_disp_surface, udi_disp_flip,
    udi_disp_wait_vblank, udi_disp_set_mode,
};

void display_init(void) {
    display_register(&udi_display_ops);
}
