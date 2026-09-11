#include "drivers/char/console/io.h"

#include <stdarg.h>

#include "kernel/asmFunc.h"

static uint8_t *vram = (uint8_t *)0;
static int scrnx = 0;
static int scrny = 0;
static int pitch = 0;
static int bpp = 8;
static int bpp_bytes = 1;
static size_t vram_bytes = 0;
static int cursor_x = 0;
static int cursor_y = -20;
static uint32_t text_color = 0;
static int gui_active = 0;

static const uint32_t ansi16[16] = {
    0xFF000000u, 0xFF0000AAu, 0xFF00AA00u, 0xFF00AAAAu, 0xFFAA0000u,
    0xFFAA00AAu, 0xFFAA5500u, 0xFFAAAAAAu, 0xFF555555u, 0xFF5555FFu,
    0xFF55FF55u, 0xFF55FFFFu, 0xFFFF5555u, 0xFFFF55FFu, 0xFFFFFF55u,
    0xFFFFFFFFu};
uint32_t io_ansi_color(int idx) {
    return ansi16[idx & 15];
}
uint8_t *io_get_vram(void) {
    return vram;
}
int io_get_scrnx(void) {
    return scrnx;
}
int io_get_scrny(void) {
    return scrny;
}
int io_get_pitch(void) {
    return pitch;
}
int io_get_bpp(void) {
    return bpp;
}
size_t io_get_vram_bytes(void) {
    return vram_bytes;
}

void io_set_gui_active(int on) {
    gui_active = on;
    if (!on) {
        cursor_x = 0;
        cursor_y = 0;
    }
}

#define PRINTF_LINE_GAP 20

void io_init(uint8_t *vram_base, int width, int height, uint32_t bytes,
             int fb_pitch, int fb_bpp) {
    vram = vram_base;
    scrnx = width;
    scrny = height;
    bpp = (fb_bpp > 0) ? fb_bpp : 8;
    bpp_bytes = bpp / 8;
    if (bpp_bytes < 1)
        bpp_bytes = 1;
    pitch = (fb_pitch > 0) ? fb_pitch : width * bpp_bytes;
    vram_bytes = (bytes > 0) ? (size_t)bytes
                             : (size_t)pitch * (size_t)height;
    cursor_x = 0;
    cursor_y = 0;
    text_color = ansi16[7];
}

void set_text_color(int color) {
    text_color = ansi16[color & 15];
}

void set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
}

int get_cursor_x(void) {
    return cursor_x;
}
int get_cursor_y(void) {
    return cursor_y;
}

#define DEBUG_CONSOLE_PORT 0xE9

static void vram_shift_up(int line_bytes) {
    int total_bytes = scrny * pitch;
    if ((pitch & 3) == 0 && ((uintptr_t)vram & 3) == 0) {
        uint32_t *dw = (uint32_t *)vram;
        int line_dw = line_bytes / 4;
        int total_dw = total_bytes / 4;
        for (int i = line_dw; i < total_dw; i++)
            dw[i - line_dw] = dw[i];
        for (int i = total_dw - line_dw; i < total_dw; i++)
            dw[i] = 0;
    } else {
        for (int i = line_bytes; i < total_bytes; i++)
            vram[i - line_bytes] = vram[i];
        for (int i = total_bytes - line_bytes; i < total_bytes; i++)
            vram[i] = 0;
    }
}

static void vram_zero_all(void) {
    int total_bytes = scrny * pitch;
    if ((pitch & 3) == 0 && ((uintptr_t)vram & 3) == 0) {
        uint32_t *dw = (uint32_t *)vram;
        int total_dw = total_bytes / 4;
        for (int i = 0; i < total_dw; i++)
            dw[i] = 0;
    } else {
        for (int i = 0; i < total_bytes; i++)
            vram[i] = 0;
    }
}

static void scroll_screen(void) {
    if (vram == 0 || scrnx <= 0 || scrny <= 0 || pitch <= 0)
        return;
    vram_shift_up(PRINTF_LINE_GAP * pitch);
    cursor_y -= PRINTF_LINE_GAP;
}

static void putc(char c) {
    if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\n') {
        cursor_y += PRINTF_LINE_GAP;
        cursor_x = 0;
        if (cursor_y + PRINTF_LINE_GAP > scrny) {
            scroll_screen();
        }
    } else {
        show_char(vram, pitch, cursor_x, cursor_y, scrnx, scrny, c, text_color,
                  0);
        cursor_x += 8;

        if (cursor_x + 8 > scrnx) {
            cursor_y += PRINTF_LINE_GAP;
            cursor_x = 0;
            if (cursor_y + PRINTF_LINE_GAP > scrny) {
                scroll_screen();
            }
        }
    }
    outb(DEBUG_CONSOLE_PORT, (uint8_t)c);
}

static void print_unsigned(uint32_t v, int base, int upper, int width, int pad0,
                           int hexPrefix) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    char buf[33];
    int n = 0;

    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v) {
            buf[n++] = digits[v % base];
            v /= base;
        }
    }

    int body = (hexPrefix ? 2 : 0) + n;
    int pad = (width > body) ? (width - body) : 0;

    if (pad0) {
        if (hexPrefix) {
            putc('0');
            putc(upper ? 'X' : 'x');
        }
        while (pad--)
            putc('0');
    } else {
        while (pad--)
            putc(' ');
        if (hexPrefix) {
            putc('0');
            putc(upper ? 'X' : 'x');
        }
    }
    while (n--)
        putc(buf[n]);
}

static void print_signed(int v, int width, int pad0) {
    unsigned int uv = (unsigned int)v;
    int neg = 0;
    char buf[12];
    int n = 0;

    if (v < 0) {
        neg = 1;
        uv = 0u - uv;
    }
    if (uv == 0) {
        buf[n++] = '0';
    } else {
        while (uv) {
            buf[n++] = '0' + uv % 10;
            uv /= 10;
        }
    }

    int body = neg + n;
    int pad = (width > body) ? (width - body) : 0;

    if (pad0) {
        if (neg)
            putc('-');
        while (pad--)
            putc('0');
    } else {
        while (pad--)
            putc(' ');
        if (neg)
            putc('-');
    }
    while (n--)
        putc(buf[n]);
}


void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; ++fmt) {
        if (*fmt != '%') {
            putc(*fmt);
            continue;
        }
        ++fmt;

        int pad0 = 0, hex_pre = 0, width = 0;
        for (;;) {
            if (*fmt == '0') {
                pad0 = 1;
                ++fmt;
            } else if (*fmt == '#') {
                hex_pre = 1;
                ++fmt;
            } else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        if (*fmt == 'l')
            ++fmt;

        switch (*fmt) {
        case 'd':
            print_signed(va_arg(ap, int), width, pad0);
            break;
        case 'u':
            print_unsigned(va_arg(ap, unsigned int), 10, 0, width, pad0, 0);
            break;
        case 'x':
            print_unsigned(va_arg(ap, unsigned int), 16, 0, width, pad0,
                           hex_pre);
            break;
        case 'X':
            print_unsigned(va_arg(ap, unsigned int), 16, 1, width, pad0,
                           hex_pre);
            break;
        case 'o':
            print_unsigned(va_arg(ap, unsigned int), 8, 0, width, pad0, 0);
            break;
        case 'c':
            putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                putc(*s++);
            break;
        }
        case '%':
            putc('%');
            break;
        case '\0':
            --fmt;
            break;
        default:
            putc('%');
            putc(*fmt);
            break;
        }
    }
    va_end(ap);
}

void console_putc(char c) {
    putc(c);
}


void io_clear_screen(void) {
    vram_zero_all();
    cursor_x = 0;
    cursor_y = 0;
    text_color = ansi16[7];
}

static inline void store_px(uint8_t *p, uint32_t color) {
    if (bpp_bytes == 4) {
        *(uint32_t *)p = color;
    } else if (bpp_bytes == 2) {
        *(uint16_t *)p = (uint16_t)color;
    } else if (bpp_bytes == 3) {
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
    } else {
        *p = (uint8_t)color;
    }
}
void show_char(uint8_t *vram_ptr, int p, int x, int y, int sw, int sh, char c,
               uint32_t color, int bg) {
    if (!vram_ptr)
        return;
    const uint8_t *font = FONT_BASE + ((uint8_t)c) * 16;
    uint32_t bgc = (bg < 0) ? 0 : (uint32_t)bg;

    for (int row = 0; row < 16; row++) {
        uint8_t bits = font[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || px >= sw || py < 0 || py >= sh)
                continue;
            uint8_t *dst = vram_ptr + (size_t)py * (size_t)p +
                           (size_t)px * (size_t)bpp_bytes;
            if (bits & (0x80 >> col))
                store_px(dst, color);
            else if (bg >= 0)
                store_px(dst, bgc);
        }
    }
}

void show_string(uint8_t *vram_ptr, int p, int x, int y, int sw, int sh,
                 const char *s, uint32_t color, int bg) {
    while (*s) {
        show_char(vram_ptr, p, x, y, sw, sh, *s, color, bg);
        x += 8;
        s++;
    }
}
