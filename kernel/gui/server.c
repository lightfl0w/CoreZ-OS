#include "kernel/gui/server.h"

#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "kernel/init/pit/pit.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/gui/font.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/wm.h"
#include "kernel/gui/display.h"
#include "kernel/gui/input.h"

static struct wl_client clients[WL_MAX_CLIENTS];
static struct wl_surface surfaces[WL_MAX_SURFACES];

static struct gfx_canvas *dst;
static int scrnx, scrny;

static struct lock comp_lock;
static int session_active = 1;

#define MAX_DAMAGE 48
static struct gfx_rect damage[MAX_DAMAGE];
static int damage_n = 0;
static int damage_full = 0;

static int cur_x = 512, cur_y = 384;
static uint8_t last_buttons = 0;

#define CURSOR_W 12
#define CURSOR_H 18
static const char *cursor_bmp[CURSOR_H] = {
    "#...........", "##..........", "#O#.........", "#OO#........",
    "#OOO#.......", "#OOOO#......", "#OOOOO#.....", "#OOOOOO#....",
    "#OOOOOOO#...", "#OOOOOOOO#..", "#OOOOO#####.", "#OO#OO#.....",
    "#O#.#OO#....", "##..#OO#....", "#....#OO#...", ".....#OO#...",
    "......#OO#..", ".......##...",
};
#define CURSOR_OUTLINE GFX_RGB(0, 0, 0)
#define CURSOR_FILL GFX_RGB(255, 255, 255)
#define UI_FONT_PX 13
#define TITLE_FONT_PX 13
#define CLOSE_FONT_PX 10

static void client_post(struct wl_client *c, int type, int32_t a, int32_t b,
                        int32_t cc) {
    if (!c || !c->used)
        return;
    lock_acquire(&c->lock);
    int next = (c->qhead + 1) % WL_CLIENT_QUEUE;
    if (next != c->qtail) {
        c->queue[c->qhead].type = type;
        c->queue[c->qhead].a = a;
        c->queue[c->qhead].b = b;
        c->queue[c->qhead].c = cc;
        c->qhead = next;
        sema_up(&c->sema);
    }
    lock_release(&c->lock);
}

void (*log_hook)(const char *s) = 0;
void comp_log(const char *s) {
    if (log_hook)
        log_hook(s);
}

void comp_damage_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    struct gfx_rect r = {x, y, w, h}, scr = {0, 0, scrnx, scrny}, v;
    if (!gfx_rect_intersect(r, scr, &v))
        return;
    lock_acquire(&comp_lock);
    if (damage_full) {
        lock_release(&comp_lock);
        return;
    }

    for (int i = 0; i < damage_n; i++) {
        struct gfx_rect u;
        if (gfx_rect_intersect(v, damage[i], &u)) {
            int x0 = v.x < damage[i].x ? v.x : damage[i].x;
            int y0 = v.y < damage[i].y ? v.y : damage[i].y;
            int x1 = (v.x + v.w) > (damage[i].x + damage[i].w)
                         ? (v.x + v.w)
                         : (damage[i].x + damage[i].w);
            int y1 = (v.y + v.h) > (damage[i].y + damage[i].h)
                         ? (v.y + v.h)
                         : (damage[i].y + damage[i].h);
            damage[i].x = x0;
            damage[i].y = y0;
            damage[i].w = x1 - x0;
            damage[i].h = y1 - y0;
            lock_release(&comp_lock);
            return;
        }
    }
    if (damage_n >= MAX_DAMAGE) {
        damage_full = 1;
    } else {
        damage[damage_n++] = v;
    }
    lock_release(&comp_lock);
}

void comp_damage_surface(struct wl_surface *s) {
    if (!s || !s->used)
        return;
    int m = WIN_SHADOW + WIN_RADIUS + 2;
    comp_damage_rect(s->x - COMP_BORDER - m, s->y - COMP_TITLE_H - m,
                     s->w + 2 * COMP_BORDER + 2 * m,
                     s->h + COMP_TITLE_H + COMP_BORDER + 2 * m);
}

void comp_post_key(uint8_t scancode, int pressed, uint8_t mods) {
    input_post(INPUT_DEV_KEYBOARD, scancode,
               (int32_t)pressed | ((int32_t)mods << 8));
}

void comp_post_mouse(int dx, int dy, uint8_t buttons) {
    input_post(INPUT_DEV_POINTER, (uint32_t)buttons,
               (int32_t)(dx & 0xFFFF) | ((int32_t)(dy & 0xFFFF) << 16));
}

struct wl_client *wl_display_connect(const char *name) {
    lock_acquire(&comp_lock);
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (clients[i].used)
            continue;
        struct wl_client *c = &clients[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        strncpy(c->name, name, 15);
        c->name[15] = 0;
        sema_init(&c->sema, 0);
        lock_init(&c->lock);
        lock_release(&comp_lock);
        return c;
    }
    lock_release(&comp_lock);
    return 0;
}

void wl_display_disconnect(struct wl_client *c) {
    if (!c)
        return;
    lock_acquire(&comp_lock);
    c->used = 0;
    lock_release(&comp_lock);
}

struct wl_surface *wl_compositor_create_surface(struct wl_client *c,
                                                const char *title) {
    if (!c || !c->used)
        return 0;
    lock_acquire(&comp_lock);
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (surfaces[i].used)
            continue;
        struct wl_surface *s = &surfaces[i];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->client = c;
        s->ws = 0;
        strncpy(s->title, title, 23);
        s->title[23] = 0;
        c->surf = s;
        lock_release(&comp_lock);
        return s;
    }
    lock_release(&comp_lock);
    return 0;
}

int wl_surface_attach(struct wl_surface *s, struct shm_pool *pool, int w,
                      int h) {
    if (!s || !s->used || !pool || !pool->in_use)
        return -1;
    lock_acquire(&comp_lock);
    s->buf = pool->data;
    s->buf_w = w;
    s->buf_h = h;
    lock_release(&comp_lock);
    return 0;
}

void wl_surface_commit(struct wl_surface *s) {
    if (!s || !s->used || !s->buf)
        return;
    lock_acquire(&comp_lock);
    s->frame_pending = 1;
    lock_release(&comp_lock);
    comp_damage_surface(s);
}

void wl_surface_destroy(struct wl_surface *s) {
    if (!s || !s->used)
        return;
    lock_acquire(&comp_lock);
    if (s->client)
        s->client->surf = 0;
    s->used = 0;
    s->buf = 0;
    lock_release(&comp_lock);
}

int wl_display_dispatch(struct wl_client *c, struct wl_event *ev) {
    if (!c || !c->used)
        return -1;
    sema_down(&c->sema);
    lock_acquire(&c->lock);
    if (c->qtail == c->qhead) {
        lock_release(&c->lock);
        ev->type = WL_EV_NONE;
        return 0;
    }
    *ev = c->queue[c->qtail];
    c->qtail = (c->qtail + 1) % WL_CLIENT_QUEUE;
    lock_release(&c->lock);
    return 0;
}

struct wl_surface **comp_surfaces(int *count) {
    static struct wl_surface *list[WL_MAX_SURFACES];
    int n = 0;
    for (int i = 0; i < WL_MAX_SURFACES; i++)
        if (surfaces[i].used)
            list[n++] = &surfaces[i];
    *count = n;
    return list;
}

int comp_screen_w(void) {
    return scrnx;
}
int comp_screen_h(void) {
    return scrny;
}

void comp_send_configure(struct wl_surface *s, int w, int h) {
    client_post(s->client, WL_EV_CONFIGURE, w, h, 0);
}
void comp_send_close(struct wl_surface *s) {
    client_post(s->client, WL_EV_CLOSE, 0, 0, 0);
}
void comp_send_key(struct wl_surface *s, int scancode, int pressed, int mods) {
    client_post(s->client, WL_EV_KEY, scancode, pressed, mods);
}
void comp_request_exit(void) {
    session_active = 0;
}

void comp_destroy_surface_pool(struct wl_surface *s, struct shm_pool **pool) {
    if (!pool || !*pool)
        return;
    lock_acquire(&comp_lock);
    if (s) {
        s->buf = 0;
        s->buf_w = 0;
        s->buf_h = 0;
    }
    lock_release(&comp_lock);
    shm_pool_destroy(*pool);
    *pool = 0;
}

static gfx_color wallpaper_color(int y) {
    int t = (scrny > 1) ? (y * 255) / (scrny - 1) : 0;
    if (t < 0)
        t = 0;
    if (t > 255)
        t = 255;
    int r = GFX_R(TH_WP_TOP) + (GFX_R(TH_WP_BOT) - GFX_R(TH_WP_TOP)) * t / 255;
    int g = GFX_G(TH_WP_TOP) + (GFX_G(TH_WP_BOT) - GFX_G(TH_WP_TOP)) * t / 255;
    int b = GFX_B(TH_WP_TOP) + (GFX_B(TH_WP_BOT) - GFX_B(TH_WP_TOP)) * t / 255;
    return GFX_RGB(r, g, b);
}

static void draw_wallpaper(struct gfx_rect *r) {
    for (int y = r->y; y < r->y + r->h; y++)
        gfx_hline(dst, r->x, y, r->w, wallpaper_color(y));
}

static void draw_window(struct wl_surface *s, struct gfx_rect *clip) {
    int fx = s->x - COMP_BORDER;
    int fy = s->y - COMP_TITLE_H;
    int fw = s->w + 2 * COMP_BORDER;
    int fh = s->h + COMP_TITLE_H + COMP_BORDER;
    int rad = WIN_RADIUS;

    struct gfx_rect frame = {fx, fy, fw, fh}, v;
    if (!gfx_rect_intersect(frame, *clip, &v))
        return;

    int focused = (wm_focused_surface() == s);
    gfx_color border_col = focused ? TH_FRAME_FOC : TH_FRAME_UNF;
    gfx_color title_col = focused ? TH_TITLE_FOC : TH_TITLE_UNF;

    gfx_fill_round(dst, fx - WIN_SHADOW + 3, fy + 3, fw + 2 * WIN_SHADOW - 6,
                   fh + 2 * WIN_SHADOW - 2, rad + WIN_SHADOW, TH_SHADOW);
    gfx_fill_round(dst, fx - WIN_SHADOW + 5, fy + 5, fw + 2 * WIN_SHADOW - 10,
                   fh + 2 * WIN_SHADOW - 6, rad + WIN_SHADOW - 2, TH_SHADOW2);

    gfx_fill_round(dst, fx, fy, fw, fh, rad, border_col);
    gfx_fill_round(dst, fx + COMP_BORDER, fy + COMP_BORDER,
                   fw - 2 * COMP_BORDER, fh - 2 * COMP_BORDER,
                   rad - COMP_BORDER, title_col);

    int title_px = TITLE_FONT_PX;
    int ty = fy + (COMP_TITLE_H - font_ascent(title_px)) / 2;
    if (ty < fy)
        ty = fy;
    font_draw(dst, fx + 8, ty, s->title, title_px,
              focused ? TH_TEXT : TH_MUTED);

    int bs = 12, cbx = fx + fw - 14, cby = fy + 1;
    gfx_fill_round(dst, cbx, cby, bs, bs, 5, focused ? TH_CLOSE : TH_DIM);
    int cw = font_text_width("x", CLOSE_FONT_PX);
    font_draw(dst, cbx + (bs - cw) / 2,
              cby + (bs - font_ascent(CLOSE_FONT_PX)) / 2, "x", CLOSE_FONT_PX,
              TH_TEXT);

    if (s->w > 0 && s->h > 0) {
        struct gfx_rect content = {s->x, s->y, s->w, s->h};
        struct gfx_rect iv;
        if (gfx_rect_intersect(content, *clip, &iv)) {
            if (s->buf && s->buf_w == s->w && s->buf_h == s->h) {
                struct gfx_canvas sc;
                sc.pixels = (gfx_color *)s->buf;
                sc.pitch = s->w * 4;
                sc.w = s->w;
                sc.h = s->h;
                sc.bytes = (size_t)s->w * (size_t)s->h * 4u;
                gfx_blit_round(dst, iv.x, iv.y, &sc, iv.x - s->x, iv.y - s->y,
                               iv.w, iv.h, 255, fx, fy, fw, fh, rad,
                               GFX_CORNER_ALL);
            } else {
                gfx_fill(dst, iv.x, iv.y, iv.w, iv.h, TH_CONTENT);
            }
        }
    }
}

static void draw_cursor(void) {
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            char p = cursor_bmp[row][col];
            if (p == '.')
                continue;
            int px = cur_x + col, py = cur_y + row;
            if (px < 0 || px >= scrnx || py < 0 || py >= scrny)
                continue;
            gfx_color c = (p == 'O') ? CURSOR_FILL : CURSOR_OUTLINE;
            dst->pixels[(size_t)py * (size_t)gfx_stride(dst) + (size_t)px] = c;
        }
    }
}

static int rect_covered_by_window(struct gfx_rect *r, struct wl_surface **vis,
                                  int vn) {
    for (int j = 0; j < vn; j++) {
        struct wl_surface *s = vis[j];
        if (s->w <= 0 || s->h <= 0)
            continue;
        if (r->x >= s->x && r->y >= s->y && r->x + r->w <= s->x + s->w &&
            r->y + r->h <= s->y + s->h) {
            return 1;
        }
    }
    return 0;
}

static void repaint(void);
static void comp_present(struct gfx_rect *rects, int n);

static void repaint(void) {
    struct gfx_rect rects[MAX_DAMAGE + 1];
    int n = 0;
    lock_acquire(&comp_lock);
    if (damage_full) {
        rects[0].x = 0;
        rects[0].y = 0;
        rects[0].w = scrnx;
        rects[0].h = scrny;
        n = 1;
    } else {
        for (int i = 0; i < damage_n; i++)
            rects[i] = damage[i];
        n = damage_n;
    }
    damage_n = 0;
    damage_full = 0;

    struct wl_surface *vis[WL_MAX_SURFACES];
    int vn = wm_collect_visible(vis, WL_MAX_SURFACES);

    for (int i = 0; i < n; i++) {
        struct gfx_rect *r = &rects[i];
        if (!rect_covered_by_window(r, vis, vn))
            draw_wallpaper(r);
        for (int j = 0; j < vn; j++)
            draw_window(vis[j], r);
        wm_draw_bar(dst, r);
    }
    lock_release(&comp_lock);

    comp_present(rects, n);

    for (int i = 0; i < vn; i++) {
        struct wl_surface *s = vis[i];
        if (s->frame_pending) {
            s->frame_pending = 0;
            client_post(s->client, WL_EV_FRAME, 0, 0, 0);
        }
    }
}

static void comp_present(struct gfx_rect *rects, int n) {
    struct display_ops *d = display_get();
    if (d == 0)
        return;
    draw_cursor();
    d->flip(rects, n);
}

void comp_init(void) {
    display_init();
    struct display_ops *d = display_get();
    d->init();

    scrnx = io_get_scrnx();
    scrny = io_get_scrny();
    dst = d->surface(DISP_BACK);

    struct gfx_canvas *fc = d->surface(DISP_FRONT);
    (void)fc;

    input_init();
    lock_init(&comp_lock);
    shm_init();
    memset(clients, 0, sizeof(clients));
    memset(surfaces, 0, sizeof(surfaces));
    damage_n = 0;
    damage_full = 1;
    cur_x = scrnx / 2;
    cur_y = scrny / 2;
    last_buttons = 0;
    session_active = 1;
}

static void drain_input(void) {
    struct input_event ev;
    while (input_get(&ev)) {
        if (ev.dev == INPUT_DEV_KEYBOARD) {
            wm_handle_key((uint8_t)ev.code, (int)(ev.value & 0xFF),
                          (uint8_t)(ev.value >> 8));
        } else if (ev.dev == INPUT_DEV_POINTER) {
            int dx = (int16_t)(ev.value & 0xFFFF);
            int dy = (int16_t)((uint32_t)ev.value >> 16);
            int nx = cur_x + dx;
            int ny = cur_y + dy;
            if (nx < 0)
                nx = 0;
            if (nx >= scrnx)
                nx = scrnx - 1;
            if (ny < 0)
                ny = 0;
            if (ny >= scrny)
                ny = scrny - 1;
            if (nx != cur_x || ny != cur_y) {
                comp_damage_rect(cur_x, cur_y, CURSOR_W, CURSOR_H);
                comp_damage_rect(nx, ny, CURSOR_W, CURSOR_H);
                wm_handle_motion(nx, ny);
                cur_x = nx;
                cur_y = ny;
            }
            uint8_t btn = (uint8_t)ev.code;
            uint8_t edge = btn ^ last_buttons;
            if (edge) {
                wm_handle_button(cur_x, cur_y, btn, edge);
                last_buttons = btn;
            }
        }
    }
}

void comp_run(void) {
    struct display_ops *d = display_get();
    while (session_active) {
        drain_input();
        if (wm_bar_check_dirty()) {
            comp_damage_rect(0, 0, scrnx, COMP_BAR_H);
        }
        if (damage_full || damage_n > 0) {
            repaint();
        }
        if (d && d->wait_vblank)
            d->wait_vblank();
        else
            mtime_sleep(20);
    }
}
