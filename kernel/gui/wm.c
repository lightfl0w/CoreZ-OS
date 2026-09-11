#include "kernel/gui/wm.h"

#include "arch/x86/interrupt/interrupt.h"
#include "kernel/init/pit/pit.h"
#include "lib/str/str.h"
#include "kernel/gui/font.h"
#include "kernel/gui/layout.h"

extern void clients_spawn_next(void);
extern void clients_broadcast_close(void);

#define SC_ENTER 0x1C
#define SC_J 0x24
#define SC_K 0x25
#define SC_H 0x23
#define SC_L 0x26
#define SC_SPACE 0x39
#define SC_F 0x21
#define SC_Q 0x10
#define SC_E 0x12
#define SC_1 0x02

struct workspace {
    struct wl_surface *order[WL_MAX_SURFACES];
    int n;
    int tcount;
    int focus;
    int mfact;
    enum layout_kind layout;
};

static struct workspace workspaces[WL_MAX_WS];
static int cur_ws = 0;
static int bar_dirty = 1;
static uint32_t bar_clock = 0;

static struct wl_surface *drag = 0;
static int drag_dx = 0, drag_dy = 0;

static void bar_invalidate(void) {
    bar_dirty = 1;
}

static int ws_index_of(struct workspace *ws, struct wl_surface *s) {
    for (int i = 0; i < ws->n; i++)
        if (ws->order[i] == s)
            return i;
    return -1;
}

void wm_init_state(void) {
    for (int i = 0; i < WL_MAX_WS; i++) {
        workspaces[i].n = 0;
        workspaces[i].tcount = 0;
        workspaces[i].focus = -1;
        workspaces[i].mfact = 550;
        workspaces[i].layout = LAYOUT_MASTER_STACK;
    }
    cur_ws = 0;
    drag = 0;
    bar_dirty = 1;
    bar_clock = 0;
}

int wm_current_ws(void) {
    return cur_ws;
}

struct wl_surface *wm_focused_surface(void) {
    struct workspace *ws = &workspaces[cur_ws];
    if (ws->focus < 0 || ws->focus >= ws->n)
        return 0;
    return ws->order[ws->focus];
}

static void arrange(void) {
    struct workspace *ws = &workspaces[cur_ws];
    struct gfx_rect area = {0, COMP_BAR_H, comp_screen_w(),
                            comp_screen_h() - COMP_BAR_H};

    int tn = ws->tcount;
    if (tn > 0) {
        struct gfx_rect rects[WL_MAX_SURFACES];
        struct layout_params p = {ws->layout, ws->mfact, COMP_GAP};
        struct wl_surface *tiled[WL_MAX_SURFACES];
        for (int i = 0; i < tn; i++)
            tiled[i] = ws->order[i];
        layout_arrange(&p, tn, area, rects);
        for (int i = 0; i < tn; i++) {
            struct wl_surface *s = tiled[i];
            if (s->x != rects[i].x || s->y != rects[i].y) {
                comp_damage_surface(s);
                s->x = rects[i].x;
                s->y = rects[i].y;
            }
            if (s->w != rects[i].w || s->h != rects[i].h) {
                comp_damage_surface(s);
                s->w = rects[i].w;
                s->h = rects[i].h;
                comp_send_configure(s, s->w, s->h);
            }
            comp_damage_surface(s);
        }
    }

    for (int i = tn; i < ws->n; i++)
        comp_damage_surface(ws->order[i]);
    bar_invalidate();
}

void wm_manage(struct wl_surface *s) {
    struct workspace *ws = &workspaces[cur_ws];
    if (ws->n >= WL_MAX_SURFACES)
        return;
    s->ws = cur_ws;
    s->floating = 0;

    for (int i = ws->n; i > ws->tcount; i--)
        ws->order[i] = ws->order[i - 1];
    ws->order[ws->tcount] = s;
    ws->n++;
    ws->tcount++;
    ws->focus = ws->tcount - 1;
    comp_log("wm: new window managed");
    arrange();
}

void wm_unmanage(struct wl_surface *s) {
    struct workspace *ws = &workspaces[s->ws];
    int idx = ws_index_of(ws, s);
    if (idx < 0)
        return;
    comp_damage_surface(s);
    if (drag == s)
        drag = 0;
    for (int i = idx; i < ws->n - 1; i++)
        ws->order[i] = ws->order[i + 1];
    ws->n--;
    if (idx < ws->tcount)
        ws->tcount--;
    if (ws->n == 0)
        ws->focus = -1;
    else if (ws->focus >= ws->n)
        ws->focus = ws->n - 1;
    else if (idx < ws->focus)
        ws->focus--;
    comp_log("wm: window closed");
    if (s->ws == cur_ws)
        arrange();
}

static void focus_index(int idx) {
    struct workspace *ws = &workspaces[cur_ws];
    if (ws->n == 0)
        return;
    if (idx < 0)
        idx = ws->n - 1;
    if (idx >= ws->n)
        idx = 0;
    if (idx == ws->focus)
        return;
    struct wl_surface *old = wm_focused_surface();
    if (old)
        comp_damage_surface(old);
    ws->focus = idx;
    comp_damage_surface(ws->order[idx]);
    bar_invalidate();
}

static void ws_switch(int target) {
    if (target == cur_ws || target < 0 || target >= WL_MAX_WS)
        return;

    struct workspace *old = &workspaces[cur_ws];
    for (int i = 0; i < old->n; i++)
        comp_damage_surface(old->order[i]);
    cur_ws = target;
    comp_damage_rect(0, 0, comp_screen_w(), comp_screen_h());
    comp_log("wm: switch workspace");
    arrange();
}

static void move_focused_to(int target) {
    if (target == cur_ws || target < 0 || target >= WL_MAX_WS)
        return;
    struct workspace *cur = &workspaces[cur_ws];
    struct wl_surface *s = wm_focused_surface();
    if (!s)
        return;
    struct workspace *dst = &workspaces[target];
    if (dst->n >= WL_MAX_SURFACES)
        return;

    int idx = ws_index_of(cur, s);
    int was_float = idx >= cur->tcount;
    comp_damage_surface(s);
    for (int i = idx; i < cur->n - 1; i++)
        cur->order[i] = cur->order[i + 1];
    cur->n--;
    if (!was_float)
        cur->tcount--;
    if (cur->n == 0)
        cur->focus = -1;
    else if (cur->focus >= cur->n)
        cur->focus = cur->n - 1;

    s->ws = target;
    if (was_float) {
        dst->order[dst->n++] = s;
    } else {
        for (int i = dst->n; i > dst->tcount; i--)
            dst->order[i] = dst->order[i - 1];
        dst->order[dst->tcount] = s;
        dst->n++;
        dst->tcount++;
    }
    comp_log("wm: window moved to workspace");
    arrange();
}

static void toggle_float(void) {
    struct workspace *ws = &workspaces[cur_ws];
    struct wl_surface *s = wm_focused_surface();
    if (!s)
        return;
    int idx = ws_index_of(ws, s);
    comp_damage_surface(s);
    if (idx < ws->tcount) {
        for (int i = idx; i < ws->n - 1; i++)
            ws->order[i] = ws->order[i + 1];
        ws->order[ws->n - 1] = s;
        ws->tcount--;
        ws->focus = ws->n - 1;
        s->floating = 1;
        s->w = 480;
        s->h = 320;
        s->x = (comp_screen_w() - s->w) / 2;
        s->y = COMP_BAR_H + (comp_screen_h() - COMP_BAR_H - s->h) / 2;
        comp_send_configure(s, s->w, s->h);
        comp_log("wm: window floating");
    } else {
        for (int i = idx; i > ws->tcount; i--)
            ws->order[i] = ws->order[i - 1];
        ws->order[ws->tcount] = s;
        ws->tcount++;
        ws->focus = ws->tcount - 1;
        s->floating = 0;
        comp_log("wm: window tiled");
    }
    arrange();
}

static void swap_focused(int dir) {
    struct workspace *ws = &workspaces[cur_ws];
    int idx = ws->focus;
    if (idx < 0 || idx >= ws->tcount)
        return;
    int other = idx + dir;
    if (other < 0)
        other = ws->tcount - 1;
    if (other >= ws->tcount)
        other = 0;
    if (other == idx)
        return;
    struct wl_surface *t = ws->order[idx];
    ws->order[idx] = ws->order[other];
    ws->order[other] = t;
    ws->focus = other;
    arrange();
}

void wm_handle_key(uint8_t scancode, int pressed, uint8_t mods) {
    if (!pressed)
        return;
    if (!(mods & MOD_ALT)) {
        struct wl_surface *f = wm_focused_surface();
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
    case SC_J:
        if (shift)
            swap_focused(1);
        else
            focus_index(workspaces[cur_ws].focus + 1);
        break;
    case SC_K:
        if (shift)
            swap_focused(-1);
        else
            focus_index(workspaces[cur_ws].focus - 1);
        break;
    case SC_H:
        workspaces[cur_ws].mfact -= 50;
        if (workspaces[cur_ws].mfact < 100)
            workspaces[cur_ws].mfact = 100;
        arrange();
        break;
    case SC_L:
        workspaces[cur_ws].mfact += 50;
        if (workspaces[cur_ws].mfact > 900)
            workspaces[cur_ws].mfact = 900;
        arrange();
        break;
    case SC_SPACE:
        workspaces[cur_ws].layout =
            (enum layout_kind)((workspaces[cur_ws].layout + 1) % LAYOUT_COUNT);
        comp_log("wm: layout changed");
        arrange();
        break;
    case SC_F:
        toggle_float();
        break;
    case SC_Q: {
        struct wl_surface *f = wm_focused_surface();
        if (f)
            comp_send_close(f);
        break;
    }
    case SC_E:
        if (shift) {
            comp_log("wm: session exit requested");
            clients_broadcast_close();
            comp_request_exit();
        }
        break;
    default:
        break;
    }
}

static struct wl_surface *hit_test(int x, int y, int *on_title, int *on_close) {
    struct workspace *ws = &workspaces[cur_ws];

    for (int i = ws->n - 1; i >= 0; i--) {
        struct wl_surface *s = ws->order[i];
        int fx = s->x - COMP_BORDER;
        int fy = s->y - COMP_TITLE_H;
        int fw = s->w + 2 * COMP_BORDER;
        int fh = s->h + COMP_TITLE_H + COMP_BORDER;
        if (x < fx || x >= fx + fw || y < fy || y >= fy + fh)
            continue;
        if (on_title)
            *on_title = (y < fy + COMP_TITLE_H);
        if (on_close)
            *on_close = (y < fy + COMP_TITLE_H) && (x >= fx + fw - 18);
        return s;
    }
    return 0;
}

void wm_handle_button(int x, int y, uint8_t buttons, uint8_t edge) {
    if (edge & 1) {
        if (buttons & 1) {
            int on_title = 0, on_close = 0;
            struct wl_surface *s = hit_test(x, y, &on_title, &on_close);
            if (!s) {
                drag = 0;
                return;
            }
            struct workspace *ws = &workspaces[cur_ws];
            int idx = ws_index_of(ws, s);
            if (idx >= 0)
                focus_index(idx);
            if (on_close) {
                comp_send_close(s);
                return;
            }

            if (s->floating) {
                if (idx >= 0 && idx != ws->n - 1) {
                    for (int i = idx; i < ws->n - 1; i++)
                        ws->order[i] = ws->order[i + 1];
                    ws->order[ws->n - 1] = s;
                    ws->focus = ws->n - 1;
                    comp_damage_rect(0, COMP_BAR_H, comp_screen_w(),
                                     comp_screen_h() - COMP_BAR_H);
                }
                if (on_title) {
                    drag = s;
                    drag_dx = x - s->x;
                    drag_dy = y - s->y;
                }
            }
        } else {
            drag = 0;
        }
    }
}

void wm_handle_motion(int x, int y) {
    if (!drag)
        return;
    struct wl_surface *s = drag;
    comp_damage_surface(s);
    s->x = x - drag_dx;
    s->y = y - drag_dy;
    if (s->x < -s->w + 32)
        s->x = -s->w + 32;
    if (s->x > comp_screen_w() - 32)
        s->x = comp_screen_w() - 32;
    if (s->y < COMP_BAR_H + COMP_TITLE_H)
        s->y = COMP_BAR_H + COMP_TITLE_H;
    if (s->y > comp_screen_h() - 24)
        s->y = comp_screen_h() - 24;
    comp_damage_surface(s);
}

int wm_collect_visible(struct wl_surface **out, int max) {
    struct workspace *ws = &workspaces[cur_ws];
    int n = ws->n < max ? ws->n : max;
    for (int i = 0; i < n; i++)
        out[i] = ws->order[i];
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

#define BAR_FONT_PX 12
static int bar_text_y(void) {
    return (COMP_BAR_H - font_ascent(BAR_FONT_PX)) / 2;
}
void wm_draw_bar(struct gfx_canvas *c, struct gfx_rect *clip) {
    struct gfx_rect bar = {0, 0, comp_screen_w(), COMP_BAR_H}, v;
    if (!gfx_rect_intersect(bar, *clip, &v))
        return;

    gfx_fill(c, v.x, v.y, v.w, v.h, TH_BAR);
    gfx_hline(c, 0, COMP_BAR_H - 1, comp_screen_w(), TH_BAR_LINE);

    int ty = bar_text_y();
    int x = 8;
    for (int i = 0; i < WL_MAX_WS; i++) {
        int cur = (i == cur_ws);
        int occ = workspaces[i].n > 0;
        int px = x, py = 3, pw = 24, ph = 16;
        gfx_color pill = cur ? TH_ACCENT : (occ ? TH_FRAME_UNF : TH_DIM);
        gfx_fill_round(c, px, py, pw, ph, 7, pill);
        char label[4] = {' ', (char)('1' + i), ' ', 0};
        gfx_color fg = cur ? TH_TITLE_FOC : (occ ? TH_TEXT : TH_MUTED);
        int lw = font_text_width(label, BAR_FONT_PX);
        font_draw(c, px + (pw - lw) / 2, py + (ph - font_ascent(BAR_FONT_PX)) / 2,
                  label, BAR_FONT_PX, fg);
        x += pw + 6;
    }

    font_draw(c, x + 6, ty, layout_name(workspaces[cur_ws].layout),
              BAR_FONT_PX, TH_ACCENT);

    struct wl_surface *f = wm_focused_surface();
    if (f)
        font_draw(c, x + 60, ty, f->title, BAR_FONT_PX, TH_TEXT);

    char right[48];
    char num[12];
    u32_to_dec(tick / PIT_HZ, num);
    right[0] = 0;
    strcat(right, "up ");
    strcat(right, num);
    strcat(right, "s");
    int rw = font_text_width(right, BAR_FONT_PX);
    int rx = comp_screen_w() - 6 - rw;
    font_draw(c, rx, ty, right, BAR_FONT_PX, TH_MUTED);
    int bw = font_text_width("CoreZOS", BAR_FONT_PX);
    int bx = rx - 10 - bw;
    font_draw(c, bx, ty, "CoreZOS", BAR_FONT_PX, TH_ACCENT);
}
