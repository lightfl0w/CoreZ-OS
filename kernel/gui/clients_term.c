#include "kernel/gui/clients_internal.h"

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
    client_repaint(dc);
}

static const struct CLIENT_DESC term_desc = {
    "term", "term - wayland-ish client", term_render, term_on_key, 30};

void clients_term_thread(void *arg) {
    (void)arg;
    struct COMP_DEMO_CLIENT dc;
    memset(&dc, 0, sizeof(dc));
    if (client_begin(&dc, &term_desc) != 0) {
        thread_exit_current();
        return;
    }
    log_hook = term_log_hook;
    client_main(&dc);
    log_hook = 0;
}
