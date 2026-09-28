#include "kernel/syscall/win32.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/tty.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/str/str.h"

struct WIN_OUT {
    char buf[WIN_IO_BUF];
    uint32_t len;
    uint32_t total;
};

struct WIN_VA {
    struct ARCH_REGS *regs;
    int next;
};

typedef int64_t (*win_fn)(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);

struct WIN_API {
    const char *name;
    win_fn fn;
};

static uint32_t win_thunk_base;

static void win_out_flush(struct WIN_OUT *o) {
    if (o->len != 0) {
        TTY.write(o->buf, o->len);
        o->total += o->len;
        o->len = 0;
    }
}

static void win_out_raw(struct WIN_OUT *o, const char *s, uint32_t n) {
    while (n != 0) {
        uint32_t room = WIN_IO_BUF - o->len;
        uint32_t take;
        if (room == 0) {
            win_out_flush(o);
            room = WIN_IO_BUF;
        }
        take = (n < room) ? n : room;
        memcpy(o->buf + o->len, s, take);
        o->len += take;
        s += take;
        n -= take;
    }
}

static void win_out_pad(struct WIN_OUT *o, char c, uint32_t n) {
    while (n != 0) {
        win_out_raw(o, &c, 1);
        n--;
    }
}

static uint32_t win_str_len(const char *s, uint32_t cap) {
    uint32_t done = 0;
    while (done < cap) {
        uint32_t n;
        if (!access_ok(s + done, 1, 0))
            return done;
        n = (uint32_t)user_strnlen(s + done, WIN_STR_CHUNK);
        done += n;
        if (n < WIN_STR_CHUNK)
            return done;
    }
    return done;
}

static void win_out_str(struct WIN_OUT *o, const char *s) {
    uint32_t done = 0;
    while (done < WIN_STR_MAX) {
        uint32_t n;
        if (!access_ok(s + done, 1, 0))
            return;
        n = (uint32_t)user_strnlen(s + done, WIN_STR_CHUNK);
        win_out_raw(o, s + done, n);
        if (n < WIN_STR_CHUNK)
            return;
        done += n;
    }
}

static uint32_t win_num(char *dst, uint64_t v, uint32_t base, int upper) {
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *dig = upper ? upper_digits : lower_digits;
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    while (v != 0) {
        tmp[n++] = dig[v % base];
        v /= base;
    }
    for (uint32_t i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    return n;
}

static void win_out_field(struct WIN_OUT *o, const char *s, uint32_t n,
                          uint32_t width, int left, int zero) {
    uint32_t pad = (width > n) ? width - n : 0;
    if (!left)
        win_out_pad(o, zero ? '0' : ' ', pad);
    win_out_raw(o, s, n);
    if (left)
        win_out_pad(o, ' ', pad);
}

static int32_t win_va_next(struct WIN_VA *va, uint64_t *out) {
    int i = va->next++;
    uint32_t addr;
    if (i == 0) {
        *out = va->regs->rdx;
        return 0;
    }
    if (i == 1) {
        *out = va->regs->r8;
        return 0;
    }
    if (i == 2) {
        *out = va->regs->r9;
        return 0;
    }
    addr = (uint32_t)va->regs->user_rsp + WIN_STACK_BASE +
           (uint32_t)(i - 3) * 8u;
    if (!access_ok((const void *)(uintptr_t)addr, 8, 0))
        return -1;
    *out = *(const uint64_t *)(uintptr_t)addr;
    return 0;
}

static uint32_t win_vprintf(struct ARCH_REGS *r, const char *fmt) {
    char kfmt[WIN_FMT_MAX];
    char tmp[32];
    struct WIN_OUT out;
    struct WIN_VA va;
    uint32_t flen;
    uint32_t i = 0;

    flen = (uint32_t)user_strnlen(fmt, WIN_FMT_MAX - 1);
    if (!access_ok(fmt, flen + 1, 0))
        return 0;
    copy_from_user(kfmt, fmt, flen);
    kfmt[flen] = 0;
    out.len = 0;
    out.total = 0;
    va.regs = r;
    va.next = 0;

    while (kfmt[i] != 0) {
        char conv;
        int left = 0;
        int zero = 0;
        int wide = 0;
        int neg = 0;
        uint32_t width = 0;
        uint32_t n = 0;
        uint64_t v = 0;

        if (kfmt[i] != '%') {
            win_out_raw(&out, &kfmt[i], 1);
            i++;
            continue;
        }
        i++;
        if (kfmt[i] == '%') {
            win_out_raw(&out, "%", 1);
            i++;
            continue;
        }
        for (;;) {
            if (kfmt[i] == '-') {
                left = 1;
                i++;
                continue;
            }
            if (kfmt[i] == '0') {
                zero = 1;
                i++;
                continue;
            }
            if (kfmt[i] == '+' || kfmt[i] == ' ' || kfmt[i] == '#') {
                i++;
                continue;
            }
            break;
        }
        while (kfmt[i] >= '0' && kfmt[i] <= '9') {
            width = width * 10u + (uint32_t)(kfmt[i] - '0');
            i++;
        }
        while (kfmt[i] == 'l' || kfmt[i] == 'h' || kfmt[i] == 'z' ||
               kfmt[i] == 'j' || kfmt[i] == 't') {
            if (kfmt[i] == 'l')
                wide = 1;
            i++;
        }
        conv = kfmt[i];
        if (conv == 0)
            break;
        i++;
        if (win_va_next(&va, &v) != 0)
            break;
        switch (conv) {
        case 's': {
            const char *s = (const char *)(uintptr_t)v;
            uint32_t slen = win_str_len(s, WIN_STR_MAX);
            uint32_t pad = (width > slen) ? width - slen : 0;
            if (!left)
                win_out_pad(&out, ' ', pad);
            win_out_str(&out, s);
            if (left)
                win_out_pad(&out, ' ', pad);
            continue;
        }
        case 'd':
        case 'i': {
            int64_t sv = wide ? (int64_t)v : (int64_t)(int32_t)(uint32_t)v;
            uint64_t mag = (sv < 0) ? (uint64_t)(-sv) : (uint64_t)sv;
            if (sv < 0) {
                neg = 1;
                tmp[n++] = '-';
            }
            n += win_num(tmp + n, mag, 10, 0);
            break;
        }
        case 'u':
            n = win_num(tmp, wide ? v : (uint32_t)v, 10, 0);
            break;
        case 'x':
            n = win_num(tmp, wide ? v : (uint32_t)v, 16, 0);
            break;
        case 'X':
            n = win_num(tmp, wide ? v : (uint32_t)v, 16, 1);
            break;
        case 'p':
            tmp[0] = '0';
            tmp[1] = 'x';
            n = 2 + win_num(tmp + 2, v, 16, 1);
            break;
        case 'c':
            tmp[0] = (char)(uint32_t)v;
            n = 1;
            break;
        default:
            tmp[0] = '%';
            tmp[1] = conv;
            n = 2;
            break;
        }
        win_out_field(&out, tmp, n, width, left, zero && !neg);
    }
    win_out_flush(&out);
    return out.total;
}

static int32_t win_user_name(char *dst, uint32_t cap, uint64_t uptr) {
    uint32_t len;
    if (uptr == 0)
        return -1;
    len = (uint32_t)user_strnlen((const char *)(uintptr_t)uptr, cap - 1);
    if (len == 0 || !access_ok((const void *)(uintptr_t)uptr, len + 1, 0))
        return -1;
    copy_from_user(dst, (const void *)(uintptr_t)uptr, len);
    dst[len] = 0;
    return 0;
}

static int64_t win_null(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_write_file(struct ARCH_REGS *r, uint64_t handle,
                              uint64_t buf, uint64_t n, uint64_t written) {
    (void)r;
    (void)handle;
    if (n != 0 && !access_ok((const void *)(uintptr_t)buf, (size_t)n, 0))
        return 0;
    uint32_t w = (uint32_t)TTY.write((const char *)(uintptr_t)buf, (uint32_t)n);
    if (written != 0 && access_ok((const void *)(uintptr_t)written, 4, 1))
        *(uint32_t *)(uintptr_t)written = w;
    return 1;
}

static int64_t win_get_std_handle(struct ARCH_REGS *r, uint64_t which,
                                  uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(int32_t)(uint32_t)which;
}

static int64_t win_exit_process(struct ARCH_REGS *r, uint64_t code, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    sys_exit((int32_t)code);
    return 0;
}

static int64_t win_get_last_error(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_set_last_error(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_sleep(struct ARCH_REGS *r, uint64_t ms, uint64_t a1,
                         uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    mtime_sleep_interruptible((uint32_t)ms);
    return 0;
}

static int64_t win_get_tick_count(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)((uint64_t)tick * 1000u / PIT_HZ);
}

static int64_t win_get_module_handle_a(struct ARCH_REGS *r, uint64_t name,
                                       uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)r;
    (void)name;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(uint64_t)current->win_base;
}

static int64_t win_get_proc_address(struct ARCH_REGS *r, uint64_t mod,
                                    uint64_t name, uint64_t a2, uint64_t a3) {
    char kname[WIN_NAME_MAX];
    (void)r;
    (void)mod;
    (void)a2;
    (void)a3;
    if (win_user_name(kname, sizeof(kname), name) != 0)
        return 0;
    return (int64_t)(uint64_t)win32_lookup(kname);
}

static int64_t win_load_library_a(struct ARCH_REGS *r, uint64_t name,
                                  uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)r;
    (void)name;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(uint64_t)current->win_base;
}

static int64_t win_free_library(struct ARCH_REGS *r, uint64_t mod, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    (void)r;
    (void)mod;
    (void)a1;
    (void)a2;
    (void)a3;
    return 1;
}

static int64_t win_virtual_protect(struct ARCH_REGS *r, uint64_t addr,
                                   uint64_t size, uint64_t prot,
                                   uint64_t old) {
    (void)r;
    (void)addr;
    (void)size;
    (void)prot;
    if (old != 0 && access_ok((const void *)(uintptr_t)old, 4, 1))
        *(uint32_t *)(uintptr_t)old = WIN_PAGE_EXECUTE_READWRITE;
    return 1;
}

static int64_t win_virtual_query(struct ARCH_REGS *r, uint64_t addr,
                                 uint64_t buf, uint64_t len, uint64_t a3) {
    (void)r;
    (void)addr;
    (void)a3;
    if (len != 0 && access_ok((const void *)(uintptr_t)buf, (size_t)len, 1)) {
        memset((void *)(uintptr_t)buf, 0, (size_t)len);
        if (len >= 0x20u)
            *(uint64_t *)(uintptr_t)(buf + 0x18u) = len;
    }
    return WIN_MEMORY_BASIC_SIZE;
}

static int64_t win_tls_get_value(struct ARCH_REGS *r, uint64_t idx, uint64_t a1,
                                 uint64_t a2, uint64_t a3) {
    (void)r;
    (void)idx;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_tls_set_value(struct ARCH_REGS *r, uint64_t idx, uint64_t v,
                                 uint64_t a2, uint64_t a3) {
    (void)r;
    (void)idx;
    (void)v;
    (void)a2;
    (void)a3;
    return 1;
}

static int64_t win_section_noop(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_puts(struct ARCH_REGS *r, uint64_t s, uint64_t a1,
                        uint64_t a2, uint64_t a3) {
    struct WIN_OUT out;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    out.len = 0;
    out.total = 0;
    win_out_str(&out, (const char *)(uintptr_t)s);
    win_out_raw(&out, "\n", 1);
    win_out_flush(&out);
    return 0;
}

static int64_t win_printf(struct ARCH_REGS *r, uint64_t fmt, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_vprintf(r, (const char *)(uintptr_t)fmt);
}

static int64_t win_strlen(struct ARCH_REGS *r, uint64_t s, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(uint64_t)win_str_len((const char *)(uintptr_t)s, 0x10000u);
}

static int64_t win_strcmp(struct ARCH_REGS *r, uint64_t a, uint64_t b,
                          uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a2;
    (void)a3;
    for (uint32_t i = 0; i < WIN_STR_MAX; i++) {
        uint8_t ca;
        uint8_t cb;
        if (!access_ok((const void *)(uintptr_t)(a + i), 1, 0) ||
            !access_ok((const void *)(uintptr_t)(b + i), 1, 0))
            return 0;
        ca = *(const uint8_t *)(uintptr_t)(a + i);
        cb = *(const uint8_t *)(uintptr_t)(b + i);
        if (ca != cb)
            return (int64_t)(int32_t)(ca - cb);
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int64_t win_strncmp(struct ARCH_REGS *r, uint64_t a, uint64_t b,
                           uint64_t n, uint64_t a3) {
    (void)r;
    (void)a3;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t ca;
        uint8_t cb;
        if (!access_ok((const void *)(uintptr_t)(a + i), 1, 0) ||
            !access_ok((const void *)(uintptr_t)(b + i), 1, 0))
            return 0;
        ca = *(const uint8_t *)(uintptr_t)(a + i);
        cb = *(const uint8_t *)(uintptr_t)(b + i);
        if (ca != cb)
            return (int64_t)(int32_t)(ca - cb);
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int64_t win_memcpy(struct ARCH_REGS *r, uint64_t dst, uint64_t src,
                          uint64_t n, uint64_t a3) {
    (void)r;
    (void)a3;
    if (n != 0 && access_ok((const void *)(uintptr_t)dst, (size_t)n, 1) &&
        access_ok((const void *)(uintptr_t)src, (size_t)n, 0))
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
    return (int64_t)dst;
}

static int64_t win_memset(struct ARCH_REGS *r, uint64_t dst, uint64_t c,
                          uint64_t n, uint64_t a3) {
    (void)r;
    (void)a3;
    if (n != 0 && access_ok((const void *)(uintptr_t)dst, (size_t)n, 1))
        memset((void *)(uintptr_t)dst, (int32_t)c, (size_t)n);
    return (int64_t)dst;
}

static const struct WIN_API win_api_table[] = {
    {"WriteFile", win_write_file},
    {"GetStdHandle", win_get_std_handle},
    {"ExitProcess", win_exit_process},
    {"GetLastError", win_get_last_error},
    {"SetLastError", win_set_last_error},
    {"Sleep", win_sleep},
    {"GetTickCount", win_get_tick_count},
    {"GetModuleHandleA", win_get_module_handle_a},
    {"GetProcAddress", win_get_proc_address},
    {"LoadLibraryA", win_load_library_a},
    {"FreeLibrary", win_free_library},
    {"VirtualProtect", win_virtual_protect},
    {"VirtualQuery", win_virtual_query},
    {"TlsGetValue", win_tls_get_value},
    {"TlsSetValue", win_tls_set_value},
    {"InitializeCriticalSection", win_section_noop},
    {"EnterCriticalSection", win_section_noop},
    {"LeaveCriticalSection", win_section_noop},
    {"DeleteCriticalSection", win_section_noop},
    {"SetUnhandledExceptionFilter", win_section_noop},
    {"puts", win_puts},
    {"printf", win_printf},
    {"strlen", win_strlen},
    {"strcmp", win_strcmp},
    {"strncmp", win_strncmp},
    {"memcpy", win_memcpy},
    {"memset", win_memset},
    {"exit", win_exit_process},
    {"_exit", win_exit_process},
    {"abort", win_exit_process},
    {"", win_null},
};

#define WIN_API_COUNT ((uint32_t)(sizeof(win_api_table) / sizeof(win_api_table[0])))

_Static_assert(WIN_API_COUNT <= WIN_THUNK_SLOTS,
               "win32 api table exceeds thunk page");

void win32_thunk_init(uint32_t base) {
    win_thunk_base = base;
    for (uint32_t i = 0; i < WIN_API_COUNT; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)(base + i * WIN_THUNK_SIZE);
        uint32_t nr = WIN32_SYSCALL_BASE + i;
        p[0] = 0x49;
        p[1] = 0x89;
        p[2] = 0xca;
        p[3] = 0xb8;
        p[4] = (uint8_t)(nr & 0xffu);
        p[5] = (uint8_t)((nr >> 8) & 0xffu);
        p[6] = (uint8_t)((nr >> 16) & 0xffu);
        p[7] = (uint8_t)((nr >> 24) & 0xffu);
        p[8] = 0x0f;
        p[9] = 0x05;
        p[10] = 0xc3;
        for (uint32_t k = 11; k < WIN_THUNK_SIZE; k++)
            p[k] = 0xcc;
    }
}

uint32_t win32_lookup(const char *name) {
    for (uint32_t i = 0; i < WIN_API_COUNT; i++) {
        if (strcmp(name, win_api_table[i].name) == 0)
            return win_thunk_base + i * WIN_THUNK_SIZE;
    }
    return 0;
}

uint32_t win32_resolve(const char *name) {
    uint32_t t = win32_lookup(name);
    if (t != 0)
        return t;
    kprintf("[pe] unresolved import %s\n", name);
    return win_thunk_base + (WIN_API_COUNT - 1) * WIN_THUNK_SIZE;
}

int64_t win32_handler(struct ARCH_REGS *r) {
    uint32_t idx = (uint32_t)r->eax - WIN32_SYSCALL_BASE;
    if (idx >= WIN_API_COUNT)
        return 0;
    return win_api_table[idx].fn(r, r->r10, r->rdx, r->r8, r->r9);
}