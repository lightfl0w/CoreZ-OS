#include "kernel/gui/clients.h"

#include "drivers/char/keyboard.h"
#include "arch/x86/interrupt/interrupt.h"
#include "lib/str/str.h"
#include "kernel/sched/thread.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/server.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/theme.h"
#include "kernel/gui/wm.h"

struct demo_client {
    struct wl_client *conn;
    struct wl_surface *surf;
    struct shm_pool *pool;
    int w, h;
    uint32_t frame_interval;
    uint32_t last_frame;
    void (*render)(struct demo_client *dc);
    void (*on_key)(struct demo_client *dc, int scancode, int mods);
};

extern void (*log_hook)(const char *s);

#define UI_FONT_PX 13
static void canvas_of(struct gfx_canvas *cv, struct demo_client *dc) {
    cv->pixels = (gfx_color *)dc->pool->data;
    cv->pitch = dc->w * 4;
    cv->w = dc->w;
    cv->h = dc->h;
    cv->bytes = (size_t)dc->w * (size_t)dc->h * 4u;
}
static int buffer_resize(struct demo_client *dc, int w, int h) {
    if (w <= 0 || h <= 0)
        return -1;
    if (dc->pool && dc->w == w && dc->h == h)
        return 0;
    if (dc->pool) {
        comp_destroy_surface_pool(dc->surf, &dc->pool);
    }
    dc->pool = shm_pool_create((uint32_t)(w * h * 4));
    if (!dc->pool)
        return -1;
    dc->w = w;
    dc->h = h;
    return 0;
}

static void attach_commit(struct demo_client *dc) {
    if (!dc->pool)
        return;
    wl_surface_attach(dc->surf, dc->pool, dc->w, dc->h);
    wl_surface_commit(dc->surf);
}

static void client_main(struct demo_client *dc) {
    for (;;) {
        struct wl_event ev;
        if (wl_display_dispatch(dc->conn, &ev) != 0)
            break;
        switch (ev.type) {
        case WL_EV_CONFIGURE:
            if (buffer_resize(dc, (int)ev.a, (int)ev.b) == 0) {
                dc->render(dc);
                attach_commit(dc);
            }
            break;
        case WL_EV_FRAME:
            if (dc->pool && tick - dc->last_frame >= dc->frame_interval) {
                dc->last_frame = tick;
                dc->render(dc);
                wl_surface_commit(dc->surf);
            }
            break;
        case WL_EV_KEY:
            if (dc->on_key && ev.b)
                dc->on_key(dc, (int)ev.a, (int)ev.c);
            break;
        case WL_EV_CLOSE:
            goto out;
        default:
            break;
        }
    }
out:
    wm_unmanage(dc->surf);
    wl_surface_destroy(dc->surf);
    if (dc->pool)
        comp_destroy_surface_pool(dc->surf, &dc->pool);
    wl_display_disconnect(dc->conn);
    dc->conn = 0;
    thread_exit_current();
}

#define TERM_LINES 40
#define TERM_COLS 96
static char term_buf[TERM_LINES][TERM_COLS];
static int term_head = 0;
static int term_count = 0;
static int term_col = 0;
static int term_cw = 8;
static int term_lh = 16;

static void term_newline(void) {
    term_head = (term_head + 1) % TERM_LINES;
    if (term_count < TERM_LINES)
        term_count++;
    memset(term_buf[term_head], 0, TERM_COLS);
    term_col = 0;
}

static void term_putc(char ch) {
    if (ch == '\n') {
        term_newline();
        return;
    }
    if (term_col >= TERM_COLS - 1)
        term_newline();
    term_buf[term_head][term_col++] = ch;
}

static void term_puts(const char *s) {
    while (*s)
        term_putc(*s++);
}

static void term_log_hook(const char *s) {
    term_puts("[log] ");
    term_puts(s);
    term_putc('\n');
}

static void term_render(struct demo_client *dc) {
    struct gfx_canvas cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, GFX_RGB(12, 15, 20));
    gfx_fill(&cv, 0, 0, dc->w, 2, TH_ACCENT);

    term_cw = font_text_width("M", UI_FONT_PX);
    if (term_cw <= 0)
        term_cw = 8;
    term_lh = font_line_height(UI_FONT_PX);
    if (term_lh <= 0)
        term_lh = 16;
    int rows = dc->h / term_lh;
    int cols = dc->w / term_cw;
    if (rows > TERM_LINES)
        rows = TERM_LINES;
    if (cols > TERM_COLS - 1)
        cols = TERM_COLS - 1;

    int start = term_head - term_count + 1;
    if (start < 0)
        start += TERM_LINES;
    int first = term_count - rows;
    if (first < 0)
        first = 0;
    for (int r = first; r < term_count; r++) {
        int li = (start + r) % TERM_LINES;
        char line[TERM_COLS];
        int n = 0;
        while (n < cols && term_buf[li][n]) {
            line[n] = term_buf[li][n];
            n++;
        }
        line[n] = 0;
        font_draw(&cv, 4, (r - first) * term_lh + 2, line, UI_FONT_PX,
                  GFX_RGB(120, 224, 150));
    }
    if ((tick / 25) & 1) {
        gfx_fill(&cv, 4 + term_col * term_cw,
                 (term_count - first - 1) * term_lh + 2, term_cw, term_lh,
                 GFX_RGB(120, 224, 150));
    }
}

static void term_on_key(struct demo_client *dc, int scancode, int mods) {
    (void)mods;
    char ch = keyboard_translate((uint8_t)scancode, mods & MOD_SHIFT);
    if (!ch)
        return;
    if (ch == '\b') {
        if (term_col > 0)
            term_buf[term_head][--term_col] = 0;
    } else {
        term_putc(ch);
    }
    dc->render(dc);
    wl_surface_commit(dc->surf);
}

static void term_thread(void *arg) {
    (void)arg;
    struct demo_client dc;
    memset(&dc, 0, sizeof(dc));
    dc.conn = wl_display_connect("term");
    if (!dc.conn) {
        thread_exit_current();
        return;
    }
    dc.surf =
        wl_compositor_create_surface(dc.conn, "term - wayland-ish client");
    if (!dc.surf) {
        wl_display_disconnect(dc.conn);
        thread_exit_current();
        return;
    }
    dc.render = term_render;
    dc.on_key = term_on_key;
    dc.frame_interval = 25;
    log_hook = term_log_hook;
    wm_manage(dc.surf);
    client_main(&dc);
    log_hook = 0;
}

static void clock_render(struct demo_client *dc) {
    struct gfx_canvas cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, GFX_RGB(10, 12, 24));

    uint32_t secs = tick / 100;
    uint32_t hh = (secs / 3600) % 24;
    uint32_t mm = (secs / 60) % 60;
    uint32_t ss = secs % 60;
    char tbuf[12];
    tbuf[0] = (char)('0' + hh / 10);
    tbuf[1] = (char)('0' + hh % 10);
    tbuf[2] = ':';
    tbuf[3] = (char)('0' + mm / 10);
    tbuf[4] = (char)('0' + mm % 10);
    tbuf[5] = ':';
    tbuf[6] = (char)('0' + ss / 10);
    tbuf[7] = (char)('0' + ss % 10);
    tbuf[8] = 0;

    int px = dc->h / 5;
    if (px < 16)
        px = 16;
    if (px > 96)
        px = 96;
    int tw = font_text_width(tbuf, px);
    int th = font_line_height(px);
    int x = (dc->w - tw) / 2;
    int y = (dc->h - th) / 2;
    font_draw(&cv, x, y, tbuf, px, GFX_RGB(120, 190, 255));

    font_draw(&cv, 8, 8, "frame-callback driven clock", UI_FONT_PX,
              TH_MUTED);
    font_draw(&cv, 8, dc->h - font_line_height(UI_FONT_PX) - 6,
              "uptime since boot", UI_FONT_PX, TH_MUTED);
}

static void clock_thread(void *arg) {
    (void)arg;
    struct demo_client dc;
    memset(&dc, 0, sizeof(dc));
    dc.conn = wl_display_connect("clock");
    if (!dc.conn) {
        thread_exit_current();
        return;
    }
    dc.surf = wl_compositor_create_surface(dc.conn, "clock");
    if (!dc.surf) {
        wl_display_disconnect(dc.conn);
        thread_exit_current();
        return;
    }
    dc.render = clock_render;
    dc.frame_interval = 20;
    wm_manage(dc.surf);
    client_main(&dc);
}

static void sysmon_render(struct demo_client *dc) {
    struct gfx_canvas cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, GFX_RGB(18, 20, 26));

    font_draw(&cv, 8, 8, "sysmon - compositor stats", UI_FONT_PX, TH_TEXT);

    int nsurf = 0;
    comp_surfaces(&nsurf);
    char line[64];
    char num[12];

    line[0] = 0;
    strcat(line, "surfaces: ");
    u32_to_dec((uint32_t)nsurf, num);
    strcat(line, num);
    font_draw(&cv, 8, 30, line, UI_FONT_PX, GFX_RGB(170, 200, 230));

    line[0] = 0;
    strcat(line, "workspace: ");
    u32_to_dec((uint32_t)(wm_current_ws() + 1), num);
    strcat(line, num);
    strcat(line, " / 4");
    font_draw(&cv, 8, 48, line, UI_FONT_PX, GFX_RGB(170, 200, 230));

    line[0] = 0;
    strcat(line, "tick: ");
    u32_to_dec(tick, num);
    strcat(line, num);
    font_draw(&cv, 8, 66, line, UI_FONT_PX, GFX_RGB(170, 200, 230));

    int gx = 8, gy = 90, gw = dc->w - 16, gh = dc->h - 104;
    if (gw > 8 && gh > 8) {
        gfx_rect(&cv, gx, gy, gw, gh, GFX_RGB(70, 78, 92));
        int bars = (gw - 4) / 6;
        for (int i = 0; i < bars; i++) {
            uint32_t v = (tick / 4 + (uint32_t)i * 7) % 40;
            int bh = (int)(v * (uint32_t)(gh - 6) / 40);
            gfx_color col = GFX_RGB(40 + (int)(v % 4) * 30,
                                    120 + (int)(v % 4) * 30, 220);
            gfx_fill(&cv, gx + 2 + i * 6, gy + gh - 2 - bh, 4, bh, col);
        }
    }
}

static void sysmon_thread(void *arg) {
    (void)arg;
    struct demo_client dc;
    memset(&dc, 0, sizeof(dc));
    dc.conn = wl_display_connect("sysmon");
    if (!dc.conn) {
        thread_exit_current();
        return;
    }
    dc.surf = wl_compositor_create_surface(dc.conn, "sysmon");
    if (!dc.surf) {
        wl_display_disconnect(dc.conn);
        thread_exit_current();
        return;
    }
    dc.render = sysmon_render;
    dc.frame_interval = 15;
    wm_manage(dc.surf);
    client_main(&dc);
}

static void plasma_render(struct demo_client *dc) {
    struct gfx_canvas cv;
    canvas_of(&cv, dc);
    gfx_color *buf = cv.pixels;
    int phase = (int)(tick / 3);
    for (int y = 0; y < dc->h; y += 2) {
        gfx_color *row0 = buf + (size_t)y * (size_t)dc->w;
        gfx_color *row1 = (y + 1 < dc->h)
                              ? buf + (size_t)(y + 1) * (size_t)dc->w
                              : 0;
        for (int x = 0; x < dc->w; x += 2) {
            int rr = ((x + phase) / 6) & 0xFF;
            int gg = ((y + phase) / 5) & 0xFF;
            int bb = ((x + y + phase) / 4) & 0xFF;
            gfx_color col = GFX_RGB(rr, gg, bb);
            row0[x] = col;
            if (x + 1 < dc->w)
                row0[x + 1] = col;
            if (row1) {
                row1[x] = col;
                if (x + 1 < dc->w)
                    row1[x + 1] = col;
            }
        }
    }
    font_draw(&cv, 8, 8, "plasma - shm client rendering", UI_FONT_PX,
              GFX_RGB(255, 255, 255));
}

static void plasma_thread(void *arg) {
    (void)arg;
    struct demo_client dc;
    memset(&dc, 0, sizeof(dc));
    dc.conn = wl_display_connect("plasma");
    if (!dc.conn) {
        thread_exit_current();
        return;
    }
    dc.surf = wl_compositor_create_surface(dc.conn, "plasma");
    if (!dc.surf) {
        wl_display_disconnect(dc.conn);
        thread_exit_current();
        return;
    }
    dc.render = plasma_render;
    dc.frame_interval = 8;
    wm_manage(dc.surf);
    client_main(&dc);
}

typedef void (*client_thread_fn)(void *);

static client_thread_fn types[] = {term_thread, clock_thread, sysmon_thread,
                                   plasma_thread};
static const char *type_names[] = {"gc_term", "gc_clock", "gc_sysmon",
                                   "gc_plasma"};
static int next_type = 0;

void clients_spawn_next(void) {
    int type_idx = next_type % 4;
    next_type++;
    kernel_thread((char *)type_names[type_idx], 6, types[type_idx], 0);
}

void clients_spawn_initial(void) {
    next_type = 0;
    clients_spawn_next();
    clients_spawn_next();
    clients_spawn_next();
}

void clients_broadcast_close(void) {
    int n = 0;
    struct wl_surface **list = comp_surfaces(&n);
    for (int i = 0; i < n; i++)
        comp_send_close(list[i]);
}
