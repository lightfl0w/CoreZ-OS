#include "kernel/gui/clients.h"

#include "drivers/char/keyboard.h"
#include "arch/x86/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/sched/thread.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/server.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/theme.h"
#include "kernel/gui/wm.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/dir.h"
#include "kernel/mm/pool/pool.h"
#include "lib/png/png.h"

struct COMP_DEMO_CLIENT {
    struct WL_CLIENT *conn;
    struct WL_SURFACE *surf;
    struct WL_SHM_POOL *pool;
    int w, h;
    uint32_t frame_interval;
    uint32_t last_frame;
    int last_dark;
    void (*render)(struct COMP_DEMO_CLIENT *dc);
    void (*on_key)(struct COMP_DEMO_CLIENT *dc, int scancode, int mods);
};

extern void (*log_hook)(const char *s);

#define UI_FONT_PX 13
static void canvas_of(struct GFX_CANVAS *cv, struct COMP_DEMO_CLIENT *dc) {
    cv->pixels = (gfx_color *)dc->pool->data;
    cv->pitch = dc->w * 4;
    cv->w = dc->w;
    cv->h = dc->h;
    cv->bytes = (size_t)dc->w * (size_t)dc->h * 4u;
}

static int buffer_resize(struct COMP_DEMO_CLIENT *dc, int w, int h) {
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

static void attach_commit(struct COMP_DEMO_CLIENT *dc) {
    if (!dc->pool)
        return;
    wl_surface_attach(dc->surf, dc->pool, dc->w, dc->h);
    wl_surface_commit(dc->surf);
}

static void client_main(struct COMP_DEMO_CLIENT *dc) {
    for (;;) {
        struct WL_EVENT ev;
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
            if (dc->pool && (tick - dc->last_frame >= dc->frame_interval ||
                             dc->last_dark != theme()->dark)) {
                dc->last_frame = tick;
                dc->last_dark = theme()->dark;
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

static gfx_color term_ink(void) {
    return theme()->dark ? GFX_RGB(120, 224, 150) : GFX_RGB(26, 127, 55);
}

static void term_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    gfx_fill(&cv, 0, 0, dc->w, 2, theme()->accent);

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
                  term_ink());
    }
    if ((tick / 25) & 1) {
        gfx_fill(&cv, 4 + term_col * term_cw,
                 (term_count - first - 1) * term_lh + 2, term_cw, term_lh,
                 term_ink());
    }
}

static void term_on_key(struct COMP_DEMO_CLIENT *dc, int scancode, int mods) {
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
    struct COMP_DEMO_CLIENT dc;
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
    dc.frame_interval = 30;
    log_hook = term_log_hook;
    wm_manage(dc.surf);
    client_main(&dc);
    log_hook = 0;
}

static void clock_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);

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
    font_draw(&cv, x, y, tbuf, px, theme()->accent);

    font_draw(&cv, 8, 8, "frame-callback driven clock", UI_FONT_PX,
              theme()->muted);
    font_draw(&cv, 8, dc->h - font_line_height(UI_FONT_PX) - 6,
              "uptime since boot", UI_FONT_PX, theme()->muted);
}

static void clock_thread(void *arg) {
    (void)arg;
    struct COMP_DEMO_CLIENT dc;
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
    dc.frame_interval = 33;
    wm_manage(dc.surf);
    client_main(&dc);
}

static void sysmon_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);

    font_draw(&cv, 8, 8, "sysmon - compositor stats", UI_FONT_PX, theme()->text);

    int nsurf = 0;
    comp_surfaces(&nsurf);
    char line[64];
    char num[12];

    line[0] = 0;
    strcat(line, "surfaces: ");
    u32_to_dec((uint32_t)nsurf, num);
    strcat(line, num);
    font_draw(&cv, 8, 30, line, UI_FONT_PX, theme()->muted);

    line[0] = 0;
    strcat(line, "workspace: ");
    u32_to_dec((uint32_t)(wm_current_ws() + 1), num);
    strcat(line, num);
    strcat(line, " / 4");
    font_draw(&cv, 8, 48, line, UI_FONT_PX, theme()->muted);

    line[0] = 0;
    strcat(line, "tick: ");
    u32_to_dec(tick, num);
    strcat(line, num);
    font_draw(&cv, 8, 66, line, UI_FONT_PX, theme()->muted);

    int gx = 8, gy = 90, gw = dc->w - 16, gh = dc->h - 104;
    if (gw > 8 && gh > 8) {
        gfx_rect(&cv, gx, gy, gw, gh, theme()->dim);
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
    struct COMP_DEMO_CLIENT dc;
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
    dc.frame_interval = 30;
    wm_manage(dc.surf);
    client_main(&dc);
}

static void plasma_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
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
    struct COMP_DEMO_CLIENT dc;
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
    dc.frame_interval = 20;
    wm_manage(dc.surf);
    client_main(&dc);
}

#define VIEWER_MAX_FILES 3
static const char *viewer_files[VIEWER_MAX_FILES] = {"/wallpaper.png",
                                                     "/pic1.png",
                                                     "/pic2.png"};

struct PNG_VIEWER {
    struct COMP_DEMO_CLIENT dc;
    struct PNG_IMAGE img;
    int idx;
};

static int viewer_load(struct PNG_VIEWER *v, int idx) {
    const char *path = viewer_files[idx];
    struct FS_STAT st;
    if (sys_stat(path, &st) != 0 || st.st_size == 0 ||
        st.st_size > (8u * 1024u * 1024u))
        return -1;
    int fd = open_file(path, O_RDONLY);
    if (fd < 0)
        return -1;
    uint32_t pages = (st.st_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *buf = (uint8_t *)get_kernel_pages(pages);
    if (!buf) {
        close_file(fd);
        return -1;
    }
    uint32_t got = read_file(fd, buf, st.st_size);
    close_file(fd);
    if (got != st.st_size) {
        free_kernel_page((uint32_t)(uintptr_t)buf);
        return -1;
    }
    if (v->img.pixels)
        png_image_free(&v->img);
    int rc = png_decode(buf, got, &v->img);
    free_kernel_page((uint32_t)(uintptr_t)buf);
    if (rc != PNG_OK)
        return -1;
    v->idx = idx;
    return 0;
}

static void png_viewer_render(struct COMP_DEMO_CLIENT *dc) {
    struct PNG_VIEWER *v = (struct PNG_VIEWER *)dc;
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    int footer = 20;
    int avail = dc->h - footer;
    if (avail < 8)
        return;
    if (v->img.pixels && v->img.w > 0 && v->img.h > 0) {
        struct GFX_CANVAS src;
        src.pixels = v->img.pixels;
        src.pitch = v->img.w * 4;
        src.w = v->img.w;
        src.h = v->img.h;
        src.bytes = (size_t)v->img.w * (size_t)v->img.h * 4u;
        int dw = dc->w, dh = avail;
        if ((int64_t)src.w * dh > (int64_t)src.h * dw)
            dh = (int)((int64_t)src.h * dw / src.w);
        else
            dw = (int)((int64_t)src.w * dh / src.h);
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        gfx_blit_scale(&cv, (dc->w - dw) / 2, (avail - dh) / 2, dw, dh, &src,
                       0, 0, src.w, src.h);
    } else {
        font_draw(&cv, 8, 8, "png view: no image", UI_FONT_PX,
                  theme()->muted);
    }
    char line[96];
    char num[12];
    const char *name = viewer_files[v->idx];
    while (*name == '/')
        name++;
    line[0] = 0;
    strcat(line, name);
    strcat(line, "  ");
    u32_to_dec((uint32_t)v->img.w, num);
    strcat(line, num);
    strcat(line, "x");
    u32_to_dec((uint32_t)v->img.h, num);
    strcat(line, num);
    strcat(line, "  [");
    u32_to_dec((uint32_t)(v->idx + 1), num);
    strcat(line, num);
    strcat(line, "/");
    u32_to_dec((uint32_t)VIEWER_MAX_FILES, num);
    strcat(line, num);
    strcat(line, "]  space/enter: next");
    gfx_fill(&cv, 0, avail, dc->w, footer, theme()->bar);
    font_draw(&cv, 6, avail + 4, line, UI_FONT_PX, theme()->text);
}

static void png_viewer_on_key(struct COMP_DEMO_CLIENT *dc, int scancode,
                              int mods) {
    struct PNG_VIEWER *v = (struct PNG_VIEWER *)dc;
    (void)mods;
    int next = v->idx;
    if (scancode == 0x39 || scancode == 0x1C || scancode == 0x4D)
        next = (v->idx + 1) % VIEWER_MAX_FILES;
    else if (scancode == 0x4B)
        next = (v->idx + VIEWER_MAX_FILES - 1) % VIEWER_MAX_FILES;
    else
        return;
    if (viewer_load(v, next) != 0) {
        comp_log("pngview: load failed");
        return;
    }
    if (dc->pool) {
        dc->render(dc);
        wl_surface_commit(dc->surf);
    }
}

static void png_viewer_thread(void *arg) {
    (void)arg;
    struct PNG_VIEWER viewer;
    struct PNG_VIEWER *v = &viewer;
    memset(v, 0, sizeof(*v));
    v->dc.conn = wl_display_connect("pngview");
    if (!v->dc.conn) {
        thread_exit_current();
        return;
    }
    v->dc.surf = wl_compositor_create_surface(v->dc.conn, "pngview");
    if (!v->dc.surf) {
        wl_display_disconnect(v->dc.conn);
        thread_exit_current();
        return;
    }
    v->dc.render = png_viewer_render;
    v->dc.on_key = png_viewer_on_key;
    v->dc.frame_interval = 1000000;
    if (viewer_load(v, 0) != 0)
        comp_log("pngview: initial image load failed");
    else
        comp_log("pngview: image decoded");
    wm_manage(v->dc.surf);
    client_main(&v->dc);
    if (v->img.pixels)
        png_image_free(&v->img);
}

static char sc_to_char(uint8_t sc, int shift) {
    static const char *lo1 = "1234567890-=";
    static const char *hi1 = "!@#$%^&*()_+";
    static const char *lo2 = "qwertyuiop[]";
    static const char *hi2 = "QWERTYUIOP{}";
    static const char *lo3 = "asdfghjkl;'";
    static const char *hi3 = "ASDFGHJKL:\"";
    static const char *lo4 = "zxcvbnm,./";
    static const char *hi4 = "ZXCVBNM<>?";
    int i;
    if (sc >= 0x02 && sc <= 0x0D) {
        i = sc - 0x02;
        return (shift ? hi1 : lo1)[i];
    }
    if (sc >= 0x10 && sc <= 0x1B) {
        i = sc - 0x10;
        return (shift ? hi2 : lo2)[i];
    }
    if (sc >= 0x1E && sc <= 0x28) {
        i = sc - 0x1E;
        return (shift ? hi3 : lo3)[i];
    }
    if (sc == 0x29)
        return shift ? '~' : '`';
    if (sc == 0x2B)
        return shift ? '|' : '\\';
    if (sc >= 0x2C && sc <= 0x35) {
        i = sc - 0x2C;
        return (shift ? hi4 : lo4)[i];
    }
    if (sc == 0x39)
        return ' ';
    return 0;
}

static void s_copy(char *dst, const char *src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void s_join(char *dst, const char *dir, const char *name, int cap) {
    s_copy(dst, dir, cap);
    int l = strlen(dst);
    if (l > 0 && dst[l - 1] != '/' && l < cap - 1)
        dst[l++] = '/';
    dst[l] = 0;
    s_copy(dst, strcat(dst, name), cap);
}

#define FILES_MAX 64
#define FILES_NAME_LEN 26
#define FILES_ROW_H 22

struct FILES_STATE {
    struct COMP_DEMO_CLIENT dc;
    char cwd[MAX_PATH_LEN];
    char names[FILES_MAX][FILES_NAME_LEN];
    uint8_t is_dir[FILES_MAX];
    uint32_t sizes[FILES_MAX];
    int count;
    int sel;
    int scroll;
};

static void files_load(struct FILES_STATE *fs) {
    fs->count = 0;
    fs->sel = 0;
    fs->scroll = 0;
    struct FS_DIR *d = sys_opendir(fs->cwd);
    if (!d)
        return;
    struct FS_DIRENT *e;
    while (fs->count < FILES_MAX && (e = sys_readdir(d)) != 0) {
        const char *nm = e->filename;
        if (nm[0] == '.' && nm[1] == 0)
            continue;
        int i = fs->count;
        s_copy(fs->names[i], nm, FILES_NAME_LEN);
        fs->is_dir[i] = (e->f_type == FT_DIRECTORY);
        fs->sizes[i] = 0;
        if (!fs->is_dir[i]) {
            char p[MAX_PATH_LEN];
            s_join(p, fs->cwd, fs->names[i], MAX_PATH_LEN);
            struct FS_STAT st;
            if (sys_stat(p, &st) == 0)
                fs->sizes[i] = st.st_size;
        }
        fs->count++;
    }
    sys_closedir(d);
}

static int files_visible(struct COMP_DEMO_CLIENT *dc) {
    int h = dc->h - 52;
    int n = h / FILES_ROW_H;
    return n < 1 ? 1 : n;
}

static void files_enter(struct FILES_STATE *fs) {
    if (fs->count == 0 || !fs->is_dir[fs->sel])
        return;
    char p[MAX_PATH_LEN];
    s_join(p, fs->cwd, fs->names[fs->sel], MAX_PATH_LEN);
    struct FS_STAT st;
    if (sys_stat(p, &st) != 0 || st.st_filetype != FT_DIRECTORY) {
        comp_log("files: open dir failed");
        return;
    }
    s_copy(fs->cwd, p, MAX_PATH_LEN);
    files_load(fs);
}

static void files_up(struct FILES_STATE *fs) {
    int l = strlen(fs->cwd);
    if (l <= 1)
        return;
    while (l > 1 && fs->cwd[l - 1] != '/')
        l--;
    if (l > 1)
        l--;
    fs->cwd[l] = 0;
    files_load(fs);
}

static void files_render(struct COMP_DEMO_CLIENT *dc) {
    struct FILES_STATE *fs = (struct FILES_STATE *)dc;
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    int vis = files_visible(dc);
    int head_h = 26;
    gfx_fill(&cv, 0, 0, dc->w, head_h, theme()->bar);
    char head[MAX_PATH_LEN + 16];
    head[0] = 0;
    strcat(head, "dir: ");
    strcat(head, fs->cwd);
    font_draw(&cv, 8, 6, head, UI_FONT_PX, theme()->title_fg_foc);
    for (int k = 0; k < vis; k++) {
        int i = fs->scroll + k;
        if (i >= fs->count)
            break;
        int ry = head_h + 4 + k * FILES_ROW_H;
        if (i == fs->sel)
            gfx_fill_round(&cv, 4, ry, dc->w - 8, FILES_ROW_H - 2, 5,
                           theme()->wp_top);
        gfx_fill_round(&cv, 8, ry + 4, 12, 12, 3,
                       fs->is_dir[i] ? theme()->accent : theme()->dim);
        font_draw(&cv, 28, ry + 3, fs->names[i], UI_FONT_PX,
                  fs->is_dir[i] ? theme()->accent : theme()->text);
        if (!fs->is_dir[i]) {
            char num[12];
            u32_to_dec(fs->sizes[i], num);
            char sz[24];
            sz[0] = 0;
            strcat(sz, num);
            strcat(sz, " B");
            int sw = font_text_width(sz, UI_FONT_PX);
            font_draw(&cv, dc->w - sw - 14, ry + 3, sz, UI_FONT_PX,
                      theme()->muted);
        }
    }
    int fy = dc->h - 22;
    gfx_fill(&cv, 0, fy, dc->w, 22, theme()->bar);
    char foot[MAX_PATH_LEN + 16];
    foot[0] = 0;
    if (fs->count > 0) {
        strcat(foot, fs->names[fs->sel]);
        if (fs->is_dir[fs->sel])
            strcat(foot, "/  Enter: open  BackSpace: up");
        else
            strcat(foot, "  Enter: info  BackSpace: up");
    } else {
        strcat(foot, "empty  BackSpace: up");
    }
    font_draw(&cv, 6, fy + 4, foot, UI_FONT_PX, theme()->muted);
}

static void files_on_key(struct COMP_DEMO_CLIENT *dc, int sc, int mods) {
    struct FILES_STATE *fs = (struct FILES_STATE *)dc;
    (void)mods;
    int vis = files_visible(dc);
    if (sc == 0x48 && fs->sel > 0)
        fs->sel--;
    else if (sc == 0x50 && fs->sel < fs->count - 1)
        fs->sel++;
    else if (sc == 0x4B)
        fs->sel -= vis;
    else if (sc == 0x4D)
        fs->sel += vis;
    else if (sc == 0x1C)
        files_enter(fs);
    else if (sc == 0x0E)
        files_up(fs);
    else
        return;
    if (fs->sel >= fs->count)
        fs->sel = fs->count - 1;
    if (fs->sel < 0)
        fs->sel = 0;
    if (fs->sel < fs->scroll)
        fs->scroll = fs->sel;
    if (fs->sel >= fs->scroll + vis)
        fs->scroll = fs->sel - vis + 1;
    dc->render(dc);
    wl_surface_commit(dc->surf);
}

static void files_thread(void *arg) {
    (void)arg;
    struct FILES_STATE fs;
    memset(&fs, 0, sizeof(fs));
    fs.dc.conn = wl_display_connect("files");
    if (!fs.dc.conn) {
        thread_exit_current();
        return;
    }
    fs.dc.surf = wl_compositor_create_surface(fs.dc.conn, "files");
    if (!fs.dc.surf) {
        wl_display_disconnect(fs.dc.conn);
        thread_exit_current();
        return;
    }
    s_copy(fs.cwd, "/", MAX_PATH_LEN);
    files_load(&fs);
    fs.dc.render = files_render;
    fs.dc.on_key = files_on_key;
    fs.dc.frame_interval = 1000000;
    wm_manage(fs.dc.surf);
    client_main(&fs.dc);
}

#define EDIT_ROWS 20
#define EDIT_COLS 76
#define EDIT_LINE_H 18

struct EDIT_STATE {
    struct COMP_DEMO_CLIENT dc;
    char lines[EDIT_ROWS][EDIT_COLS];
    int len[EDIT_ROWS];
    int cur_l;
    int cur_c;
    int scroll;
    int dirty;
};

static void edit_load(struct EDIT_STATE *ed) {
    struct FS_STAT st;
    if (sys_stat("/note.txt", &st) != 0 || st.st_size == 0 ||
        st.st_size > 32768)
        return;
    int fd = open_file("/note.txt", O_RDONLY);
    if (fd < 0)
        return;
    static char buf[16384];
    uint32_t got = read_file(fd, buf, st.st_size);
    close_file(fd);
    int l = 0;
    int c = 0;
    for (uint32_t i = 0; i < got; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            ed->lines[l][c] = 0;
            ed->len[l] = c;
            l++;
            c = 0;
            if (l >= EDIT_ROWS)
                break;
            continue;
        }
        if (c < EDIT_COLS - 1)
            ed->lines[l][c++] = ch;
    }
    ed->lines[l][c] = 0;
    ed->len[l] = c;
    for (int k = l + 1; k < EDIT_ROWS; k++) {
        ed->lines[k][0] = 0;
        ed->len[k] = 0;
    }
    ed->cur_l = 0;
    ed->cur_c = 0;
    ed->scroll = 0;
}

static void edit_save(struct EDIT_STATE *ed) {
    sys_unlink("/note.txt");
    create_file("/note.txt");
    int fd = open_file("/note.txt", O_WRONLY);
    if (fd < 0) {
        kprintf("edit: save open failed\n");
        return;
    }
    for (int k = 0; k < EDIT_ROWS; k++) {
        if (ed->len[k] > 0)
            write_file(fd, ed->lines[k], (uint32_t)ed->len[k]);
        write_file(fd, "\n", 1);
    }
    close_file(fd);
    ed->dirty = 0;
    kprintf("edit: saved /note.txt\n");
}

static void edit_render(struct COMP_DEMO_CLIENT *dc) {
    struct EDIT_STATE *ed = (struct EDIT_STATE *)dc;
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    int vis = (dc->h - 40) / EDIT_LINE_H;
    if (vis < 1)
        vis = 1;
    for (int k = 0; k < vis && ed->scroll + k < EDIT_ROWS; k++) {
        int l = ed->scroll + k;
        font_draw(&cv, 10, 6 + k * EDIT_LINE_H, ed->lines[l], UI_FONT_PX,
                  theme()->text);
        if (l == ed->cur_l) {
            char pre[EDIT_COLS];
            s_copy(pre, ed->lines[l], EDIT_COLS);
            if (ed->cur_c < ed->len[l])
                pre[ed->cur_c] = 0;
            int cw = font_text_width(pre, UI_FONT_PX);
            gfx_fill(&cv, 10 + cw, 6 + k * EDIT_LINE_H, 2,
                     EDIT_LINE_H - 4, theme()->accent);
        }
    }
    int fy = dc->h - 22;
    gfx_fill(&cv, 0, fy, dc->w, 22, theme()->bar);
    char foot[48];
    char num[12];
    foot[0] = 0;
    strcat(foot, "/note.txt  Ctrl+S save  Ln ");
    u32_to_dec((uint32_t)(ed->cur_l + 1), num);
    strcat(foot, num);
    strcat(foot, " Col ");
    u32_to_dec((uint32_t)(ed->cur_c + 1), num);
    strcat(foot, num);
    if (ed->dirty)
        strcat(foot, "  *");
    font_draw(&cv, 6, fy + 4, foot, UI_FONT_PX, theme()->muted);
}

static void edit_ensure_visible(struct EDIT_STATE *ed,
                                struct COMP_DEMO_CLIENT *dc) {
    int vis = (dc->h - 40) / EDIT_LINE_H;
    if (vis < 1)
        vis = 1;
    if (ed->cur_l < ed->scroll)
        ed->scroll = ed->cur_l;
    if (ed->cur_l >= ed->scroll + vis)
        ed->scroll = ed->cur_l - vis + 1;
}

static void edit_on_key(struct COMP_DEMO_CLIENT *dc, int sc, int mods) {
    struct EDIT_STATE *ed = (struct EDIT_STATE *)dc;
    if ((mods & KBD_MOD_CTRL) && !(mods & KBD_MOD_ALT) && sc == 0x1F) {
        edit_save(ed);
        dc->render(dc);
        wl_surface_commit(dc->surf);
        return;
    }
    if (sc == 0x0E) {
        if (ed->cur_c > 0) {
            ed->cur_c--;
            for (int i = ed->cur_c; i < ed->len[ed->cur_l]; i++)
                ed->lines[ed->cur_l][i] = ed->lines[ed->cur_l][i + 1];
            ed->len[ed->cur_l]--;
            ed->dirty = 1;
        } else if (ed->cur_l > 0) {
            int prev = ed->cur_l - 1;
            int room = EDIT_COLS - 1 - ed->len[prev];
            int move = ed->len[ed->cur_l] < room ? ed->len[ed->cur_l] : room;
            for (int i = 0; i < move; i++)
                ed->lines[prev][ed->len[prev] + i] = ed->lines[ed->cur_l][i];
            ed->cur_c = ed->len[prev];
            ed->len[prev] += move;
            for (int k = ed->cur_l; k < EDIT_ROWS - 1; k++) {
                s_copy(ed->lines[k], ed->lines[k + 1], EDIT_COLS);
                ed->len[k] = ed->len[k + 1];
            }
            ed->cur_l--;
            ed->dirty = 1;
        }
    } else if (sc == 0x1C) {
        if (ed->cur_l + 1 < EDIT_ROWS) {
            for (int k = EDIT_ROWS - 1; k > ed->cur_l; k--) {
                s_copy(ed->lines[k], ed->lines[k - 1], EDIT_COLS);
                ed->len[k] = ed->len[k - 1];
            }
            int tail = ed->len[ed->cur_l] - ed->cur_c;
            s_copy(ed->lines[ed->cur_l + 1], ed->lines[ed->cur_l] + ed->cur_c,
                   EDIT_COLS);
            ed->len[ed->cur_l + 1] = tail > 0 ? tail : 0;
            ed->lines[ed->cur_l][ed->cur_c] = 0;
            ed->len[ed->cur_l] = ed->cur_c;
            ed->cur_l++;
            ed->cur_c = 0;
            ed->dirty = 1;
        }
    } else if (sc == 0x48) {
        if (ed->cur_l > 0)
            ed->cur_l--;
        if (ed->cur_c > ed->len[ed->cur_l])
            ed->cur_c = ed->len[ed->cur_l];
    } else if (sc == 0x50) {
        if (ed->cur_l + 1 < EDIT_ROWS)
            ed->cur_l++;
        if (ed->cur_c > ed->len[ed->cur_l])
            ed->cur_c = ed->len[ed->cur_l];
    } else if (sc == 0x4B) {
        if (ed->cur_c > 0)
            ed->cur_c--;
    } else if (sc == 0x4D) {
        if (ed->cur_c < ed->len[ed->cur_l])
            ed->cur_c++;
    } else {
        char ch = sc_to_char(sc, mods & 1);
        if (ch && ed->len[ed->cur_l] < EDIT_COLS - 1) {
            for (int i = ed->len[ed->cur_l]; i > ed->cur_c; i--)
                ed->lines[ed->cur_l][i] = ed->lines[ed->cur_l][i - 1];
            ed->lines[ed->cur_l][ed->cur_c++] = ch;
            ed->len[ed->cur_l]++;
            ed->dirty = 1;
        } else {
            return;
        }
    }
    edit_ensure_visible(ed, dc);
    dc->render(dc);
    wl_surface_commit(dc->surf);
}

static void edit_thread(void *arg) {
    (void)arg;
    struct EDIT_STATE ed;
    memset(&ed, 0, sizeof(ed));
    ed.dc.conn = wl_display_connect("edit");
    if (!ed.dc.conn) {
        thread_exit_current();
        return;
    }
    ed.dc.surf = wl_compositor_create_surface(ed.dc.conn, "edit");
    if (!ed.dc.surf) {
        wl_display_disconnect(ed.dc.conn);
        thread_exit_current();
        return;
    }
    edit_load(&ed);
    ed.dc.render = edit_render;
    ed.dc.on_key = edit_on_key;
    ed.dc.frame_interval = 1000000;
    wm_manage(ed.dc.surf);
    client_main(&ed.dc);
}

typedef void (*client_thread_fn)(void *);

static client_thread_fn types[] = {term_thread,     clock_thread,
                                   sysmon_thread,   png_viewer_thread,
                                   plasma_thread,   files_thread,
                                   edit_thread};
static const char *type_names[] = {"gc_term", "gc_clock", "gc_sysmon",
                                   "gc_pngview", "gc_plasma", "gc_files",
                                   "gc_edit"};
#define CLIENT_TYPES ((int)(sizeof(types) / sizeof(types[0])))
static int next_type = 0;

void clients_spawn_next(void) {
    int type_idx = next_type % CLIENT_TYPES;
    next_type++;
    kernel_thread((char *)type_names[type_idx], 6, types[type_idx], 0, 0xF);
}

void clients_spawn_initial(void) {
    next_type = 0;
    for (int i = 0; i < 4; i++)
        clients_spawn_next();
}

void clients_broadcast_close(void) {
    int n = 0;
    struct WL_SURFACE **list = comp_surfaces(&n);
    for (int i = 0; i < n; i++)
        comp_send_close(list[i]);
}
