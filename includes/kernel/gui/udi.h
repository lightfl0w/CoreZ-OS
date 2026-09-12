#ifndef GUI_UDI_H
#define GUI_UDI_H

#include "kernel/gui/gfx.h"
#include <stdint.h>

struct udi_buffer {
    uint64_t handle;
    void *vmem;
    uint32_t w;
    uint32_t h;
    uint32_t bpp;
    uint32_t size;
};

struct udi_ops {
    const char *name;
    int (*probe)(void);
    int (*init)(uint32_t w, uint32_t h, uint32_t bpp);
    int (*alloc_buffer)(uint32_t w, uint32_t h, uint32_t bpp,
                        struct udi_buffer *out);
    void (*free_buffer)(uint64_t handle);
    int (*commit)(uint64_t handle, struct gfx_rect *rects, int n);
    void (*wait_vblank)(void);
};

int udi_init(uint32_t w, uint32_t h, uint32_t bpp);
struct udi_ops *udi_active(void);
void udi_register(struct udi_ops *ops);

#endif
