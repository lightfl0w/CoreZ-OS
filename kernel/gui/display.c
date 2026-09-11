#include "kernel/gui/display.h"

#include "drivers/char/console/io.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"

static struct display_ops *active;

void display_register(struct display_ops *ops) {
    active = ops;
}

struct display_ops *display_get(void) {
    return active;
}

static struct gfx_canvas lfb_front;
static struct gfx_canvas lfb_back;
static int lfb_has_back;

static int lfb_init(void) {
    int w = io_get_scrnx();
    int h = io_get_scrny();
    int pitch = io_get_pitch();
    if (pitch < w * 4)
        pitch = w * 4;
    if (w <= 0 || h <= 0)
        return -1;

    lfb_front.pixels = (gfx_color *)io_get_vram();
    lfb_front.pitch = pitch;
    lfb_front.w = w;
    lfb_front.h = h;
    lfb_front.bytes = io_get_vram_bytes();

    size_t bsz = (size_t)w * (size_t)h * 4u;
    uint8_t *bp = (uint8_t *)get_kernel_pages(
        (uint32_t)((bsz + (size_t)PAGE_SIZE - 1) / (size_t)PAGE_SIZE));
    if (bp) {
        lfb_back.pixels = (gfx_color *)bp;
        lfb_back.pitch = w * 4;
        lfb_back.w = w;
        lfb_back.h = h;
        lfb_back.bytes = bsz;
        lfb_has_back = 1;
    } else {
        lfb_back = lfb_front;
        lfb_has_back = 0;
    }
    return 0;
}

static struct gfx_canvas *lfb_surface(int which) {
    if (which == DISP_BACK)
        return &lfb_back;
    return &lfb_front;
}

static int lfb_flip(struct gfx_rect *rects, int n) {
    if (!lfb_has_back || n <= 0)
        return 0;
    for (int i = 0; i < n; i++) {
        struct gfx_rect *r = &rects[i];
        gfx_present(&lfb_front, r->x, r->y, &lfb_back, r->x, r->y, r->w, r->h);
    }
    return 0;
}

static void lfb_wait_vblank(void) {
    mtime_sleep(16);
}

static int lfb_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    (void)w;
    (void)h;
    (void)bpp;
    return -1;
}

struct display_ops lfb_display_ops = {
    "lfb",    lfb_init,      lfb_surface, lfb_flip,
    lfb_wait_vblank, lfb_set_mode,
};

void display_init(void) {
    display_register(&lfb_display_ops);
}
