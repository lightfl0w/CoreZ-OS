#include "kernel/gui/gfx.h"

#include <stddef.h>
#include <stdint.h>


static struct gfx_fb_format fb_fmt = {32, 16, 8, 8, 8, 0, 8};

void gfx_set_fb_format(const struct gfx_fb_format *fmt) {
    if (fmt)
        fb_fmt = *fmt;
}

int gfx_fb_bpp(void) {
    return fb_fmt.bpp;
}

gfx_color gfx_over(gfx_color dst, gfx_color src, int alpha) {
    int sa = GFX_A(src);
    if (alpha < 255)
        sa = (sa * alpha + 127) / 255;
    if (sa <= 0)
        return dst;
    if (sa >= 255)
        return src | 0xFF000000u;
    int dr = GFX_R(dst), dg = GFX_G(dst), db = GFX_B(dst);
    int sr = GFX_R(src), sg = GFX_G(src), sb = GFX_B(src);
    int r = dr + ((sr - dr) * sa + 127) / 255;
    int g = dg + ((sg - dg) * sa + 127) / 255;
    int b = db + ((sb - db) * sa + 127) / 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b,
                       struct gfx_rect *out) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    if (out) {
        out->x = x0;
        out->y = y0;
        out->w = x1 - x0;
        out->h = y1 - y0;
    }
    return 1;
}

static size_t canvas_bytes(const struct gfx_canvas *c, int *ok) {
    if (!c || !c->pixels || c->w <= 0 || c->h <= 0 || c->pitch < c->w * 4) {
        *ok = 0;
        return 0;
    }
    return gfx_canvas_mapped_bytes(c, ok);
}

void gfx_px(struct gfx_canvas *c, int x, int y, gfx_color color) {
    if (!c || x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    int ok;
    size_t mapped = canvas_bytes(c, &ok);
    if (!ok)
        return;
    size_t off = (size_t)y * (size_t)c->pitch + (size_t)x * 4u;
    if (off + 4 > mapped)
        return;
    gfx_color *p = (gfx_color *)(void *)((uint8_t *)c->pixels + off);
    *p = gfx_over(*p, color, 255);
}

void gfx_fill(struct gfx_canvas *c, int x, int y, int w, int h,
              gfx_color color) {
    if (!c || w <= 0 || h <= 0)
        return;
    int ok;
    size_t mapped = canvas_bytes(c, &ok);
    if (!ok || mapped == 0)
        return;
    struct gfx_rect clip = {0, 0, c->w, c->h};
    struct gfx_rect r = {x, y, w, h}, v;
    if (!gfx_rect_intersect(clip, r, &v))
        return;
    int a = GFX_A(color);
    gfx_color rgb = color & 0x00FFFFFFu;
    for (int row = 0; row < v.h; row++) {
        size_t off = (size_t)(v.y + row) * (size_t)c->pitch + (size_t)v.x * 4u;
        if (off + (size_t)v.w * 4u > mapped)
            return;
        gfx_color *p = (gfx_color *)(void *)((uint8_t *)c->pixels + off);
        if (a >= 255) {
            for (int i = 0; i < v.w; i++)
                p[i] = rgb | 0xFF000000u;
        } else if (a > 0) {
            for (int i = 0; i < v.w; i++)
                p[i] = gfx_over(p[i], rgb, a);
        }
    }
}

void gfx_hline(struct gfx_canvas *c, int x, int y, int len, gfx_color color) {
    gfx_fill(c, x, y, len, 1, color);
}

void gfx_vline(struct gfx_canvas *c, int x, int y, int len, gfx_color color) {
    gfx_fill(c, x, y, 1, len, color);
}

void gfx_rect(struct gfx_canvas *c, int x, int y, int w, int h,
              gfx_color color) {
    if (w <= 0 || h <= 0)
        return;
    gfx_hline(c, x, y, w, color);
    gfx_hline(c, x, y + h - 1, w, color);
    gfx_vline(c, x, y, h, color);
    gfx_vline(c, x + w - 1, y, h, color);
}

static int gfx_blit_clip(struct gfx_canvas *dst, int *dx, int *dy,
                         const struct gfx_canvas *src, int *sx, int *sy, int *w,
                         int *h) {
    if (*dx < 0) {
        *sx -= *dx;
        *w += *dx;
        *dx = 0;
    }
    if (*dy < 0) {
        *sy -= *dy;
        *h += *dy;
        *dy = 0;
    }
    if (*dx + *w > dst->w)
        *w = dst->w - *dx;
    if (*dy + *h > dst->h)
        *h = dst->h - *dy;
    if (*w <= 0 || *h <= 0)
        return 0;
    if (*sx < 0) {
        *dx -= *sx;
        *w += *sx;
        *sx = 0;
    }
    if (*sy < 0) {
        *dy -= *sy;
        *h += *sy;
        *sy = 0;
    }
    if (*sx + *w > src->w)
        *w = src->w - *sx;
    if (*sy + *h > src->h)
        *h = src->h - *sy;
    if (*w <= 0 || *h <= 0)
        return 0;
    return 1;
}

void gfx_blit(struct gfx_canvas *dst, int dx, int dy,
              const struct gfx_canvas *src, int sx, int sy, int w, int h) {
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    int ok;
    size_t dsz = canvas_bytes(dst, &ok);
    if (!ok)
        return;
    size_t ssz = canvas_bytes(src, &ok);
    if (!ok)
        return;
    if (!gfx_blit_clip(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    size_t dlast = (size_t)(dy + h - 1) * (size_t)dst->pitch +
                   (size_t)(dx + w - 1) * 4u + 4u;
    size_t slast = (size_t)(sy + h - 1) * (size_t)src->pitch +
                   (size_t)(sx + w - 1) * 4u + 4u;
    if (dlast > dsz || slast > ssz)
        return;
    for (int row = 0; row < h; row++) {
        gfx_color *d = (gfx_color *)(void *)((uint8_t *)dst->pixels +
                                             (size_t)(dy + row) * dst->pitch) +
                       dx;
        const gfx_color *s =
            (const gfx_color *)(const void *)((const uint8_t *)src->pixels +
                                              (size_t)(sy + row) * src->pitch) +
            sx;
        for (int i = 0; i < w; i++)
            d[i] = s[i];
    }
}

void gfx_blit_alpha(struct gfx_canvas *dst, int dx, int dy,
                    const struct gfx_canvas *src, int sx, int sy, int w, int h,
                    int alpha) {
    if (!dst || !src || !dst->pixels || !src->pixels || alpha <= 0)
        return;
    int ok;
    size_t dsz = canvas_bytes(dst, &ok);
    if (!ok)
        return;
    size_t ssz = canvas_bytes(src, &ok);
    if (!ok)
        return;
    if (!gfx_blit_clip(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    size_t dlast = (size_t)(dy + h - 1) * (size_t)dst->pitch +
                   (size_t)(dx + w - 1) * 4u + 4u;
    size_t slast = (size_t)(sy + h - 1) * (size_t)src->pitch +
                   (size_t)(sx + w - 1) * 4u + 4u;
    if (dlast > dsz || slast > ssz)
        return;
    for (int row = 0; row < h; row++) {
        gfx_color *d = (gfx_color *)(void *)((uint8_t *)dst->pixels +
                                             (size_t)(dy + row) * dst->pitch) +
                       dx;
        const gfx_color *s =
            (const gfx_color *)(const void *)((const uint8_t *)src->pixels +
                                              (size_t)(sy + row) * src->pitch) +
            sx;
        for (int i = 0; i < w; i++) {
            gfx_color sp = s[i];
            int sa = GFX_A(sp);
            if (sa == 0)
                continue;
            d[i] = gfx_over(d[i], sp, alpha);
        }
    }
}

uint8_t gfx_round_coverage(int px, int py, int w, int h, int rad,
                           int corners) {
    int cx, cy;
    if (rad <= 0)
        return 255;
    if (px < rad) {
        if (py < rad) {
            if (!(corners & GFX_CORNER_TL))
                return 255;
            cx = rad;
            cy = rad;
        } else if (py >= h - rad) {
            if (!(corners & GFX_CORNER_BL))
                return 255;
            cx = rad;
            cy = h - rad;
        } else {
            return 255;
        }
    } else if (px >= w - rad) {
        if (py < rad) {
            if (!(corners & GFX_CORNER_TR))
                return 255;
            cx = w - rad;
            cy = rad;
        } else if (py >= h - rad) {
            if (!(corners & GFX_CORNER_BR))
                return 255;
            cx = w - rad;
            cy = h - rad;
        } else {
            return 255;
        }
    } else {
        return 255;
    }
    int rr = rad * 8;
    int rr2 = rr * rr;
    int bx = 8 * px + 1 - 8 * cx;
    int by = 8 * py + 1 - 8 * cy;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = by + 2 * sy;
        int dy2 = dy * dy;
        for (int sx = 0; sx < 4; sx++) {
            int dx = bx + 2 * sx;
            if (dx * dx + dy2 <= rr2)
                hits++;
        }
    }
    return (uint8_t)(hits * 255 / 16);
}

static void round_core(struct gfx_canvas *c, int x, int y, int w, int h,
                       int rad, int corners, gfx_color color, int alpha,
                       int outside) {
    if (!c || w <= 0 || h <= 0)
        return;
    int ok;
    size_t mapped = canvas_bytes(c, &ok);
    if (!ok || mapped == 0)
        return;
    struct gfx_rect clip = {0, 0, c->w, c->h};
    struct gfx_rect r = {x, y, w, h}, v;
    if (!gfx_rect_intersect(clip, r, &v))
        return;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int ca = (GFX_A(color) * alpha + 127) / 255;
    if (ca <= 0)
        return;
    gfx_color rgb = color & 0x00FFFFFFu;
    for (int row = 0; row < v.h; row++) {
        size_t off = (size_t)(v.y + row) * (size_t)c->pitch + (size_t)v.x * 4u;
        if (off + (size_t)v.w * 4u > mapped)
            return;
        gfx_color *p = (gfx_color *)(void *)((uint8_t *)c->pixels + off);
        int py = v.y + row - y;
        int full_row = (py >= rad && py < h - rad);
        for (int col = 0; col < v.w; col++) {
            int px = v.x + col - x;
            int cov;
            if (full_row && px >= rad && px < w - rad)
                cov = 255;
            else {
                cov = gfx_round_coverage(px, py, w, h, rad, corners);
                if (outside)
                    cov = 255 - cov;
            }
            if (cov <= 0)
                continue;
            int a = ca * cov / 255;
            if (a >= 255)
                p[col] = rgb | 0xFF000000u;
            else if (a > 0)
                p[col] = gfx_over(p[col], rgb, a);
        }
    }
}

void gfx_fill_round(struct gfx_canvas *c, int x, int y, int w, int h, int rad,
                    gfx_color color) {
    round_core(c, x, y, w, h, rad, GFX_CORNER_ALL, color, 255, 0);
}
void gfx_fill_round_a(struct gfx_canvas *c, int x, int y, int w, int h, int rad,
                      gfx_color color, int alpha) {
    round_core(c, x, y, w, h, rad, GFX_CORNER_ALL, color, alpha, 0);
}
void gfx_mask_round(struct gfx_canvas *c, int x, int y, int w, int h, int rad,
                    gfx_color color, int corners) {
    round_core(c, x, y, w, h, rad, corners, color, 255, 1);
}
void gfx_blit_round(struct gfx_canvas *dst, int dx, int dy,
                    const struct gfx_canvas *src, int sx, int sy, int w, int h,
                    int alpha, int rx, int ry, int rw, int rh, int rad,
                    int corners) {
    if (!dst || !src || !dst->pixels || !src->pixels || alpha <= 0)
        return;
    int ok;
    size_t dsz = canvas_bytes(dst, &ok);
    if (!ok)
        return;
    size_t ssz = canvas_bytes(src, &ok);
    if (!ok)
        return;
    if (!gfx_blit_clip(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > rw)
        rad = rw / 2;
    if (rad * 2 > rh)
        rad = rh / 2;
    size_t dlast = (size_t)(dy + h - 1) * (size_t)dst->pitch +
                   (size_t)(dx + w - 1) * 4u + 4u;
    size_t slast = (size_t)(sy + h - 1) * (size_t)src->pitch +
                   (size_t)(sx + w - 1) * 4u + 4u;
    if (dlast > dsz || slast > ssz)
        return;
    for (int row = 0; row < h; row++) {
        gfx_color *d = (gfx_color *)(void *)((uint8_t *)dst->pixels +
                                             (size_t)(dy + row) * dst->pitch) +
                       dx;
        const gfx_color *s =
            (const gfx_color *)(const void *)((const uint8_t *)src->pixels +
                                              (size_t)(sy + row) * src->pitch) +
            sx;
        int py = dy + row - ry;
        for (int i = 0; i < w; i++) {
            gfx_color sp = s[i];
            int sa = GFX_A(sp);
            if (sa == 0)
                continue;
            int px = dx + i - rx;
            int cov = gfx_round_coverage(px, py, rw, rh, rad, corners);
            if (cov <= 0)
                continue;
            int a = (sa * alpha + 127) / 255;
            a = a * cov / 255;
            if (a <= 0)
                continue;
            d[i] = gfx_over(d[i], sp, a * 255 / (GFX_A(sp) > 0 ? GFX_A(sp) : 1));
        }
    }
}
static int fmt_is_standard_32(const struct gfx_fb_format *f) {
    return f->bpp == 32 && f->r_pos == 16 && f->r_bits == 8 && f->g_pos == 8 &&
           f->g_bits == 8 && f->b_pos == 0 && f->b_bits == 8;
}
static uint32_t scale_channel(int v, int bits) {
    if (bits >= 8)
        return (uint32_t)v << (bits - 8);
    int max = (1 << bits) - 1;
    return (uint32_t)((v * max + 127) / 255);
}

void gfx_present(struct gfx_canvas *dst, int dx, int dy,
                 const struct gfx_canvas *src, int sx, int sy, int w, int h) {
    if (gfx_fb_bpp() != 32) {
        gfx_blit(dst, dx, dy, src, sx, sy, w, h);
        return;
    }
    if (fmt_is_standard_32(&fb_fmt)) {
        gfx_blit(dst, dx, dy, src, sx, sy, w, h);
        return;
    }
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    int ok;
    size_t dsz = canvas_bytes(dst, &ok);
    if (!ok)
        return;
    size_t ssz = canvas_bytes(src, &ok);
    if (!ok)
        return;
    if (!gfx_blit_clip(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    size_t dlast = (size_t)(dy + h - 1) * (size_t)dst->pitch +
                   (size_t)(dx + w - 1) * 4u + 4u;
    size_t slast = (size_t)(sy + h - 1) * (size_t)src->pitch +
                   (size_t)(sx + w - 1) * 4u + 4u;
    if (dlast > dsz || slast > ssz)
        return;
    for (int row = 0; row < h; row++) {
        uint32_t *d = (uint32_t *)(void *)((uint8_t *)dst->pixels +
                                           (size_t)(dy + row) * dst->pitch) +
                      dx;
        const gfx_color *s =
            (const gfx_color *)(const void *)((const uint8_t *)src->pixels +
                                              (size_t)(sy + row) * src->pitch) +
            sx;
        for (int i = 0; i < w; i++) {
            gfx_color p = s[i];
            uint32_t dev = 0;
            if (fb_fmt.r_bits)
                dev |= scale_channel(GFX_R(p), fb_fmt.r_bits) << fb_fmt.r_pos;
            if (fb_fmt.g_bits)
                dev |= scale_channel(GFX_G(p), fb_fmt.g_bits) << fb_fmt.g_pos;
            if (fb_fmt.b_bits)
                dev |= scale_channel(GFX_B(p), fb_fmt.b_bits) << fb_fmt.b_pos;
            d[i] = dev;
        }
    }
}
