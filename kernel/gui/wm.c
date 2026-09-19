
#include "kernel/gui/wm.h"

#include "arch/x86/interrupt/interrupt.h"
#include "kernel/gui/font.h"
#include "kernel/init/pit/pit.h"
#include "lib/str/str.h"
#include "drivers/char/console/io.h"
#include "drivers/char/rtc.h"

extern void clients_spawn_next(void);
extern void clients_broadcast_close(void);

#define SC_ENTER 0x1C
#define SC_TAB 0x0F
#define SC_Q 0x10
#define SC_A 0x1E
#define SC_M 0x32
#define SC_T 0x14
#define SC_E 0x12
#define SC_1 0x02
#define SC_UP 0x48
#define SC_DOWN 0x50
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D

enum WIN_HIT {
    WIN_HIT_NONE = 0,
    WIN_HIT_TITLE,
    WIN_HIT_CLOSE,
    WIN_HIT_EDGE_T,
    WIN_HIT_EDGE_B,
    WIN_HIT_EDGE_L,
    WIN_HIT_EDGE_R,
    WIN_HIT_EDGE_TL,
    WIN_HIT_EDGE_TR,
    WIN_HIT_EDGE_BL,
    WIN_HIT_EDGE_BR,
    WIN_HIT_CONTENT,
};

struct GUI_WINREC {
    struct WL_SURFACE *s;
    int save_x, save_y, save_w, save_h;
};

struct GUI_WORKSPACE {
    struct GUI_WINREC win[WL_MAX_SURFACES];
    int n;
    int focus;
};

static struct GUI_WORKSPACE workspaces[WL_MAX_WS];
static int cur_ws = 0;
static int bar_dirty = 1;
static uint32_t bar_clock = 0;

static struct WL_SURFACE *grab;
static enum WIN_HIT grab_kind;
static int grab_dx, grab_dy;
static uint8_t cur_mods;
static enum WIN_HIT snap_pending;
static uint32_t last_click_tick;
static int last_click_x, last_click_y;

#define DBL_CLICK_TICKS (PIT_HZ * 4 / 10)
#define SNAP_MARGIN 14
#define BAR_FONT_PX 12
#define BAR_PILL_W 24
#define TASKBAR_H (COMP_BAR_H - 10)
#define TASKBAR_BTN_MIN 78
#define TASKBAR_BTN_MAXW 170
#define TASKBAR_TOP (comp_screen_h() - COMP_BAR_H)

static const int alpha_levels[] = {255, 216, 176, 136};
static int alpha_idx = 0;

static void bar_invalidate(void) {
    bar_dirty = 1;
}

static int ws_index_of(struct GUI_WORKSPACE *ws, struct WL_SURFACE *s) {
    if ((unsigned long)ws < 0xC0000000ul ||
        (unsigned long)ws >= 0xC2000000ul || ws->n < 0 ||
        ws->n > WL_MAX_SURFACES)
        return -1;
    for (int i = 0; i < ws->n; i++)
        if (ws->win[i].s == s)
            return i;
    return -1;
}

void wm_init_state(void) {
    for (int i = 0; i < WL_MAX_WS; i++) {
        workspaces[i].n = 0;
        workspaces[i].focus = -1;
    }
    cur_ws = 0;
    grab = 0;
    grab_kind = WIN_HIT_NONE;
    alpha_idx = 0;
    bar_dirty = 1;
    bar_clock = 0;
}

int wm_current_ws(void) {
    return cur_ws;
}

struct WL_SURFACE *wm_focused_surface(void) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    if (ws->focus < 0 || ws->focus >= ws->n)
        return 0;
    return ws->win[ws->focus].s;
}

static void frame_of(struct WL_SURFACE *s, int *fx, int *fy, int *fw, int *fh) {
    *fx = s->x - COMP_BORDER;
    *fy = s->y - COMP_TITLE_H;
    *fw = s->w + 2 * COMP_BORDER;
    *fh = s->h + COMP_TITLE_H + COMP_BORDER;
}

static void clamp_geom(struct WL_SURFACE *s) {
    if (s->w < 80)
        s->w = 80;
    if (s->h < 60)
        s->h = 60;
    int top = COMP_TITLE_H + 2;
    int bottom = TASKBAR_TOP;
    if (s->x < -s->w + 48)
        s->x = -s->w + 48;
    if (s->x > comp_screen_w() - 48)
        s->x = comp_screen_w() - 48;
    if (s->y < top)
        s->y = top;
    if (s->y + s->h > bottom) {
        s->h = bottom - s->y;
        if (s->h < 60) {
            s->h = 60;
            s->y = bottom - 60;
        }
    }
    if (s->y < top)
        s->y = top;
    if (s->y > bottom - 32)
        s->y = bottom - 32;
}

static void apply_geom(struct WL_SURFACE *s, int x, int y, int w, int h) {
    comp_damage_surface(s);
    if (s->x != x || s->y != y) {
        s->x = x;
        s->y = y;
    }
    if (s->w != w || s->h != h) {
        s->w = w;
        s->h = h;
        comp_send_configure(s, w, h);
    }
    clamp_geom(s);
    comp_damage_surface(s);
    bar_invalidate();
}

static void raise_and_focus(struct GUI_WORKSPACE *ws, int idx) {
    if (idx < 0 || idx >= ws->n)
        return;
    if (idx != ws->n - 1) {
        struct GUI_WINREC t = ws->win[idx];
        for (int i = idx; i < ws->n - 1; i++)
            ws->win[i] = ws->win[i + 1];
        ws->win[ws->n - 1] = t;
        idx = ws->n - 1;
        comp_damage_surface(t.s);
    }
    if (ws->focus != idx) {
        struct WL_SURFACE *old = wm_focused_surface();
        comp_surface_invalidate(old);
        ws->focus = idx;
        comp_surface_invalidate(ws->win[idx].s);
        if (old)
            comp_damage_surface(old);
        comp_damage_surface(ws->win[idx].s);
        bar_invalidate();
    }
}

struct WIN_ANIM {
    struct WL_SURFACE *s;
    int target;
};
static struct WIN_ANIM anims[WL_MAX_SURFACES];
static int anim_n;

static void anim_start(struct WL_SURFACE *s, int target) {
    for (int i = 0; i < anim_n; i++)
        if (anims[i].s == s)
            return;
    if (anim_n >= WL_MAX_SURFACES)
        return;
    wl_surface_set_alpha(s, 0);
    anims[anim_n].s = s;
    anims[anim_n].target = target;
    anim_n++;
}

int wm_anim_step(void) {
    int progressed = 0;
    for (int i = anim_n - 1; i >= 0; i--) {
        struct WL_SURFACE *s = anims[i].s;
        if (!s || !s->used) {
            anims[i] = anims[--anim_n];
            continue;
        }
        int a = s->alpha + 48;
        if (a > anims[i].target)
            a = anims[i].target;
        wl_surface_set_alpha(s, a);
        progressed = 1;
        if (a == anims[i].target)
            anims[i] = anims[--anim_n];
    }
    return progressed;
}

static struct WL_SURFACE *hit_test(int x, int y, enum WIN_HIT *kind);
static int taskbar_pills_x(void);
static int taskbar_layout(int *xs, int *bws, int max);

static struct WL_SURFACE *hover_s;
static int hover_close;
static int hover_btn = -1;
static int hover_pill = -1;

int wm_hover_close(struct WL_SURFACE *s) {
    return s != 0 && s == hover_s && hover_close;
}

void wm_handle_hover(int x, int y) {
    struct WL_SURFACE *ns = 0;
    int nclose = 0;
    int nbtn = -1;
    int npill = -1;
    if (y < TASKBAR_TOP) {
        enum WIN_HIT kind = WIN_HIT_NONE;
        ns = hit_test(x, y, &kind);
        if (ns) {
            int fx;
            int fy;
            int fw;
            int fh;
            frame_of(ns, &fx, &fy, &fw, &fh);
            nclose = (y < fy + COMP_TITLE_H + COMP_BORDER &&
                      x >= fx + fw - 20 && x < fx + fw - COMP_BORDER);
        }
    } else {
        int px = taskbar_pills_x();
        for (int i = 0; i < WL_MAX_WS; i++) {
            int bx = px + i * (BAR_PILL_W + 4);
            if (x >= bx && x < bx + BAR_PILL_W)
                npill = i;
        }
        int xs[WL_MAX_SURFACES];
        int bws[WL_MAX_SURFACES];
        int n = taskbar_layout(xs, bws, WL_MAX_SURFACES);
        for (int i = 0; i < n; i++)
            if (x >= xs[i] && x < xs[i] + bws[i])
                nbtn = i;
    }
    if (ns != hover_s || nclose != hover_close) {
        if (hover_s && hover_s->used) {
            int fx, fy, fw, fh;
            frame_of(hover_s, &fx, &fy, &fw, &fh);
            comp_surface_invalidate(hover_s);
            comp_damage_rect(fx, fy, fw, COMP_TITLE_H + COMP_BORDER + 1);
        }
        hover_s = ns;
        hover_close = nclose;
        if (hover_s && hover_s->used) {
            int fx, fy, fw, fh;
            frame_of(hover_s, &fx, &fy, &fw, &fh);
            comp_surface_invalidate(hover_s);
            comp_damage_rect(fx, fy, fw, COMP_TITLE_H + COMP_BORDER + 1);
        }
    }
    if (nbtn != hover_btn || npill != hover_pill) {
        hover_btn = nbtn;
        hover_pill = npill;
        bar_invalidate();
    }
}

void wm_manage(struct WL_SURFACE *s) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    if (ws->n >= WL_MAX_SURFACES)
        return;
    s->ws = cur_ws;
    s->alpha = (uint8_t)alpha_levels[alpha_idx];
    anim_start(s, alpha_levels[alpha_idx]);

    int cascade = ws->n % 6;
    s->w = 480;
    s->h = 320;
    s->x = 60 + cascade * 28;
    if (s->x + s->w > comp_screen_w())
        s->x = comp_screen_w() - s->w - 8;
    s->y = COMP_TITLE_H + 14 + cascade * 24;
    if (s->y + s->h > TASKBAR_TOP - 6)
        s->y = TASKBAR_TOP - s->h - 6;

    ws->win[ws->n].s = s;
    ws->win[ws->n].save_x = 0;
    ws->win[ws->n].save_y = 0;
    ws->win[ws->n].save_w = 0;
    ws->win[ws->n].save_h = 0;
    ws->n++;
    comp_send_configure(s, s->w, s->h);
    comp_log("wm: window stacked");
    raise_and_focus(ws, ws->n - 1);
}

void wm_unmanage(struct WL_SURFACE *s) {
    if (!s || (unsigned long)s < 0xC0000000ul ||
        (unsigned long)s >= 0xC2000000ul || !s->used || s->ws < 0 ||
        s->ws >= WL_MAX_WS)
        return;
    struct GUI_WORKSPACE *ws = &workspaces[s->ws];
    int idx = ws_index_of(ws, s);
    if (idx < 0)
        return;
    comp_damage_surface(s);
    if (grab == s) {
        grab = 0;
        grab_kind = WIN_HIT_NONE;
    }
    for (int i = idx; i < ws->n - 1; i++)
        ws->win[i] = ws->win[i + 1];
    ws->n--;
    if (ws->n == 0)
        ws->focus = -1;
    else if (ws->focus >= ws->n)
        ws->focus = ws->n - 1;
    else if (idx < ws->focus)
        ws->focus--;
    comp_log("wm: window closed");
    if (s->ws == cur_ws)
        bar_invalidate();
}

static void focus_index(struct GUI_WORKSPACE *ws, int idx) {
    if (ws->n == 0)
        return;
    if (idx < 0)
        idx = ws->n - 1;
    if (idx >= ws->n)
        idx = 0;
    raise_and_focus(ws, idx);
}

static void ws_switch(int target) {
    if (target == cur_ws || target < 0 || target >= WL_MAX_WS)
        return;
    struct GUI_WORKSPACE *old = &workspaces[cur_ws];
    for (int i = 0; i < old->n; i++)
        comp_damage_surface(old->win[i].s);
    cur_ws = target;
    comp_damage_rect(0, 0, comp_screen_w(), comp_screen_h());
    comp_log("wm: switch workspace");
    bar_invalidate();
}

static void move_focused_to(int target) {
    if (target == cur_ws || target < 0 || target >= WL_MAX_WS)
        return;
    struct GUI_WORKSPACE *cur = &workspaces[cur_ws];
    struct WL_SURFACE *s = wm_focused_surface();
    if (!s)
        return;
    struct GUI_WORKSPACE *dst = &workspaces[target];
    if (dst->n >= WL_MAX_SURFACES)
        return;
    int idx = ws_index_of(cur, s);
    if (idx < 0)
        return;
    comp_damage_surface(s);
    dst->win[dst->n].s = s;
    dst->win[dst->n].save_x = 0;
    dst->win[dst->n].save_y = 0;
    dst->win[dst->n].save_w = 0;
    dst->win[dst->n].save_h = 0;
    dst->n++;
    for (int i = idx; i < cur->n - 1; i++)
        cur->win[i] = cur->win[i + 1];
    cur->n--;
    if (cur->n == 0)
        cur->focus = -1;
    else if (cur->focus >= cur->n)
        cur->focus = cur->n - 1;
    s->ws = target;
    comp_log("wm: window moved to workspace");
    bar_invalidate();
}

static void toggle_maximize(void) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    struct WL_SURFACE *s = wm_focused_surface();
    if (!s)
        return;
    int idx = ws_index_of(ws, s);
    if (idx < 0)
        return;
    struct GUI_WINREC *r = &ws->win[idx];
    if (r->save_w > 0) {
        apply_geom(s, r->save_x, r->save_y, r->save_w, r->save_h);
        r->save_w = 0;
        comp_log("wm: window restored");
    } else {
        r->save_x = s->x;
        r->save_y = s->y;
        r->save_w = s->w;
        r->save_h = s->h;
        apply_geom(s, 8, COMP_TITLE_H + 2, comp_screen_w() - 16,
                   TASKBAR_TOP - COMP_TITLE_H - 6);
        comp_log("wm: window maximized");
    }
}

static void cycle_alpha(void) {
    alpha_idx = (alpha_idx + 1) % (int)(sizeof(alpha_levels) / sizeof(int));
    struct WL_SURFACE *s = wm_focused_surface();
    if (s)
        wl_surface_set_alpha(s, alpha_levels[alpha_idx]);
    bar_invalidate();
}

static void nudge_focused(int dx, int dy) {
    struct WL_SURFACE *s = wm_focused_surface();
    if (!s)
        return;
    apply_geom(s, s->x + dx, s->y + dy, s->w, s->h);
}

static int tab_sel = -1;

static void tab_commit(void) {
    if (tab_sel < 0)
        return;
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    int n = ws->n;
    if (n > 0) {
        int idx = n - 1 - tab_sel;
        if (idx < 0)
            idx = 0;
        if (idx >= n)
            idx = n - 1;
        kprintf("wm: tab focus -> %s\n", ws->win[idx].s->title);
        raise_and_focus(ws, idx);
    }
    tab_sel = -1;
    comp_damage_rect(0, 0, comp_screen_w(), comp_screen_h());
}

static void tab_advance(void) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    int n = ws->n;
    if (n == 0)
        return;
    if (n == 1) {
        focus_index(ws, 0);
        return;
    }
    if (tab_sel < 0) {
        int idx = ws->focus;
        if (idx < 0 || idx >= n)
            idx = n - 1;
        tab_sel = (n - 1 - idx + 1) % n;
    } else {
        tab_sel = (tab_sel + 1) % n;
    }
    comp_damage_rect(0, 0, comp_screen_w(), comp_screen_h());
}

void wm_draw_overlay(struct GFX_CANVAS *c, struct GFX_RECT *clip) {
    if (tab_sel < 0)
        return;
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    int n = ws->n;
    if (n == 0) {
        tab_sel = -1;
        return;
    }
    if (tab_sel >= n)
        tab_sel = n - 1;
    const struct GUI_THEME *t = theme();
    int row_h = 24;
    int box_w = 380;
    int box_h = 14 + n * row_h + 8;
    int bx = (comp_screen_w() - box_w) / 2;
    int by = (comp_screen_h() - box_h) / 3;
    struct GFX_RECT box;
    struct GFX_RECT v;
    box.x = bx;
    box.y = by;
    box.w = box_w;
    box.h = box_h;
    if (!gfx_rect_intersect(box, *clip, &v))
        return;
    gfx_fill_round(c, bx, by, box_w, box_h, 10, t->bar_line);
    gfx_fill_round(c, bx + 2, by + 2, box_w - 4, box_h - 4, 8, t->content);
    for (int k = 0; k < n; k++) {
        int idx = n - 1 - k;
        struct WL_SURFACE *s = ws->win[idx].s;
        int ry = by + 8 + k * row_h;
        if (k == tab_sel)
            gfx_fill_round(c, bx + 6, ry, box_w - 12, row_h - 2, 6,
                           t->accent);
        gfx_color fg = (k == tab_sel) ? t->title_fg_foc : t->text;
        int tyy = ry + (row_h - 2 - font_ascent(BAR_FONT_PX)) / 2 + 1;
        font_draw_clip(c, bx + 16, tyy, s->title, BAR_FONT_PX, fg, &v);
    }
}

void wm_handle_key(uint8_t scancode, int pressed, uint8_t mods) {
    if (!pressed) {
        if (tab_sel >= 0)
            kprintf("wm: tab release sc=%02x sel=%d\n", scancode, tab_sel);
        if (scancode == 0x38 && tab_sel >= 0)
            tab_commit();
        return;
    }
    if (scancode == 0x0F)
        kprintf("wm: tab press mods=%x\n", mods);
    cur_mods = mods;

    if (!(mods & (MOD_ALT | MOD_CTRL))) {
        struct WL_SURFACE *f = wm_focused_surface();
        if (f)
            comp_send_key(f, scancode, pressed, mods);
        return;
    }

    int shift = mods & MOD_SHIFT;

    if (scancode >= SC_1 && scancode < SC_1 + WL_MAX_WS) {
        int target = scancode - SC_1;
        if (shift)
            move_focused_to(target);
        else
            ws_switch(target);
        return;
    }

    switch (scancode) {
    case SC_ENTER:
        clients_spawn_next();
        break;
    case SC_TAB:
        tab_advance();
        break;
    case SC_Q: {
        struct WL_SURFACE *f = wm_focused_surface();
        if (f)
            comp_send_close(f);
        break;
    }
    case SC_A:
        cycle_alpha();
        break;
    case SC_M:
        toggle_maximize();
        break;
    case SC_T:
        comp_log(theme()->name);
        theme_toggle();
        comp_invalidate_all();
        comp_damage_rect(0, 0, comp_screen_w(), comp_screen_h());
        bar_invalidate();
        break;
    case SC_LEFT:
        nudge_focused(shift ? -1 : -16, 0);
        break;
    case SC_RIGHT:
        nudge_focused(shift ? 1 : 16, 0);
        break;
    case SC_UP:
        nudge_focused(0, shift ? -1 : -16);
        break;
    case SC_DOWN:
        nudge_focused(0, shift ? 1 : 16);
        break;
    case SC_E:
        if (shift) {
            comp_log("wm: session exit requested");
            clients_broadcast_close();
            comp_request_exit();
        }
        break;
    default:
        if ((mods & MOD_CTRL) && !(mods & MOD_ALT)) {
            struct WL_SURFACE *f = wm_focused_surface();
            kprintf("wm: fwd sc=%02x\n", scancode);
            if (f)
                comp_send_key(f, scancode, pressed, mods);
        }
        break;
    }
}

static struct WL_SURFACE *hit_test(int x, int y, enum WIN_HIT *kind) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    if (y >= TASKBAR_TOP)
        return 0;
    for (int i = ws->n - 1; i >= 0; i--) {
        struct WL_SURFACE *s = ws->win[i].s;
        int fx, fy, fw, fh;
        frame_of(s, &fx, &fy, &fw, &fh);
        if (x < fx || x >= fx + fw || y < fy || y >= fy + fh)
            continue;
        if (kind)
            *kind = WIN_HIT_CONTENT;
        int near_t = (y < fy + WIN_RESIZE_GRAB);
        int near_b = (y >= fy + fh - WIN_RESIZE_GRAB);
        int near_l = (x < fx + WIN_RESIZE_GRAB);
        int near_r = (x >= fx + fw - WIN_RESIZE_GRAB);
        if (near_t || near_b || near_l || near_r) {
            int is_close = (!near_t && y < fy + COMP_TITLE_H + COMP_BORDER &&
                            x >= fx + fw - 20);
            if (is_close) {
                if (kind)
                    *kind = WIN_HIT_CLOSE;
                return s;
            }
            if (kind) {
                if (near_t && near_l)
                    *kind = WIN_HIT_EDGE_TL;
                else if (near_t && near_r)
                    *kind = WIN_HIT_EDGE_TR;
                else if (near_b && near_l)
                    *kind = WIN_HIT_EDGE_BL;
                else if (near_b && near_r)
                    *kind = WIN_HIT_EDGE_BR;
                else if (near_t)
                    *kind = WIN_HIT_EDGE_T;
                else if (near_b)
                    *kind = WIN_HIT_EDGE_B;
                else if (near_l)
                    *kind = WIN_HIT_EDGE_L;
                else
                    *kind = WIN_HIT_EDGE_R;
            }
            return s;
        }
        if (y < fy + COMP_TITLE_H + COMP_BORDER) {
            if (x >= fx + fw - 20 && x < fx + fw - COMP_BORDER) {
                if (kind)
                    *kind = WIN_HIT_CLOSE;
            } else if (kind) {
                *kind = WIN_HIT_TITLE;
            }
        }
        return s;
    }
    return 0;
}

static int is_resize_kind(enum WIN_HIT k) {
    return (k >= WIN_HIT_EDGE_T && k <= WIN_HIT_EDGE_BR);
}

static void toggle_maximize(void);

static void apply_snap(struct WL_SURFACE *s) {
    int top = COMP_TITLE_H + 2;
    int hgt = TASKBAR_TOP - COMP_TITLE_H - 6;
    int half = comp_screen_w() / 2 - 12;
    if (snap_pending == WIN_HIT_EDGE_L) {
        apply_geom(s, 8, top, half, hgt);
        comp_log("wm: snap left half");
    } else if (snap_pending == WIN_HIT_EDGE_R) {
        apply_geom(s, comp_screen_w() / 2 + 4, top, half, hgt);
        comp_log("wm: snap right half");
    } else if (snap_pending == WIN_HIT_EDGE_T) {
        toggle_maximize();
    }
    snap_pending = WIN_HIT_NONE;
}

static int taskbar_btn_w(struct WL_SURFACE *s) {
    int w = font_text_width(s->title, BAR_FONT_PX) + 22;
    if (w < TASKBAR_BTN_MIN)
        w = TASKBAR_BTN_MIN;
    if (w > TASKBAR_BTN_MAXW)
        w = TASKBAR_BTN_MAXW;
    return w;
}

static int taskbar_pills_x(void) {
    return comp_screen_w() - 8 - (WL_MAX_WS * (BAR_PILL_W + 4) - 4);
}

static int taskbar_layout(int *xs, int *bws, int max) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    int limit = taskbar_pills_x() - 12 - font_text_width("00:00",
                                                         BAR_FONT_PX) - 8;
    int x = 8;
    int n = 0;
    for (int i = 0; i < ws->n && n < max; i++) {
        int bw = taskbar_btn_w(ws->win[i].s);
        if (x + bw > limit)
            break;
        xs[n] = x;
        bws[n] = bw;
        x += bw + 4;
        n++;
    }
    return n;
}

static void taskbar_click(int x, int y) {
    if (y < TASKBAR_TOP + 1 || y >= comp_screen_h() - 1)
        return;
    int px = taskbar_pills_x();
    for (int i = 0; i < WL_MAX_WS; i++) {
        int bx = px + i * (BAR_PILL_W + 4);
        if (x >= bx && x < bx + BAR_PILL_W) {
            ws_switch(i);
            return;
        }
    }
    int xs[WL_MAX_SURFACES], bws[WL_MAX_SURFACES];
    int n = taskbar_layout(xs, bws, WL_MAX_SURFACES);
    for (int i = 0; i < n; i++) {
        if (x >= xs[i] && x < xs[i] + bws[i]) {
            raise_and_focus(&workspaces[cur_ws], i);
            return;
        }
    }
}

void wm_handle_button(int x, int y, uint8_t buttons, uint8_t edge) {
    if (edge & 2) {
        if (grab && snap_pending != WIN_HIT_NONE)
            apply_snap(grab);
        grab = 0;
        grab_kind = WIN_HIT_NONE;
        snap_pending = WIN_HIT_NONE;
        return;
    }
    if (!(edge & 1))
        return;
    if (!(buttons & 1))
        return;
    if (y >= TASKBAR_TOP) {
        taskbar_click(x, y);
        return;
    }
    enum WIN_HIT kind = WIN_HIT_NONE;
    struct WL_SURFACE *s = hit_test(x, y, &kind);
    if (!s)
        return;
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    raise_and_focus(ws, ws_index_of(ws, s));
    if (kind == WIN_HIT_CLOSE) {
        comp_send_close(s);
        return;
    }
    snap_pending = WIN_HIT_NONE;

    if (kind == WIN_HIT_CONTENT && (cur_mods & (MOD_ALT | MOD_CTRL)))
        kind = WIN_HIT_TITLE;

    if (kind == WIN_HIT_TITLE) {
        int dbl = (tick - last_click_tick <= DBL_CLICK_TICKS) &&
                  (x - last_click_x) < 6 && (x - last_click_x) > -6 &&
                  (y - last_click_y) < 6 && (y - last_click_y) > -6;
        last_click_tick = tick;
        last_click_x = x;
        last_click_y = y;
        if (dbl) {
            toggle_maximize();
            return;
        }
    }
    if (kind == WIN_HIT_TITLE || is_resize_kind(kind)) {
        grab = s;
        grab_kind = kind;
        grab_dx = x - s->x;
        grab_dy = y - s->y;
    }
}

void wm_handle_motion(int x, int y) {
    if (!grab)
        return;
    struct WL_SURFACE *s = grab;
    if (grab_kind == WIN_HIT_TITLE) {
        apply_geom(s, x - grab_dx, y - grab_dy, s->w, s->h);
        if (x < SNAP_MARGIN)
            snap_pending = WIN_HIT_EDGE_L;
        else if (x >= comp_screen_w() - SNAP_MARGIN)
            snap_pending = WIN_HIT_EDGE_R;
        else if (y < SNAP_MARGIN)
            snap_pending = WIN_HIT_EDGE_T;
        else
            snap_pending = WIN_HIT_NONE;
        return;
    }
    int nx = s->x, ny = s->y, nw = s->w, nh = s->h;
    int right = x - grab_dx + WIN_RESIZE_GRAB;
    int bottom = y - grab_dy + WIN_RESIZE_GRAB;
    int left = x - grab_dx;
    int top = y - grab_dy;
    if (grab_kind == WIN_HIT_EDGE_R || grab_kind == WIN_HIT_EDGE_TR ||
        grab_kind == WIN_HIT_EDGE_BR) {
        nw = right;
    }
    if (grab_kind == WIN_HIT_EDGE_B || grab_kind == WIN_HIT_EDGE_BL ||
        grab_kind == WIN_HIT_EDGE_BR) {
        nh = bottom;
    }
    if (grab_kind == WIN_HIT_EDGE_L || grab_kind == WIN_HIT_EDGE_TL ||
        grab_kind == WIN_HIT_EDGE_BL) {
        int right_fixed = s->x + s->w;
        if (nx < right_fixed - 80) {
            int dx = left - s->x;
            nx = s->x + dx;
            nw = right_fixed - nx;
        }
    }
    if (grab_kind == WIN_HIT_EDGE_T || grab_kind == WIN_HIT_EDGE_TL ||
        grab_kind == WIN_HIT_EDGE_TR) {
        int bottom_fixed = s->y + s->h;
        int min_y = COMP_TITLE_H + 2;
        if (ny > min_y && top > min_y) {
            int dy = top - s->y;
            ny = s->y + dy;
            nh = bottom_fixed - ny;
        }
    }
    apply_geom(s, nx, ny, nw, nh);
}

struct WL_SURFACE *wm_surface_at(int x, int y) {
    enum WIN_HIT kind = WIN_HIT_NONE;
    return hit_test(x, y, &kind);
}

int wm_collect_visible(struct WL_SURFACE **out, int max) {
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];
    int n = ws->n < max ? ws->n : max;
    for (int i = 0; i < n; i++)
        out[i] = ws->win[i].s;
    return n;
}

int wm_bar_check_dirty(void) {
    if (tick / PIT_HZ != bar_clock) {
        bar_clock = tick / PIT_HZ;
        bar_dirty = 1;
    }
    int d = bar_dirty;
    bar_dirty = 0;
    return d;
}

void wm_draw_bar(struct GFX_CANVAS *c, struct GFX_RECT *clip) {
    int sw = comp_screen_w();
    struct GFX_RECT bar = {0, TASKBAR_TOP, sw, COMP_BAR_H}, v;
    if (!gfx_rect_intersect(bar, *clip, &v))
        return;
    const struct GUI_THEME *t = theme();

    gfx_fill(c, v.x, v.y, v.w, v.h, t->bar);
    gfx_hline(c, 0, TASKBAR_TOP, sw, t->bar_line);

    int by = TASKBAR_TOP + 5;
    int ty = by + (TASKBAR_H - font_ascent(BAR_FONT_PX)) / 2;
    struct WL_SURFACE *f = wm_focused_surface();
    struct GUI_WORKSPACE *ws = &workspaces[cur_ws];

    int xs[WL_MAX_SURFACES], bws[WL_MAX_SURFACES];
    int n = taskbar_layout(xs, bws, WL_MAX_SURFACES);
    for (int i = 0; i < n; i++) {
        struct WL_SURFACE *s = ws->win[i].s;
        int focused = (s == f);
        gfx_fill_round(c, xs[i], by, bws[i], TASKBAR_H, 6,
                       focused ? t->accent
                               : (i == hover_btn ? t->frame_foc
                                                 : t->frame_unf));
        struct GFX_RECT btn = {xs[i], by, bws[i], TASKBAR_H}, bv;
        if (!gfx_rect_intersect(btn, v, &bv))
            continue;
        font_draw_clip(c, xs[i] + 9, ty, s->title, BAR_FONT_PX,
                       focused ? t->title_fg_foc : t->text, &bv);
    }

    int px = taskbar_pills_x();
    for (int i = 0; i < WL_MAX_WS; i++) {
        int cur = (i == cur_ws);
        int occ = workspaces[i].n > 0;
        int bx = px + i * (BAR_PILL_W + 4);
        gfx_color pill = cur ? t->accent
                             : (i == hover_pill ? t->frame_foc
                                                : (occ ? t->frame_unf
                                                       : t->dim));
        gfx_fill_round(c, bx, by, BAR_PILL_W, TASKBAR_H, 6, pill);
        char label[4] = {' ', (char)('1' + i), ' ', 0};
        gfx_color fg = cur ? t->title_fg_foc : (occ ? t->text : t->muted);
        int lw = font_text_width(label, BAR_FONT_PX);
        font_draw_clip(c, bx + (BAR_PILL_W - lw) / 2, ty, label, BAR_FONT_PX,
                       fg, &v);
    }

    uint8_t hh;
    uint8_t mm;
    uint8_t ss;
    rtc_read_time(&hh, &mm, &ss);
    char up[8];
    up[0] = (char)('0' + hh / 10);
    up[1] = (char)('0' + hh % 10);
    up[2] = ':';
    up[3] = (char)('0' + mm / 10);
    up[4] = (char)('0' + mm % 10);
    up[5] = 0;
    int rw = font_text_width(up, BAR_FONT_PX);
    font_draw_clip(c, px - 12 - rw, ty, up, BAR_FONT_PX, t->muted, &v);
    (void)ss;
}
