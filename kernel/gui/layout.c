#include "kernel/gui/layout.h"

const char *layout_name(enum GUI_LAYOUT_KIND kind) {
    switch (kind) {
    case LAYOUT_MASTER_STACK:
        return "[M]";
    case LAYOUT_TALL:
        return "[|]";
    case LAYOUT_WIDE:
        return "[-]";
    case LAYOUT_MONOCLE:
        return "[F]";
    default:
        return "[?]";
    }
}

static void rect_inset(struct GFX_RECT *r, int gap) {
    r->x += gap / 2;
    r->y += gap / 2;
    r->w -= gap;
    r->h -= gap;
    if (r->w < 16)
        r->w = 16;
    if (r->h < 16)
        r->h = 16;
}

void layout_arrange(const struct GUI_LAYOUT_PARAMS *p, int n, struct GFX_RECT area,
                    struct GFX_RECT *out) {
    if (n <= 0)
        return;

    if (n == 1 || p->kind == LAYOUT_MONOCLE) {
        for (int i = 0; i < n; i++) {
            out[i] = area;
            rect_inset(&out[i], p->gap);
        }
        return;
    }

    switch (p->kind) {
    case LAYOUT_MASTER_STACK: {
        int mw = area.w * p->mfact / 1000;

        out[0].x = area.x;
        out[0].y = area.y;
        out[0].w = mw;
        out[0].h = area.h;
        rect_inset(&out[0], p->gap);

        int sx = area.x + mw;
        int sw = area.w - mw;
        int cnt = n - 1;
        int each = area.h / cnt;
        int rem = area.h - each * cnt;
        for (int i = 0; i < cnt; i++) {
            out[i + 1].x = sx;
            out[i + 1].y = area.y + i * each;
            out[i + 1].w = sw;
            out[i + 1].h = each + (i == cnt - 1 ? rem : 0);
            rect_inset(&out[i + 1], p->gap);
        }
        break;
    }
    case LAYOUT_TALL: {
        int each = area.w / n;
        int rem = area.w - each * n;
        for (int i = 0; i < n; i++) {
            out[i].x = area.x + i * each;
            out[i].y = area.y;
            out[i].w = each + (i == n - 1 ? rem : 0);
            out[i].h = area.h;
            rect_inset(&out[i], p->gap);
        }
        break;
    }
    case LAYOUT_WIDE: {
        int each = area.h / n;
        int rem = area.h - each * n;
        for (int i = 0; i < n; i++) {
            out[i].x = area.x;
            out[i].y = area.y + i * each;
            out[i].w = area.w;
            out[i].h = each + (i == n - 1 ? rem : 0);
            rect_inset(&out[i], p->gap);
        }
        break;
    }
    default:
        for (int i = 0; i < n; i++) {
            out[i] = area;
            rect_inset(&out[i], p->gap);
        }
        break;
    }
}
