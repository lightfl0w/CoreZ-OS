#ifndef GUI_DISPLAY_H
#define GUI_DISPLAY_H

#include "kernel/gui/gfx.h"
#include <stdint.h>

enum disp_surface { DISP_FRONT = 0, DISP_BACK = 1 };

struct display_ops {
    const char *name;
    int (*init)(void);
    struct gfx_canvas *(*surface)(int which);
    int (*flip)(struct gfx_rect *rects, int n);
    void (*wait_vblank)(void);
    int (*set_mode)(uint32_t w, uint32_t h, uint32_t bpp);
};

void display_init(void);
void display_register(struct display_ops *ops);
struct display_ops *display_get(void);

#endif
