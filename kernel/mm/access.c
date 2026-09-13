#include "kernel/mm/access.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"
#define USER_VADDR_BEGIN 0x8048000u
static int user_range_walk(uint32_t addr, uint32_t len, int write);
int access_ok(const void *addr, size_t n, int write) {
    if (n == 0) {
        return 1;
    }
    uint32_t a = (uint32_t)(uintptr_t)addr;
    if (a < USER_VADDR_BEGIN || a >= USER_SPACE_END ||
        n > (size_t)(USER_SPACE_END - a)) {
        return 0;
    }
    return user_range_walk(a, (uint32_t)n, write);
}

static int user_page_readable(uint32_t a) {
    uint64_t *pde = pde_ptr(a);
    if (pde == NULL || !(*pde & PTE_P)) {
        return 0;
    }
    if (*pde & PTE_PS) {
        return (*pde & PTE_U) != 0;
    }
    uint64_t *pte = pte_ptr(a);
    return (*pte & PTE_P) && (*pte & PTE_U);
}

static int user_str_span(const char *src, uint32_t max, char *dst) {
    if (src == NULL || max == 0) {
        return -1;
    }
    uint32_t a = (uint32_t)(uintptr_t)src;
    if (a < USER_VADDR_BEGIN || a >= USER_SPACE_END) {
        return -1;
    }
    uint32_t off = 0;
    while (off < max) {
        uint32_t va = a + off;
        if (!user_page_readable(va)) {
            return -1;
        }
        uint32_t room = PAGE_SIZE - (va & 0xFFFu);
        uint32_t n = (room < max - off) ? room : (max - off);
        const char *s = (const char *)(uintptr_t)va;
        for (uint32_t i = 0; i < n; i++) {
            char c = s[i];
            if (dst) {
                dst[off + i] = c;
            }
            if (c == 0) {
                return (int)(off + i);
            }
        }
        off += n;
    }
    return -1;
}

int copy_str_from_user(char *dst, const char *src, uint32_t max) {
    return user_str_span(src, max, dst) < 0 ? -1 : 0;
}

int user_strnlen(const char *src, uint32_t max) {
    return user_str_span(src, max, NULL);
}

static int user_range_walk(uint32_t addr, uint32_t len, int write) {
    if (addr < USER_VADDR_BEGIN || len == 0 ||
        len > USER_SPACE_END - USER_VADDR_BEGIN ||
        addr > USER_SPACE_END - len) {
        return 0;
    }
    uint32_t first = addr & ~0xFFFu;
    uint32_t last = (addr + len - 1) & ~0xFFFu;
    for (uint32_t page = first;; page += PAGE_SIZE) {
        uint64_t *pde = pde_ptr(page);
        if (pde == NULL || !(*pde & PTE_P)) {
            return 0;
        }
        if (*pde & PTE_PS) {
            uint64_t need = PTE_U | (write ? PTE_W : 0);
            if ((*pde & need) != need) {
                return 0;
            }
        } else {
            uint64_t *pte = pte_ptr(page);
            if (!(*pte & PTE_P) || !(*pte & PTE_U)) {
                return 0;
            }
            if (write && !(*pte & PTE_W) &&
                (!(*pte & COW_FLAG) || !page_cow_resolve(page, *pte))) {
                return 0;
            }
        }
        if (page == last) {
            return 1;
        }
    }
}

int user_range_readable(uint32_t addr, uint32_t len) {
    return user_range_walk(addr, len, 0);
}

int user_range_writable(uint32_t addr, uint32_t len) {
    return user_range_walk(addr, len, 1);
}
