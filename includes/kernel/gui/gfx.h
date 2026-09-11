#ifndef GUI_GFX_H
#define GUI_GFX_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t gfx_color;
#define GFX_RGBA(r, g, b, a)                                                   \
    ((gfx_color)((((uint32_t)(a)) << 24) | (((uint32_t)(r)) << 16) |           \
                 (((uint32_t)(g)) << 8) | ((uint32_t)(b))))
#define GFX_RGB(r, g, b) GFX_RGBA((r), (g), (b), 0xFFu)
#define GFX_A(c) ((int)(((c) >> 24) & 0xFFu))
#define GFX_R(c) ((int)(((c) >> 16) & 0xFFu))
#define GFX_G(c) ((int)(((c) >> 8) & 0xFFu))
#define GFX_B(c) ((int)((c) & 0xFFu))
static inline gfx_color gfx_alpha_mul(gfx_color c, int a) {
    if (a >= 255)
        return c;
    if (a <= 0)
        return c & 0x00FFFFFFu;
    int na = (GFX_A(c) * a + 127) / 255;
    return (c & 0x00FFFFFFu) | ((uint32_t)na << 24);
}
struct gfx_canvas {
    gfx_color *pixels;
    int pitch;
    int w, h;
    size_t bytes;
};

static inline int gfx_stride(const struct gfx_canvas *c) {
    return c->pitch >> 2;
}
static inline gfx_color *gfx_row(struct gfx_canvas *c, int y) {
    return (gfx_color *)(void *)((uint8_t *)c->pixels + (size_t)y *
                                                           (size_t)c->pitch);
}
static inline const gfx_color *gfx_row_c(const struct gfx_canvas *c, int y) {
    return (const gfx_color *)(const void *)((const uint8_t *)c->pixels +
                                             (size_t)y * (size_t)c->pitch);
}
static inline size_t gfx_canvas_mapped_bytes(const struct gfx_canvas *c,
                                             int *ok) {
    *ok = 1;
    if (c->bytes > 0)
        return c->bytes;
    size_t s;
    if (__builtin_mul_overflow((size_t)c->pitch, (size_t)c->h, &s)) {
        *ok = 0;
        return 0;
    }
    return s;
}

struct gfx_rect {
    int x, y, w, h;
};

struct gfx_fb_format {
    int bpp;
    int r_pos, r_bits;
    int g_pos, g_bits;
    int b_pos, b_bits;
};
void gfx_set_fb_format(const struct gfx_fb_format *fmt);
int gfx_fb_bpp(void);

gfx_color gfx_over(gfx_color dst, gfx_color src, int alpha);

void gfx_px(struct gfx_canvas *c, int x, int y, gfx_color color);

void gfx_fill(struct gfx_canvas *c, int x, int y, int w, int h, gfx_color color);
void gfx_rect(struct gfx_canvas *c, int x, int y, int w, int h, gfx_color color);
void gfx_hline(struct gfx_canvas *c, int x, int y, int len, gfx_color color);
void gfx_vline(struct gfx_canvas *c, int x, int y, int len, gfx_color color);

void gfx_blit(struct gfx_canvas *dst, int dx, int dy,
              const struct gfx_canvas *src, int sx, int sy, int w, int h);
void gfx_blit_alpha(struct gfx_canvas *dst, int dx, int dy,
                    const struct gfx_canvas *src, int sx, int sy, int w, int h,
                    int alpha);

#define GFX_CORNER_TL 1
#define GFX_CORNER_TR 2
#define GFX_CORNER_BL 4
#define GFX_CORNER_BR 8
#define GFX_CORNER_ALL                                                         \
    (GFX_CORNER_TL | GFX_CORNER_TR | GFX_CORNER_BL | GFX_CORNER_BR)

uint8_t gfx_round_coverage(int px, int py, int w, int h, int rad, int corners);
void gfx_fill_round(struct gfx_canvas *c, int x, int y, int w, int h, int rad,
                    gfx_color color);
void gfx_fill_round_a(struct gfx_canvas *c, int x, int y, int w, int h, int rad,
                      gfx_color color, int alpha);
void gfx_mask_round(struct gfx_canvas *c, int x, int y, int w, int h, int rad,
                    gfx_color color, int corners);
void gfx_blit_round(struct gfx_canvas *dst, int dx, int dy,
                    const struct gfx_canvas *src, int sx, int sy, int w, int h,
                    int alpha, int rx, int ry, int rw, int rh, int rad,
                    int corners);
int gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b,
                       struct gfx_rect *out);
void gfx_present(struct gfx_canvas *dst, int dx, int dy,
                 const struct gfx_canvas *src, int sx, int sy, int w, int h);

#endif
