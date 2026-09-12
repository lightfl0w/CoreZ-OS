#include "kernel/gui/udi.h"

#include "drivers/char/console/io.h"
#include "kernel/gui/vgpu_pci.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"

extern struct udi_ops udi_virtio_ops;

#define UDI_MAX_BACKENDS 4

static struct udi_ops *backends[UDI_MAX_BACKENDS];
static int backend_count;
static struct udi_ops *active;

void udi_register(struct udi_ops *ops) {
    if (ops == 0 || backend_count >= UDI_MAX_BACKENDS)
        return;
    backends[backend_count++] = ops;
}

struct udi_ops *udi_active(void) {
    return active;
}

int udi_init(uint32_t w, uint32_t h, uint32_t bpp) {
    if (active)
        return 0;

    extern struct udi_ops udi_sw_ops;
    udi_register(&udi_virtio_ops);
    udi_register(&udi_sw_ops);

    for (int i = 0; i < backend_count; i++) {
        struct udi_ops *ops = backends[i];
        if (ops->probe && ops->probe() != 0)
            continue;
        if (ops->init(w, h, bpp) != 0)
            continue;
        active = ops;
        return 0;
    }
    return -1;
}

static struct gfx_canvas sw_front;
static struct gfx_canvas sw_back;
static uint64_t sw_handle;

static int sw_probe(void) {
    return (io_get_vram() != 0) ? 0 : -1;
}

static int sw_init(uint32_t w, uint32_t h, uint32_t bpp) {
    int pitch = io_get_pitch();
    if (pitch < (int)(w * 4))
        pitch = (int)(w * 4);
    sw_front.pixels = (gfx_color *)io_get_vram();
    sw_front.pitch = pitch;
    sw_front.w = w;
    sw_front.h = h;
    sw_front.bytes = io_get_vram_bytes();

    size_t bsz = (size_t)w * (size_t)h * 4u;
    uint8_t *bp = (uint8_t *)get_kernel_pages(
        (uint32_t)((bsz + (size_t)PAGE_SIZE - 1) / (size_t)PAGE_SIZE));
    if (bp == 0)
        return -1;
    sw_back.pixels = (gfx_color *)bp;
    sw_back.pitch = w * 4;
    sw_back.w = w;
    sw_back.h = h;
    sw_back.bytes = bsz;
    sw_handle = 1;
    return 0;
}

static int sw_alloc_buffer(uint32_t w, uint32_t h, uint32_t bpp,
                           struct udi_buffer *out) {
    out->handle = sw_handle;
    out->vmem = sw_back.pixels;
    out->w = w;
    out->h = h;
    out->bpp = bpp;
    out->size = w * h * (bpp / 8);
    return 0;
}

static void sw_free_buffer(uint64_t handle) {
    (void)handle;
}

static int sw_commit(uint64_t handle, struct gfx_rect *rects, int n) {
    (void)handle;
    for (int i = 0; i < n; i++) {
        struct gfx_rect *r = &rects[i];
        gfx_present(&sw_front, r->x, r->y, &sw_back, r->x, r->y, r->w, r->h);
    }
    return 0;
}

static void sw_wait_vblank(void) {
    mtime_sleep(16);
}

struct udi_ops udi_sw_ops = {
    "software", sw_probe,  sw_init,      sw_alloc_buffer,
    sw_free_buffer, sw_commit, sw_wait_vblank,
};
