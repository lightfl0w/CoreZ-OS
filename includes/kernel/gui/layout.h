#ifndef GUI_LAYOUT_H
#define GUI_LAYOUT_H

#include "kernel/gui/gfx.h"

enum GUI_LAYOUT_KIND {
    LAYOUT_MASTER_STACK = 0,
    LAYOUT_TALL,
    LAYOUT_WIDE,
    LAYOUT_MONOCLE,
    LAYOUT_COUNT
};

struct GUI_LAYOUT_PARAMS {
    enum GUI_LAYOUT_KIND kind;
    int mfact;
    int gap;
};

void layout_arrange(const struct GUI_LAYOUT_PARAMS *p, int n, struct GFX_RECT area,
                    struct GFX_RECT *out);

const char *layout_name(enum GUI_LAYOUT_KIND kind);

#endif
