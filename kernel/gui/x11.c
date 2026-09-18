
#include "kernel/gui/x11.h"

#include <stddef.h>
#include <stdint.h>

#include "drivers/char/console/io.h"
#include "kernel/gui/gfx.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/wm.h"
#include "lib/str/str.h"

static struct X11_CONN conns[X11_MAX_CONNS];
static uint32_t xid_next = X11_ROOT_WINDOW + 1;
static uint32_t gid_next = 0x06000200;
static uint32_t pid_next = 0x06000400;
static uint32_t atom_next = 100;

void x11_gateway_init(void) {
    memset(conns, 0, sizeof(conns));
    xid_next = X11_ROOT_WINDOW + 1;
    gid_next = 0x06000200;
    pid_next = 0x06000400;
    atom_next = 100;
    comp_log("x11gw: gateway ready (proto 11.0)");
}

struct X11_CONN *x11_conn_open(void) {
    for (int i = 0; i < X11_MAX_CONNS; i++) {
        if (conns[i].used)
            continue;
        struct X11_CONN *c = &conns[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->seq = 0;
        c->client_id = (uint32_t)(i + 1);
        lock_init(&c->lock);
        return c;
    }
    return 0;
}

void x11_conn_close(struct X11_CONN *c) {
    if (!c || !c->used)
        return;
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (!c->win[i].used)
            continue;
        if (c->win[i].s) {
            c->win[i].s->x11_owner = 0;
            wm_unmanage(c->win[i].s);
            wl_surface_destroy(c->win[i].s);
        }
        if (c->win[i].pool)
            shm_pool_destroy(c->win[i].pool);
    }
    for (int i = 0; i < X11_MAX_PIXMAPS; i++)
        if (c->pix[i].used && c->pix[i].data)
            free_kernel_page((uint32_t)c->pix[i].data);
    memset(c, 0, sizeof(*c));
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#define TRIG_ONE 1024
static const int16_t g_sin_q10[91] = {
    0,    17,   35,   52,   70,   87,   104,  121,  139,  156,  173,  190,
    207,  224,  241,  258,  275,  292,  309,  325,  342,  358,  374,  390,
    406,  422,  438,  453,  469,  484,  499,  514,  529,  543,  558,  572,
    586,  600,  613,  627,  640,  653,  666,  678,  691,  703,  715,  726,
    738,  749,  760,  771,  781,  791,  801,  811,  820,  829,  838,  846,
    855,  863,  870,  878,  885,  892,  898,  905,  911,  916,  922,  927,
    932,  936,  941,  945,  948,  952,  955,  958,  960,  962,  964,  966,
    967,  968,  969,  970,  970,  970,  970,  969};

static int trig_sin(int deg) {
    deg %= 360;
    if (deg < 0)
        deg += 360;
    if (deg <= 90)
        return g_sin_q10[deg];
    if (deg <= 180)
        return g_sin_q10[180 - deg];
    if (deg <= 270)
        return -g_sin_q10[deg - 180];
    return -g_sin_q10[360 - deg];
}

static int trig_cos(int deg) {
    return trig_sin(deg + 90);
}

static int out_bytes(struct X11_CONN *c, const void *p, uint32_t n) {
    lock_acquire(&c->lock);
    int ok = 1;
    if (c->out_head + n > X11_OUT_BUF) {
        uint32_t live = c->out_head - c->out_tail;
        if (live + n > X11_OUT_BUF) {
            ok = 0;
        } else {
            memmove(c->out, c->out + c->out_tail, live);
            c->out_head = live;
            c->out_tail = 0;
        }
    }
    if (ok) {
        memcpy(c->out + c->out_head, p, n);
        c->out_head += n;
    }
    lock_release(&c->lock);
    if (!ok)
        c->dead = 1;
    return ok ? 0 : -1;
}

uint32_t x11_conn_drain(struct X11_CONN *c, uint8_t *buf, uint32_t len) {
    uint32_t n;
    lock_acquire(&c->lock);
    n = c->out_head - c->out_tail;
    if (n > len)
        n = len;
    if (n > 0) {
        memcpy(buf, c->out + c->out_tail, n);
        c->out_tail += n;
        if (c->out_tail == c->out_head)
            c->out_head = c->out_tail = 0;
    }
    lock_release(&c->lock);
    return n;
}

static void post_error(struct X11_CONN *c, uint8_t code, uint8_t major,
                       uint16_t minor, uint32_t res) {
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[1] = code;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 4, res);
    wr16(ev + 8, minor);
    ev[10] = major;
    out_bytes(c, ev, 32);
}

static void reply_init(struct X11_CONN *c, uint8_t major, uint32_t extra,
                       uint8_t *head) {
    memset(head, 0, 32);
    head[0] = 1;
    head[1] = major;
    wr16(head + 2, (uint16_t)c->seq);
    wr32(head + 4, extra / 4);
}

static void reply_finish(struct X11_CONN *c, const uint8_t *head,
                         const uint8_t *extra, uint32_t n) {
    out_bytes(c, head, 32);
    if (n)
        out_bytes(c, extra, n);
}

static void out_event(struct X11_CONN *c, const uint8_t *ev) {
    out_bytes(c, ev, 32);
}

static int win_idx(struct X11_CONN *c, uint32_t xid) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++)
        if (c->win[i].used && c->win[i].xid == xid)
            return i;
    return -1;
}

static int gc_idx(struct X11_CONN *c, uint32_t gid) {
    for (int i = 0; i < X11_MAX_GCS; i++)
        if (c->gc[i].used && c->gc[i].gid == gid)
            return i;
    return -1;
}

static int pix_idx(struct X11_CONN *c, uint32_t pid) {
    for (int i = 0; i < X11_MAX_PIXMAPS; i++)
        if (c->pix[i].used && c->pix[i].pid == pid)
            return i;
    return -1;
}

static uint32_t atom_intern(struct X11_CONN *c, const char *name, int len,
                            int only_if_exists, int *found) {
    for (int i = 0; i < c->atom_n; i++) {
        if ((int)strlen(c->atoms[i].name) == len &&
            memcmp(c->atoms[i].name, name, (size_t)len) == 0) {
            if (found)
                *found = 1;
            return c->atoms[i].atom;
        }
    }
    if (only_if_exists) {
        if (found)
            *found = 0;
        return 0;
    }
    if (c->atom_n >= X11_MAX_ATOMS) {
        if (found)
            *found = 0;
        return 0;
    }
    int n = len < 23 ? len : 23;
    memcpy(c->atoms[c->atom_n].name, name, (size_t)n);
    c->atoms[c->atom_n].name[n] = 0;
    c->atoms[c->atom_n].atom = atom_next++;
    if (found)
        *found = 1;
    return c->atoms[c->atom_n++].atom;
}

struct X11_DRAW {
    struct GFX_CANVAS cv;
    int ok;
};

static struct X11_DRAW draw_get(struct X11_CONN *c, uint32_t id) {
    struct X11_DRAW d;
    memset(&d, 0, sizeof(d));
    int wi = win_idx(c, id);
    if (wi >= 0) {
        struct X11_WINDOW *w = &c->win[wi];
        if (!w->pool || w->w == 0 || w->h == 0)
            return d;
        d.cv.pixels = (gfx_color *)w->pool->data;
        d.cv.pitch = (int)(w->w * 4u);
        d.cv.w = (int)w->w;
        d.cv.h = (int)w->h;
        d.cv.bytes = (size_t)w->w * (size_t)w->h * 4u;
        d.ok = 1;
        return d;
    }
    int pi = pix_idx(c, id);
    if (pi >= 0) {
        struct X11_PIXMAP *p = &c->pix[pi];
        if (!p->data)
            return d;
        d.cv.pixels = (gfx_color *)p->data;
        d.cv.pitch = (int)(p->w * 4u);
        d.cv.w = (int)p->w;
        d.cv.h = (int)p->h;
        d.cv.bytes = p->size;
        d.ok = 1;
    }
    return d;
}

static void draw_commit(struct X11_CONN *c, uint32_t id) {
    int wi = win_idx(c, id);
    if (wi >= 0 && c->win[wi].s)
        wl_surface_commit(c->win[wi].s);
}

static gfx_color pix_to_color(uint32_t pixel) {
    return GFX_RGB((int)((pixel >> 16) & 0xFF), (int)((pixel >> 8) & 0xFF),
                   (int)(pixel & 0xFF));
}

static int16_t clo16(int v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

static int win_realize(struct X11_CONN *c, int i) {
    struct X11_WINDOW *w = &c->win[i];
    if (w->s)
        return 0;
    if (w->w == 0 || w->h == 0)
        return -1;
    uint32_t bytes = (uint32_t)w->w * (uint32_t)w->h * 4u;
    struct WL_SHM_POOL *pool = shm_pool_create(bytes);
    if (!pool)
        return -1;
    char title[24];
    title[0] = 0;
    strcat(title, "x11:");
    char num[12];
    u32_to_dec(w->xid & 0xFFFFFFu, num);
    strcat(title, num);
    struct WL_CLIENT *cl = wl_display_connect("x11gw");
    if (!cl) {
        shm_pool_destroy(pool);
        return -1;
    }
    struct WL_SURFACE *s = wl_compositor_create_surface(cl, title);
    if (!s) {
        shm_pool_destroy(pool);
        wl_display_disconnect(cl);
        return -1;
    }
    w->s = s;
    w->pool = pool;
    s->x11_owner = c;
    s->x11_xid = w->xid;
    struct GFX_CANVAS cv;
    memset(&cv, 0, sizeof(cv));
    cv.pixels = (gfx_color *)pool->data;
    cv.pitch = (int)(w->w * 4u);
    cv.w = (int)w->w;
    cv.h = (int)w->h;
    cv.bytes = bytes;
    gfx_fill(&cv, 0, 0, cv.w, cv.h, pix_to_color(w->bg_pixel));
    int bp = pix_idx(c, w->bg_pixmap);
    if (bp >= 0 && c->pix[bp].data) {
        struct GFX_CANVAS src;
        memset(&src, 0, sizeof(src));
        src.pixels = (gfx_color *)c->pix[bp].data;
        src.pitch = (int)(c->pix[bp].w * 4u);
        src.w = (int)c->pix[bp].w;
        src.h = (int)c->pix[bp].h;
        src.bytes = c->pix[bp].size;
        gfx_blit(&cv, 0, 0, &src, 0, 0, src.w, src.h);
    }
    wl_surface_attach(s, pool, (int)w->w, (int)w->h);
    wl_surface_commit(s);
    wm_manage(s);

    s->x = w->x;
    s->y = w->y;
    s->w = (int)w->w;
    s->h = (int)w->h;
    comp_damage_surface(s);
    return 0;
}

static void win_teardown(struct X11_CONN *c, int i) {
    struct X11_WINDOW *w = &c->win[i];
    if (w->s) {
        w->s->x11_owner = 0;
        wm_unmanage(w->s);
        wl_surface_destroy(w->s);
        w->s = 0;
    }
    if (w->pool) {
        shm_pool_destroy(w->pool);
        w->pool = 0;
    }
    w->used = 0;
    w->mapped = 0;
}

static void post_event_to(struct X11_CONN *c, struct X11_WINDOW *w,
                          const uint8_t *ev, uint32_t mask_bit) {
    if (!w || !w->used || !(w->event_mask & mask_bit))
        return;
    out_event(c, ev);
}

static void h_create_window(struct X11_CONN *c, const uint8_t *p,
                            uint32_t len) {
    if (len < 32)
        return post_error(c, X11_ERR_Length, X11_REQ_CreateWindow, 0, 0);
    uint32_t wid = rd32(p + 4);
    if (win_idx(c, wid) >= 0)
        return post_error(c, X11_ERR_Value, X11_REQ_CreateWindow, 0, wid);
    int slot = -1;
    for (int i = 0; i < X11_MAX_WINDOWS; i++)
        if (!c->win[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreateWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[slot];
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->xid = wid;
    w->depth = p[1];
    w->x = (int16_t)rd16(p + 12);
    w->y = (int16_t)rd16(p + 14);
    w->w = rd16(p + 16);
    w->h = rd16(p + 18);
    w->border = rd16(p + 20);
    w->kind = p[22];
    w->bg_pixel = 0xFFFFFFu;
    w->colormap = X11_DEFAULT_COLORMAP;
    uint32_t mask = rd32(p + 28);
    const uint8_t *v = p + 32;
    uint32_t remain = len - 32;
    for (int bit = 0; bit < 15 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        switch (1u << bit) {
        case X11_CWBackPixmap:
            w->bg_pixmap = val;
            break;
        case X11_CWBackPixel:
            w->bg_pixel = val;
            break;
        case X11_CWOverrideRedirect:
            w->override_redirect = (val != 0);
            break;
        case X11_CWEventMask:
            w->event_mask = val;
            break;
        case X11_CWColormap:
            w->colormap = val;
            break;
        default:
            break;
        }
    }
    if (win_realize(c, slot) != 0) {
        w->used = 0;
        post_error(c, X11_ERR_Alloc, X11_REQ_CreateWindow, 0, wid);
    }
}

static void h_change_attrs(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_ChangeWindowAttributes, 0,
                          0);
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ChangeWindowAttributes, 0,
                          wid);
    struct X11_WINDOW *w = &c->win[i];
    uint32_t mask = rd32(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int bg_changed = 0;
    for (int bit = 0; bit < 15 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        switch (1u << bit) {
        case X11_CWBackPixel:
            w->bg_pixel = val;
            bg_changed = 1;
            break;
        case X11_CWBackPixmap:
            w->bg_pixmap = val;
            bg_changed = 1;
            break;
        case X11_CWEventMask:
            w->event_mask = val;
            break;
        case X11_CWOverrideRedirect:
            w->override_redirect = (val != 0);
            break;
        case X11_CWColormap:
            w->colormap = val;
            break;
        default:
            break;
        }
    }
    if (bg_changed && w->s && w->pool) {
        struct GFX_CANVAS cv;
        memset(&cv, 0, sizeof(cv));
        cv.pixels = (gfx_color *)w->pool->data;
        cv.pitch = (int)(w->w * 4u);
        cv.w = (int)w->w;
        cv.h = (int)w->h;
        cv.bytes = (size_t)w->w * (size_t)w->h * 4u;
        gfx_fill(&cv, 0, 0, cv.w, cv.h, pix_to_color(w->bg_pixel));
        wl_surface_commit(w->s);
    }
}

static void h_get_window_attrs(struct X11_CONN *c, const uint8_t *p,
                               uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_GetWindowAttributes, 0,
                          wid);
    struct X11_WINDOW *w = &c->win[i];
    uint8_t head[32];
    uint8_t extra[12];
    memset(extra, 0, sizeof(extra));
    reply_init(c, X11_REQ_GetWindowAttributes, sizeof(extra), head);
    head[1] = 0;
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_VISUAL);
    wr16(b + 4, w->kind);
    b[6] = 0;
    b[7] = 0;
    wr32(b + 8, 0);
    wr32(b + 12, 0);
    b[16] = 0;
    b[17] = 1;
    b[18] = (uint8_t)(w->mapped ? 2 : 0);
    b[19] = (uint8_t)(w->override_redirect ? 1 : 0);
    wr32(b + 20, w->colormap);
    wr32(extra + 0, w->event_mask);
    wr32(extra + 4, w->event_mask);
    wr16(extra + 8, 0);
    reply_finish(c, head, extra, sizeof(extra));
}

static void h_destroy_window(struct X11_CONN *c, const uint8_t *p,
                             uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_DestroyWindow, 0, wid);
    win_teardown(c, i);
}

static void h_map_window(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_MapWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[i];
    if (win_realize(c, i) != 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_MapWindow, 0, wid);
    w->mapped = 1;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = X11_EV_MAP_NOTIFY;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 4, wid);
    wr32(ev + 8, X11_ROOT_WINDOW);
    ev[12] = (uint8_t)(w->override_redirect ? 1 : 0);
    post_event_to(c, w, ev, 0x00020000u );

    memset(ev, 0, sizeof(ev));
    ev[0] = X11_EV_EXPOSE;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 4, wid);
    wr16(ev + 12, w->w);
    wr16(ev + 14, w->h);
    post_event_to(c, w, ev, 0x00008000u );
}

static void h_unmap_window(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_UnmapWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[i];
    if (w->s) {
        w->s->x11_owner = 0;
        wm_unmanage(w->s);
        wl_surface_destroy(w->s);
        w->s = 0;
    }
    if (w->pool) {
        shm_pool_destroy(w->pool);
        w->pool = 0;
    }
    w->mapped = 0;
}

static void h_configure_window(struct X11_CONN *c, const uint8_t *p,
                               uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_ConfigureWindow, 0, 0);
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ConfigureWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[i];
    uint16_t mask = rd16(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int32_t nx = w->x, ny = w->y;
    uint32_t nw = w->w, nh = w->h;
    int resize = 0, move = 0;
    for (int bit = 0; bit < 15 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        switch (bit) {
        case 0:
            nx = (int32_t)val;
            move = 1;
            break;
        case 1:
            ny = (int32_t)val;
            move = 1;
            break;
        case 2:
            nw = val & 0xFFFFu;
            resize = 1;
            break;
        case 3:
            nh = val & 0xFFFFu;
            resize = 1;
            break;
        default:
            break;
        }
    }
    if (!w->s)
        return;
    w->x = clo16(nx);
    w->y = clo16(ny);
    if (resize && nw > 0 && nh > 0 && (nw != w->w || nh != w->h)) {
        struct WL_SHM_POOL *np = shm_pool_create(nw * nh * 4u);
        if (!np)
            return post_error(c, X11_ERR_Alloc, X11_REQ_ConfigureWindow, 0,
                              wid);
        if (w->pool)
            shm_pool_destroy(w->pool);
        w->pool = np;
        w->w = nw;
        w->h = nh;
        struct GFX_CANVAS cv;
        memset(&cv, 0, sizeof(cv));
        cv.pixels = (gfx_color *)np->data;
        cv.pitch = (int)(nw * 4u);
        cv.w = (int)nw;
        cv.h = (int)nh;
        cv.bytes = (size_t)nw * (size_t)nh * 4u;
        gfx_fill(&cv, 0, 0, cv.w, cv.h, pix_to_color(w->bg_pixel));
        wl_surface_attach(w->s, np, (int)nw, (int)nh);
    }
    if (move || resize) {
        comp_damage_surface(w->s);
        w->s->x = w->x;
        w->s->y = w->y;
        w->s->w = (int)w->w;
        w->s->h = (int)w->h;
        comp_send_configure(w->s, (int)w->w, (int)w->h);
        comp_damage_surface(w->s);
    }
}

static void h_get_geometry(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    uint8_t head[32];
    reply_init(c, X11_REQ_GetGeometry, 0, head);
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_WINDOW);
    int wi = win_idx(c, did);
    head[1] = 24;
    if (wi >= 0) {
        struct X11_WINDOW *w = &c->win[wi];
        wr16(b + 4, (uint16_t)w->x);
        wr16(b + 6, (uint16_t)w->y);
        wr16(b + 8, w->w);
        wr16(b + 10, w->h);
        wr16(b + 12, w->border);
        head[1] = w->depth;
    } else {
        wr16(b + 8, (uint16_t)comp_screen_w());
        wr16(b + 10, (uint16_t)comp_screen_h());
    }
    reply_finish(c, head, 0, 0);
}

static void h_query_tree(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    if (wid != X11_ROOT_WINDOW && win_idx(c, wid) < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_QueryTree, 0, wid);
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryTree, 0, head);
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_WINDOW);
    wr32(b + 4, 0);
    wr16(b + 8, 0);
    reply_finish(c, head, 0, 0);
}

static void h_intern_atom(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 8)
        return post_error(c, X11_ERR_Length, X11_REQ_InternAtom, 0, 0);
    uint16_t nl = rd16(p + 4);
    int only = p[1];
    const char *name = (const char *)(p + 8);
    if ((uint32_t)nl > len - 8)
        return post_error(c, X11_ERR_Length, X11_REQ_InternAtom, 0, 0);
    int found = 0;
    uint32_t atom = atom_intern(c, name, nl, only, &found);
    uint8_t head[32];
    reply_init(c, X11_REQ_InternAtom, 0, head);
    wr32(head + 8, atom);
    reply_finish(c, head, 0, 0);
}

static void h_change_property(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    if (len < 24)
        return post_error(c, X11_ERR_Length, X11_REQ_ChangeProperty, 0, 0);
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ChangeProperty, 0, wid);
    uint32_t prop = rd32(p + 8);
    uint32_t type = rd32(p + 12);
    uint8_t format = p[16];
    uint32_t n = rd32(p + 20);
    uint32_t nbytes = n * (format / 8);
    if (nbytes > 64)
        nbytes = 64;
    if (nbytes > len - 24)
        nbytes = len - 24;
    struct X11_WINDOW *w = &c->win[i];
    int slot = -1;
    for (int k = 0; k < X11_MAX_PROPS; k++)
        if (w->props[k].used && w->props[k].atom == prop) {
            slot = k;
            break;
        }
    if (slot < 0)
        for (int k = 0; k < X11_MAX_PROPS; k++)
            if (!w->props[k].used) {
                slot = k;
                break;
            }
    if (slot < 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_ChangeProperty, 0, prop);
    memset(&w->props[slot], 0, sizeof(w->props[slot]));
    w->props[slot].used = 1;
    w->props[slot].atom = prop;
    w->props[slot].type = type;
    w->props[slot].format = format;
    w->props[slot].nbytes = nbytes;
    if (nbytes)
        memcpy(w->props[slot].data, p + 24, nbytes);
}

static void h_get_property(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_GetProperty, 0, wid);
    uint32_t prop = rd32(p + 8);
    struct X11_WINDOW *w = &c->win[i];
    uint8_t head[32];
    uint8_t extra[64];
    memset(extra, 0, sizeof(extra));
    uint32_t nbytes = 0;
    reply_init(c, X11_REQ_GetProperty, 0, head);
    for (int k = 0; k < X11_MAX_PROPS; k++) {
        if (!w->props[k].used || w->props[k].atom != prop)
            continue;
        head[1] = w->props[k].format;
        uint8_t *b = head + 8;
        wr32(b + 0, w->props[k].type);
        wr32(b + 4, 0);
        wr32(b + 8, w->props[k].nbytes / (w->props[k].format / 8));
        memcpy(extra, w->props[k].data, w->props[k].nbytes);
        nbytes = ((w->props[k].nbytes + 3) / 4) * 4;
        break;
    }
    wr32(head + 4, nbytes / 4);
    reply_finish(c, head, extra, nbytes);
}

static void h_delete_property(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_DeleteProperty, 0, wid);
    uint32_t prop = rd32(p + 8);
    for (int k = 0; k < X11_MAX_PROPS; k++)
        if (c->win[i].props[k].used && c->win[i].props[k].atom == prop)
            c->win[i].props[k].used = 0;
}

static void h_query_pointer(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)p;
    (void)len;
    int px = comp_pointer_x(), py = comp_pointer_y();
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryPointer, 0, head);
    head[1] = 1;
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_WINDOW);
    wr32(b + 4, 0);
    wr16(b + 8, (uint16_t)px);
    wr16(b + 10, (uint16_t)py);
    wr16(b + 12, (uint16_t)px);
    wr16(b + 14, (uint16_t)py);
    wr16(b + 16, (uint16_t)comp_pointer_buttons());
    reply_finish(c, head, 0, 0);
}

static void h_translate_coords(struct X11_CONN *c, const uint8_t *p,
                               uint32_t len) {
    (void)len;
    int wi = win_idx(c, rd32(p + 4));
    if (wi < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_TranslateCoordinates, 0,
                          rd32(p + 4));
    struct X11_WINDOW *w = &c->win[wi];
    int16_t sx = (int16_t)rd16(p + 12), sy = (int16_t)rd16(p + 14);
    uint8_t head[32];
    reply_init(c, X11_REQ_TranslateCoordinates, 0, head);
    head[1] = 1;
    uint8_t *b = head + 8;
    wr32(b + 0, 0);
    wr16(b + 4, (uint16_t)(sx + w->x));
    wr16(b + 6, (uint16_t)(sy + w->y));
    reply_finish(c, head, 0, 0);
}

static void h_get_input_focus(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_GetInputFocus, 0, head);
    head[1] = 0;
    wr32(head + 8, c->focus);
    reply_finish(c, head, 0, 0);
}

static void h_set_input_focus(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    if (wid != 0 && wid != 1 && win_idx(c, wid) < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_SetInputFocus, 0, wid);
    c->focus = wid;
}

static void h_open_font(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t fid = rd32(p + 4);
    if (fid & 0x3)
        return post_error(c, X11_ERR_Value, X11_REQ_OpenFont, 0, fid);
    uint8_t head[32];
    reply_init(c, X11_REQ_OpenFont, 0, head);
    reply_finish(c, head, 0, 0);
}

static void h_query_text_extents(struct X11_CONN *c, const uint8_t *p,
                                 uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryTextExtents, 0, head);
    head[1] = 0;
    reply_finish(c, head, 0, 0);
}

static void h_create_pixmap(struct X11_CONN *c, const uint8_t *p,
                            uint32_t len) {
    (void)len;
    uint32_t pid = rd32(p + 4);
    uint32_t w = rd16(p + 12), h = rd16(p + 14);
    if (pix_idx(c, pid) >= 0)
        return post_error(c, X11_ERR_Value, X11_REQ_CreatePixmap, 0, pid);
    int slot = -1;
    for (int i = 0; i < X11_MAX_PIXMAPS; i++)
        if (!c->pix[i].used) {
            slot = i;
            break;
        }
    if (slot < 0 || w == 0 || h == 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreatePixmap, 0, pid);
    uint32_t bytes = w * h * 4u;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (!mem)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreatePixmap, 0, pid);
    memset(mem, 0, pages * PAGE_SIZE);
    c->pix[slot].used = 1;
    c->pix[slot].pid = pid;
    c->pix[slot].w = w;
    c->pix[slot].h = h;
    c->pix[slot].depth = p[1];
    c->pix[slot].data = mem;
    c->pix[slot].size = bytes;
    (void)pid_next;
}

static void h_free_pixmap(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t pid = rd32(p + 4);
    int i = pix_idx(c, pid);
    if (i < 0)
        return post_error(c, X11_ERR_Pixmap, X11_REQ_FreePixmap, 0, pid);
    if (c->pix[i].data)
        free_kernel_page((uint32_t)c->pix[i].data);
    memset(&c->pix[i], 0, sizeof(c->pix[i]));
}

static void h_create_gc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_CreateGC, 0, 0);
    uint32_t gid = rd32(p + 4);
    if (gc_idx(c, gid) >= 0)
        return post_error(c, X11_ERR_Value, X11_REQ_CreateGC, 0, gid);
    int slot = -1;
    for (int i = 0; i < X11_MAX_GCS; i++)
        if (!c->gc[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreateGC, 0, gid);
    memset(&c->gc[slot], 0, sizeof(c->gc[slot]));
    c->gc[slot].used = 1;
    c->gc[slot].gid = gid;
    c->gc[slot].fg = 0x000000u;
    c->gc[slot].bg = 0xFFFFFFu;
    c->gc[slot].line_width = 1;
    uint32_t mask = rd32(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    for (int bit = 0; bit < 23 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        if ((1u << bit) == X11_GC_Foreground)
            c->gc[slot].fg = val;
        else if ((1u << bit) == X11_GC_Background)
            c->gc[slot].bg = val;
        else if ((1u << bit) == X11_GC_LineWidth)
            c->gc[slot].line_width = val;
    }
    (void)gid_next;
}

static void h_change_gc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_ChangeGC, 0, 0);
    int gi = gc_idx(c, rd32(p + 4));
    if (gi < 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_ChangeGC, 0,
                          rd32(p + 4));
    uint32_t mask = rd32(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    for (int bit = 0; bit < 23 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        if ((1u << bit) == X11_GC_Foreground)
            c->gc[gi].fg = val;
        else if ((1u << bit) == X11_GC_Background)
            c->gc[gi].bg = val;
        else if ((1u << bit) == X11_GC_LineWidth)
            c->gc[gi].line_width = val;
    }
}

static void h_free_gc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t gid = rd32(p + 4);
    int i = gc_idx(c, gid);
    if (i < 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_FreeGC, 0, gid);
    c->gc[i].used = 0;
}

static void h_clear_area(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int wi = win_idx(c, wid);
    if (wi < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ClearArea, 0, wid);
    struct X11_DRAW d = draw_get(c, wid);
    if (!d.ok)
        return;
    int16_t x = (int16_t)rd16(p + 8), y = (int16_t)rd16(p + 10);
    uint16_t w = rd16(p + 12), h = rd16(p + 14);
    if (p[1]) {
        uint8_t ev[32];
        memset(ev, 0, sizeof(ev));
        ev[0] = X11_EV_EXPOSE;
        wr16(ev + 2, (uint16_t)c->seq);
        wr32(ev + 4, wid);
        wr16(ev + 8, (uint16_t)x);
        wr16(ev + 10, (uint16_t)y);
        wr16(ev + 12, w);
        wr16(ev + 14, h);
        post_event_to(c, &c->win[wi], ev, 0x00008000u);
    }
    if (w == 0 || h == 0) {
        w = (uint16_t)d.cv.w;
        h = (uint16_t)d.cv.h;
        x = 0;
        y = 0;
    }
    gfx_fill(&d.cv, x, y, w, h, pix_to_color(c->win[wi].bg_pixel));
    draw_commit(c, wid);
}

static void h_copy_area(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    struct X11_DRAW src = draw_get(c, rd32(p + 4));
    struct X11_DRAW dst = draw_get(c, rd32(p + 8));
    if (!src.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_CopyArea, 0,
                          rd32(p + 4));
    if (!dst.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_CopyArea, 0,
                          rd32(p + 8));
    int16_t sx = (int16_t)rd16(p + 12), sy = (int16_t)rd16(p + 14);
    int16_t dx = (int16_t)rd16(p + 16), dy = (int16_t)rd16(p + 18);
    uint16_t w = rd16(p + 20), h = rd16(p + 22);
    gfx_blit(&dst.cv, dx, dy, &src.cv, sx, sy, w, h);
    draw_commit(c, rd32(p + 8));
}

static int gc_fg(struct X11_CONN *c, uint32_t gid, gfx_color *out) {
    int gi = gc_idx(c, gid);
    if (gi < 0)
        return -1;
    *out = pix_to_color(c->gc[gi].fg);
    return 0;
}

static void h_poly_point(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyPoint, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyPoint, 0,
                          rd32(p + 8));
    uint8_t coord_mode = p[1];
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int cur_x = 0, cur_y = 0;
    while (remain >= 4) {
        int16_t px = (int16_t)rd16(v), py = (int16_t)rd16(v + 2);
        if (coord_mode == 0) {
            gfx_px(&d.cv, px, py, fg);
            cur_x = px;
            cur_y = py;
        } else {
            gfx_px(&d.cv, cur_x + px, cur_y + py, fg);
            cur_x += px;
            cur_y += py;
        }
        v += 4;
        remain -= 4;
    }
    draw_commit(c, did);
}

static void draw_line(struct GFX_CANVAS *cv, int x0, int y0, int x1, int y1,
                      gfx_color col) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    int err = dx - dy;
    for (;;) {
        gfx_px(cv, x0, y0, col);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void h_poly_line(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyLine, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyLine, 0,
                          rd32(p + 8));
    uint8_t coord_mode = p[1];
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int cur_x = 0, cur_y = 0, first = 1;
    while (remain >= 4) {
        int16_t gx = (int16_t)rd16(v), gy = (int16_t)rd16(v + 2);
        int px = (coord_mode == 0) ? gx : cur_x + gx;
        int py = (coord_mode == 0) ? gy : cur_y + gy;
        if (!first)
            draw_line(&d.cv, cur_x, cur_y, px, py, fg);
        cur_x = px;
        cur_y = py;
        first = 0;
        v += 4;
        remain -= 4;
    }
    draw_commit(c, did);
}

static void h_poly_segment(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolySegment, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolySegment, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 8) {
        draw_line(&d.cv, (int16_t)rd16(v), (int16_t)rd16(v + 2),
                  (int16_t)rd16(v + 4), (int16_t)rd16(v + 6), fg);
        v += 8;
        remain -= 8;
    }
    draw_commit(c, did);
}

static void h_poly_rectangle(struct X11_CONN *c, const uint8_t *p,
                             uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyRectangle, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyRectangle, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 8) {
        gfx_rect(&d.cv, (int16_t)rd16(v), (int16_t)rd16(v + 2), rd16(v + 4),
                 rd16(v + 6), fg);
        v += 8;
        remain -= 8;
    }
    draw_commit(c, did);
}

static void h_poly_fill_rect(struct X11_CONN *c, const uint8_t *p,
                             uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyFillRectangle, 0,
                          did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyFillRectangle, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 8) {
        gfx_fill(&d.cv, (int16_t)rd16(v), (int16_t)rd16(v + 2), rd16(v + 4),
                 rd16(v + 6), fg);
        v += 8;
        remain -= 8;
    }
    draw_commit(c, did);
}

static void h_poly_arc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyArc, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyArc, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 12) {
        int x = (int16_t)rd16(v), y = (int16_t)rd16(v + 2);
        int w = rd16(v + 4), h = rd16(v + 6);
        int a1 = (int16_t)rd16(v + 8), a2 = (int16_t)rd16(v + 10);
        int rx = w / 2, ry = h / 2;
        int cx = x + rx, cy = y + ry;
        if (a2 < a1)
            a2 += 360;
        for (int a = a1; a <= a2; a += 2) {
            int idx = a % 360;
            if (idx < 0)
                idx += 360;
            int px = cx + (int)((long)rx * trig_cos(idx) / TRIG_ONE);
            int py = cy + (int)((long)ry * trig_sin(idx) / TRIG_ONE);
            gfx_px(&d.cv, px, py, fg);
        }
        v += 12;
        remain -= 12;
    }
    draw_commit(c, did);
}

static void h_poly_fill_arc(struct X11_CONN *c, const uint8_t *p,
                            uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyFillArc, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyFillArc, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 12) {
        int x = (int16_t)rd16(v), y = (int16_t)rd16(v + 2);
        int w = rd16(v + 4), h = rd16(v + 6);
        int rad = (w < h ? w : h) / 2;
        gfx_fill_round(&d.cv, x, y, w, h, rad, fg);
        v += 12;
        remain -= 12;
    }
    draw_commit(c, did);
}

static void h_fill_poly(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_FillPoly, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_FillPoly, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int16_t xs[16], ys[16];
    int n = 0;
    while (remain >= 4 && n < 16) {
        xs[n] = (int16_t)rd16(v);
        ys[n] = (int16_t)rd16(v + 2);
        n++;
        v += 4;
        remain -= 4;
    }
    if (n < 3)
        return;
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < ymin)
            ymin = ys[i];
        if (ys[i] > ymax)
            ymax = ys[i];
    }
    for (int y = ymin; y <= ymax; y++) {
        int xints[16], cnt = 0;
        for (int i = 0; i < n && cnt < 15; i++) {
            int j = (i + 1) % n;
            if ((ys[i] <= y && ys[j] > y) || (ys[j] <= y && ys[i] > y)) {
                int num = (y - ys[i]) * (xs[j] - xs[i]);
                int den = ys[j] - ys[i];
                xints[cnt++] = xs[i] + (den != 0 ? num / den : 0);
            }
        }
        for (int i = 0; i < cnt; i++)
            for (int k = i + 1; k < cnt; k++)
                if (xints[k] < xints[i]) {
                    int t = xints[i];
                    xints[i] = xints[k];
                    xints[k] = t;
                }
        for (int i = 0; i + 1 < cnt; i += 2)
            gfx_hline(&d.cv, xints[i], y, xints[i + 1] - xints[i] + 1, fg);
    }
    draw_commit(c, did);
}

static void h_put_image(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 28)
        return post_error(c, X11_ERR_Length, X11_REQ_PutImage, 0, 0);
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PutImage, 0, did);
    uint16_t w = rd16(p + 12), h = rd16(p + 14);
    int16_t dx = (int16_t)rd16(p + 16), dy = (int16_t)rd16(p + 18);
    uint8_t depth = p[21];
    uint8_t format = p[1];
    if (format != 2 || (depth != 24 && depth != 32))
        return post_error(c, X11_ERR_Match, X11_REQ_PutImage, 0, did);
    const uint8_t *src = p + 24;
    uint32_t row_bytes = (depth == 32) ? (uint32_t)w * 4u
                                       : (((uint32_t)w * 3u + 3u) & ~3u);
    uint32_t avail = len - 24;
    uint32_t rows = (row_bytes > 0) ? (avail / row_bytes) : 0;
    if (rows > h)
        rows = h;
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *s = src + r * row_bytes;
        int py = dy + (int)r;
        if (py < 0 || py >= d.cv.h)
            continue;
        for (uint32_t x = 0; x < w; x++) {
            int px = dx + (int)x;
            if (px < 0 || px >= d.cv.w)
                continue;
            const uint8_t *pixp = s + (depth == 32 ? x * 4u : x * 3u);
            gfx_px(&d.cv, px, py,
                   GFX_RGB((int)pixp[2], (int)pixp[1], (int)pixp[0]));
        }
    }
    draw_commit(c, did);
}

static void h_get_image(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 20)
        return post_error(c, X11_ERR_Length, X11_REQ_GetImage, 0, 0);
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_GetImage, 0, did);
    int16_t x = (int16_t)rd16(p + 12), y = (int16_t)rd16(p + 14);
    uint16_t w = rd16(p + 16), h = rd16(p + 18);
    uint8_t format = p[1];
    if (format != 2)
        return post_error(c, X11_ERR_Match, X11_REQ_GetImage, 0, did);
    uint32_t row_bytes = ((uint32_t)w * 3u + 3u) & ~3u;
    uint32_t total = row_bytes * h;
    uint8_t head[32];
    reply_init(c, X11_REQ_GetImage, total, head);
    head[1] = 24;
    out_bytes(c, head, 32);
    for (uint32_t r = 0; r < h; r++) {
        uint8_t row[2048];
        uint32_t n = row_bytes < sizeof(row) ? row_bytes : (uint32_t)sizeof(row);
        memset(row, 0, n);
        for (uint32_t xx = 0; xx < w; xx++) {
            int px = x + (int)xx, py = y + (int)r;
            if (px < 0 || py < 0 || px >= d.cv.w || py >= d.cv.h)
                continue;
            gfx_color col = *(gfx_color *)(void *)((uint8_t *)d.cv.pixels +
                                                   (size_t)py *
                                                       (size_t)d.cv.pitch +
                                                   (size_t)px * 4u);
            if (xx * 3u + 2 < n) {
                row[xx * 3 + 0] = (uint8_t)GFX_B(col);
                row[xx * 3 + 1] = (uint8_t)GFX_G(col);
                row[xx * 3 + 2] = (uint8_t)GFX_R(col);
            }
        }
        out_bytes(c, row, n);
    }
}

static void h_alloc_color(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint16_t r = rd16(p + 8), g = rd16(p + 10), b = rd16(p + 12);
    uint32_t pixel = ((uint32_t)(r >> 8) << 16) | ((uint32_t)(g >> 8) << 8) |
                     (uint32_t)(b >> 8);
    uint8_t head[32];
    reply_init(c, X11_REQ_AllocColor, 0, head);
    uint8_t *bb = head + 8;
    wr16(bb + 0, r);
    wr16(bb + 2, g);
    wr16(bb + 4, b);
    wr32(bb + 8, pixel);
    reply_finish(c, head, 0, 0);
}

static void h_alloc_named_color(struct X11_CONN *c, const uint8_t *p,
                                uint32_t len) {
    (void)len;
    uint16_t nl = rd16(p + 8);
    char name[24];
    int n = nl < 23 ? nl : 23;
    memcpy(name, p + 12, (size_t)n);
    name[n] = 0;
    uint32_t pixel = 0xFFFFFFu;
    if (strncmp(name, "black", 5) == 0)
        pixel = 0x000000u;
    else if (strncmp(name, "red", 3) == 0)
        pixel = 0xFF0000u;
    else if (strncmp(name, "green", 5) == 0)
        pixel = 0x00FF00u;
    else if (strncmp(name, "blue", 4) == 0)
        pixel = 0x0000FFu;
    else if (strncmp(name, "gray", 4) == 0)
        pixel = 0x808080u;
    uint8_t head[32];
    reply_init(c, X11_REQ_AllocNamedColor, 0, head);
    uint8_t *b = head + 8;
    wr32(b + 0, pixel);
    uint16_t rv = (uint16_t)(((pixel >> 16) & 0xFF) * 257);
    uint16_t gv = (uint16_t)(((pixel >> 8) & 0xFF) * 257);
    uint16_t bv = (uint16_t)((pixel & 0xFF) * 257);
    wr16(b + 4, rv);
    wr16(b + 6, gv);
    wr16(b + 8, bv);
    wr16(b + 10, rv);
    wr16(b + 12, gv);
    wr16(b + 14, bv);
    reply_finish(c, head, 0, 0);
}

static void h_query_colors(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t n = len >= 12 ? (len - 8) / 4 : 0;
    if (n > 3)
        n = 3;
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryColors, 0, head);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pix = rd32(p + 8 + i * 4);
        uint8_t *b = head + 8 + i * 8;
        wr16(b + 0, (uint16_t)(((pix >> 16) & 0xFF) * 257));
        wr16(b + 2, (uint16_t)(((pix >> 8) & 0xFF) * 257));
        wr16(b + 4, (uint16_t)((pix & 0xFF) * 257));
    }
    reply_finish(c, head, 0, 0);
}

static void h_lookup_color(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint16_t nl = rd16(p + 8);
    char name[24];
    int n = nl < 23 ? nl : 23;
    memcpy(name, p + 12, (size_t)n);
    name[n] = 0;
    uint32_t pixel = 0xFFFFFFu;
    if (strncmp(name, "black", 5) == 0)
        pixel = 0x000000u;
    uint8_t head[32];
    reply_init(c, X11_REQ_LookupColor, 0, head);
    uint8_t *b = head + 8;
    uint16_t rv = (uint16_t)(((pixel >> 16) & 0xFF) * 257);
    uint16_t gv = (uint16_t)(((pixel >> 8) & 0xFF) * 257);
    uint16_t bv = (uint16_t)((pixel & 0xFF) * 257);
    wr16(b + 0, rv);
    wr16(b + 2, gv);
    wr16(b + 4, bv);
    wr16(b + 6, rv);
    wr16(b + 8, gv);
    wr16(b + 10, bv);
    reply_finish(c, head, 0, 0);
}

static void h_query_extension(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryExtension, 0, head);
    head[1] = 0;
    head[8] = 0;
    reply_finish(c, head, 0, 0);
}

static void h_list_extensions(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_ListExtensions, 0, head);
    head[1] = 0;
    reply_finish(c, head, 0, 0);
}

static void dispatch(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    uint8_t op = p[0];
    uint8_t minor = p[1];
    switch (op) {
    case X11_REQ_CreateWindow:
        h_create_window(c, p, len);
        break;
    case X11_REQ_ChangeWindowAttributes:
        h_change_attrs(c, p, len);
        break;
    case X11_REQ_GetWindowAttributes:
        h_get_window_attrs(c, p, len);
        break;
    case X11_REQ_DestroyWindow:
        h_destroy_window(c, p, len);
        break;
    case X11_REQ_MapWindow:
        h_map_window(c, p, len);
        break;
    case X11_REQ_UnmapWindow:
        h_unmap_window(c, p, len);
        break;
    case X11_REQ_ConfigureWindow:
        h_configure_window(c, p, len);
        break;
    case X11_REQ_GetGeometry:
        h_get_geometry(c, p, len);
        break;
    case X11_REQ_QueryTree:
        h_query_tree(c, p, len);
        break;
    case X11_REQ_InternAtom:
        h_intern_atom(c, p, len);
        break;
    case X11_REQ_ChangeProperty:
        h_change_property(c, p, len);
        break;
    case X11_REQ_GetProperty:
        h_get_property(c, p, len);
        break;
    case X11_REQ_DeleteProperty:
        h_delete_property(c, p, len);
        break;
    case X11_REQ_QueryPointer:
        h_query_pointer(c, p, len);
        break;
    case X11_REQ_TranslateCoordinates:
        h_translate_coords(c, p, len);
        break;
    case X11_REQ_GetInputFocus:
        h_get_input_focus(c, p, len);
        break;
    case X11_REQ_SetInputFocus:
        h_set_input_focus(c, p, len);
        break;
    case X11_REQ_OpenFont:
        h_open_font(c, p, len);
        break;
    case X11_REQ_QueryTextExtents:
        h_query_text_extents(c, p, len);
        break;
    case X11_REQ_CreatePixmap:
        h_create_pixmap(c, p, len);
        break;
    case X11_REQ_FreePixmap:
        h_free_pixmap(c, p, len);
        break;
    case X11_REQ_CreateGC:
        h_create_gc(c, p, len);
        break;
    case X11_REQ_ChangeGC:
        h_change_gc(c, p, len);
        break;
    case X11_REQ_FreeGC:
        h_free_gc(c, p, len);
        break;
    case X11_REQ_ClearArea:
        h_clear_area(c, p, len);
        break;
    case X11_REQ_CopyArea:
        h_copy_area(c, p, len);
        break;
    case X11_REQ_PolyPoint:
        h_poly_point(c, p, len);
        break;
    case X11_REQ_PolyLine:
        h_poly_line(c, p, len);
        break;
    case X11_REQ_PolySegment:
        h_poly_segment(c, p, len);
        break;
    case X11_REQ_PolyRectangle:
        h_poly_rectangle(c, p, len);
        break;
    case X11_REQ_PolyArc:
        h_poly_arc(c, p, len);
        break;
    case X11_REQ_PolyFillArc:
        h_poly_fill_arc(c, p, len);
        break;
    case X11_REQ_FillPoly:
        h_fill_poly(c, p, len);
        break;
    case X11_REQ_PolyFillRectangle:
        h_poly_fill_rect(c, p, len);
        break;
    case X11_REQ_PutImage:
        h_put_image(c, p, len);
        break;
    case X11_REQ_GetImage:
        h_get_image(c, p, len);
        break;
    case X11_REQ_AllocColor:
        h_alloc_color(c, p, len);
        break;
    case X11_REQ_AllocNamedColor:
        h_alloc_named_color(c, p, len);
        break;
    case X11_REQ_QueryColors:
        h_query_colors(c, p, len);
        break;
    case X11_REQ_LookupColor:
        h_lookup_color(c, p, len);
        break;
    case X11_REQ_QueryExtension:
        h_query_extension(c, p, len);
        break;
    case X11_REQ_ListExtensions:
        h_list_extensions(c, p, len);
        break;
    case X11_REQ_KillClient:
        c->dead = 1;
        break;
    case X11_REQ_SetCloseDownMode:
    case X11_REQ_SetAccessControl:
    case X11_REQ_Bell:
    case X11_REQ_SetScreenSaver:
    case X11_REQ_InstallColormap:
    case X11_REQ_FreeColormap:
    case X11_REQ_CreateColormap:
    case X11_REQ_SetDashes:
    case X11_REQ_SetClipRectangles:
    case X11_REQ_CopyGC:
    case X11_REQ_CloseFont:
    case X11_REQ_ImageText8:
    case X11_REQ_NoOperation:
        break;
    default:
        post_error(c, X11_ERR_Request, op, minor, 0);
        break;
    }
}

static int send_setup_reply(struct X11_CONN *c) {
    const char *vendor = "NCS X11 Gateway";
    uint32_t vlen = (uint32_t)strlen(vendor);
    uint32_t vpad = (vlen + 3) & ~3u;
    uint32_t add = vpad + 8 + 40 + 8 + 24;
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    uint32_t o = 8;
    buf[0] = 1;
    wr16(buf + 2, X11_PROTO_MAJOR);
    wr16(buf + 4, X11_PROTO_MINOR);
    wr16(buf + 6, (uint16_t)(add / 4));
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    wr32(body + 0, 0x00010000u);
    wr32(body + 4, 0x06000000u);
    wr32(body + 8, 0x000FFFFFu);
    wr32(body + 12, 0);
    wr16(body + 16, (uint16_t)vlen);
    wr16(body + 18, 256);
    body[20] = 1;
    body[21] = 1;
    body[22] = 0;
    body[23] = 0;
    uint8_t pad8[8];
    memset(pad8, 0, sizeof(pad8));
    pad8[0] = 32;
    pad8[1] = 32;
    pad8[2] = 8;
    pad8[3] = 255;
    (void)o;
    uint8_t out[256];
    memset(out, 0, sizeof(out));
    memcpy(out, buf, 8);
    memcpy(out + 8, body, 24);
    uint32_t w = 32;
    memcpy(out + w, pad8, 8);
    w += 8;
    memcpy(out + w, vendor, vlen);
    w += vpad;
    out[w++] = 24;
    out[w++] = 32;
    out[w++] = 32;
    out[w++] = 0;
    w += 4;
    wr32(out + w, X11_ROOT_WINDOW);
    w += 4;
    wr32(out + w, X11_DEFAULT_COLORMAP);
    w += 4;
    wr32(out + w, 0xFFFFFFu);
    w += 4;
    wr32(out + w, 0x000000u);
    w += 4;
    wr32(out + w, 0);
    w += 4;
    wr16(out + w, (uint16_t)comp_screen_w());
    w += 2;
    wr16(out + w, (uint16_t)comp_screen_h());
    w += 2;
    wr16(out + w, 340);
    w += 2;
    wr16(out + w, 255);
    w += 2;
    wr16(out + w, 1);
    w += 2;
    wr16(out + w, 1);
    w += 2;
    wr32(out + w, X11_ROOT_VISUAL);
    w += 4;
    out[w++] = 0;
    out[w++] = 0;
    out[w++] = 24;
    out[w++] = 1;
    out[w++] = 24;
    out[w++] = 0;
    wr16(out + w, 1);
    w += 2;
    w += 4;
    wr32(out + w, X11_ROOT_VISUAL);
    w += 4;
    out[w++] = 4;
    out[w++] = 8;
    wr16(out + w, 256);
    w += 2;
    wr32(out + w, 0x00FF0000u);
    w += 4;
    wr32(out + w, 0x0000FF00u);
    w += 4;
    wr32(out + w, 0x000000FFu);
    w += 4;
    w += 4;
    return out_bytes(c, out, w);
}

static int handshake(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return 0;
    uint8_t order = p[0];
    uint16_t nlen = rd16(p + 6);
    uint16_t dlen = rd16(p + 8);
    uint32_t need = 12 + ((nlen + 3) & ~3u) + ((dlen + 3) & ~3u);
    if (len < need)
        return 0;
    if (send_setup_reply(c) != 0) {
        c->dead = 1;
        return -1;
    }
    kprintf("x11gw: client connected (%s, proto %u.%u)\n",
            order == 0x6C ? "LSBFirst" : "MSBFirst", rd16(p + 2), rd16(p + 4));
    c->seq = 0;
    return (int)need;
}

int x11_conn_feed(struct X11_CONN *c, const uint8_t *data, uint32_t len) {
    if (!c || !c->used || !data)
        return -1;
    uint32_t off = 0;
    if (c->seq == 0 && c->req_len == 0 && len >= 1 &&
        (data[0] == 0x6C || data[0] == 0x42)) {
        int used = handshake(c, data, len);
        if (used < 0)
            return 0;
        if (used == 0)
            return (int)len;
        off = (uint32_t)used;
    }
    while (off < len) {
        if (c->req_need == 0) {
            uint32_t avail = len - off;
            if (c->req_len < 4 && avail >= 4 - c->req_len) {
                memcpy(c->req + c->req_len, data + off, 4 - c->req_len);
                off += 4 - c->req_len;
                c->req_len = 4;
            } else if (c->req_len < 4) {
                memcpy(c->req + c->req_len, data + off, avail);
                c->req_len += avail;
                return (int)off;
            }
            uint16_t units = rd16(c->req + 2);
            if (units == 0)
                return -1;
            c->req_need = (uint32_t)units * 4u;
            if (c->req_need > X11_REQ_BUF)
                return -1;
        }
        uint32_t want = c->req_need - c->req_len;
        uint32_t avail = len - off;
        uint32_t take = want < avail ? want : avail;
        if (c->req_len + take > X11_REQ_BUF)
            return -1;
        memcpy(c->req + c->req_len, data + off, take);
        c->req_len += take;
        off += take;
        if (c->req_len == c->req_need) {
            c->seq = (uint32_t)(c->seq + 1) & 0xFFFFu;
            dispatch(c, c->req, c->req_need);
            c->req_len = 0;
            c->req_need = 0;
        }
    }
    return (int)off;
}

static int owner_of(struct WL_SURFACE *s, struct X11_CONN **out,
                    struct X11_WINDOW **wout) {
    if (!s || !s->x11_owner)
        return -1;
    struct X11_CONN *c = (struct X11_CONN *)s->x11_owner;
    int i = win_idx(c, s->x11_xid);
    if (i < 0)
        return -1;
    *out = c;
    *wout = &c->win[i];
    return 0;
}

void x11_notify_key(struct WL_SURFACE *s, int keycode, int pressed, int mods) {
    struct X11_CONN *c;
    struct X11_WINDOW *w;
    if (owner_of(s, &c, &w) != 0)
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)(pressed ? X11_EV_KEY_PRESS : X11_EV_KEY_RELEASE);
    ev[1] = (uint8_t)(keycode & 0xFF);
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 8, X11_ROOT_WINDOW);
    wr32(ev + 12, w->xid);
    wr16(ev + 20, (uint16_t)comp_pointer_x());
    wr16(ev + 22, (uint16_t)comp_pointer_y());
    wr16(ev + 24, (uint16_t)comp_pointer_x());
    wr16(ev + 26, (uint16_t)comp_pointer_y());
    wr16(ev + 28, (uint16_t)((mods & MOD_SHIFT) ? 0x0001 : 0));
    post_event_to(c, w, ev, pressed ? 0x00000001u : 0x00000002u);
}

void x11_notify_button(struct WL_SURFACE *s, int x, int y, int button,
                       int pressed) {
    struct X11_CONN *c;
    struct X11_WINDOW *w;
    if (owner_of(s, &c, &w) != 0)
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)(pressed ? X11_EV_BUTTON_PRESS : X11_EV_BUTTON_RELEASE);
    ev[1] = (uint8_t)(button & 0xFF);
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 8, X11_ROOT_WINDOW);
    wr32(ev + 12, w->xid);
    wr16(ev + 20, (uint16_t)x);
    wr16(ev + 22, (uint16_t)y);
    wr16(ev + 24, (uint16_t)comp_pointer_x());
    wr16(ev + 26, (uint16_t)comp_pointer_y());
    post_event_to(c, w, ev, pressed ? 0x00000004u : 0x00000008u);
}

void x11_notify_motion(struct WL_SURFACE *s, int x, int y) {
    struct X11_CONN *c;
    struct X11_WINDOW *w;
    if (owner_of(s, &c, &w) != 0)
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = X11_EV_MOTION_NOTIFY;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 8, X11_ROOT_WINDOW);
    wr32(ev + 12, w->xid);
    wr16(ev + 20, (uint16_t)x);
    wr16(ev + 22, (uint16_t)y);
    wr16(ev + 24, (uint16_t)comp_pointer_x());
    wr16(ev + 26, (uint16_t)comp_pointer_y());
    post_event_to(c, w, ev, 0x00000040u);
}
