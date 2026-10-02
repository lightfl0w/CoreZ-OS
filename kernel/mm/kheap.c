#include "kernel/mm/kheap.h"
#include "kernel/assert.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/sync.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"

#define HEAP_ALIGN          ((size_t)16)
#define HEAP_ALIGN_LOG2     4u
#define HEAP_SMALL_FL       (HEAP_ALIGN_LOG2 + HEAP_SL_LOG2)
#define HEAP_HDR            (sizeof(struct MM_HEAP_BLOCK))
#define HEAP_MIN_BLOCK      (2 * HEAP_HDR)
#define HEAP_MAX_BYTES      ((size_t)1 << 30)

#define HEAP_FREE           ((size_t)1u << 0)
#define HEAP_PREV_FREE      ((size_t)1u << 1)
#define HEAP_SIZE_MASK      (~(size_t)3)

#define KHEAP_AREA_MIN_PAGES 16u
#define KHEAP_AREA_MAX_PAGES 256u
#define KHEAP_MAX_AREAS      64u

static struct MM_HEAP kheap_ctl;
static struct MM_HEAP_AREA kheap_areas[KHEAP_MAX_AREAS];
static uint32_t kheap_area_count;
static struct SCHED_LOCK kheap_lock;
static uint32_t kheap_used_bytes;
static uint32_t kheap_peak_bytes;
static uint32_t kheap_alloc_count;

static size_t block_total(const struct MM_HEAP_BLOCK *b) {
    return b->size & HEAP_SIZE_MASK;
}

static struct MM_HEAP_BLOCK *block_next(struct MM_HEAP_BLOCK *b) {
    return (struct MM_HEAP_BLOCK *)((char *)b + block_total(b));
}

static void *block_user(struct MM_HEAP_BLOCK *b) {
    return (char *)b + HEAP_HDR;
}

static struct MM_HEAP_BLOCK *block_from(void *ptr) {
    return (struct MM_HEAP_BLOCK *)((char *)ptr - HEAP_HDR);
}

static struct MM_HEAP_FREE_LINK *free_link(struct MM_HEAP_BLOCK *b) {
    return (struct MM_HEAP_FREE_LINK *)((char *)b + HEAP_HDR);
}

static int heap_fls(size_t x) {
    return 63 - __builtin_clzll((unsigned long long)x);
}

static void map_insert(size_t total, int *fl, int *sl) {
    int f = heap_fls(total);
    *fl = f;
    *sl = (int)((total >> (f - HEAP_SL_LOG2)) & (HEAP_SL_COUNT - 1));
}

static int map_request(size_t total, int *fl, int *sl) {
    if (total >= HEAP_MAX_BYTES) {
        return -1;
    }
    map_insert(total, fl, sl);
    return 0;
}

static void freelist_push(struct MM_HEAP_BLOCK *b) {
    int fl, sl;
    map_insert(block_total(b), &fl, &sl);
    struct MM_HEAP_FREE_LINK *l = free_link(b);
    l->next = kheap_ctl.blocks[fl][sl];
    l->prev = NULL;
    if (l->next != NULL) {
        free_link(l->next)->prev = b;
    }
    kheap_ctl.blocks[fl][sl] = b;
    kheap_ctl.sl_bitmap[fl] |= 1u << sl;
    kheap_ctl.fl_bitmap |= 1u << fl;
}

static void freelist_remove(struct MM_HEAP_BLOCK *b) {
    int fl, sl;
    map_insert(block_total(b), &fl, &sl);
    struct MM_HEAP_FREE_LINK *l = free_link(b);
    if (l->prev != NULL) {
        free_link(l->prev)->next = l->next;
    } else {
        kheap_ctl.blocks[fl][sl] = l->next;
        if (l->next == NULL) {
            kheap_ctl.sl_bitmap[fl] &= ~(1u << sl);
            if (kheap_ctl.sl_bitmap[fl] == 0) {
                kheap_ctl.fl_bitmap &= ~(1u << fl);
            }
        }
    }
    if (l->next != NULL) {
        free_link(l->next)->prev = l->prev;
    }
}

static struct MM_HEAP_BLOCK *list_take(int fl0, int sl0, size_t total) {
    for (int fl = fl0; fl < (int)HEAP_FL_COUNT; sl0 = 0, fl++) {
        uint32_t sl_map = kheap_ctl.sl_bitmap[fl] & (~0u << sl0);
        while (sl_map != 0) {
            int sl = __builtin_ffs((int)sl_map) - 1;
            struct MM_HEAP_BLOCK *b = kheap_ctl.blocks[fl][sl];
            if (fl == fl0 && sl == sl0) {
                struct MM_HEAP_BLOCK *fit;
                for (fit = b; fit != NULL; fit = free_link(fit)->next) {
                    if (block_total(fit) >= total) {
                        break;
                    }
                }
                if (fit == NULL) {
                    sl_map &= ~(1u << sl);
                    continue;
                }
                b = fit;
            }
            freelist_remove(b);
            return b;
        }
    }
    return NULL;
}

static void block_release(struct MM_HEAP_BLOCK *b) {
    b->size |= HEAP_FREE;
    struct MM_HEAP_BLOCK *n = block_next(b);
    n->size |= HEAP_PREV_FREE;
    if ((n->size & HEAP_FREE) && block_total(n) != 0) {
        freelist_remove(n);
        b->size = (block_total(b) + block_total(n)) | HEAP_FREE |
                  (b->size & HEAP_PREV_FREE);
        struct MM_HEAP_BLOCK *m = block_next(b);
        m->prev = b;
    }
    if (b->size & HEAP_PREV_FREE) {
        struct MM_HEAP_BLOCK *p = b->prev;
        freelist_remove(p);
        p->size = (block_total(p) + block_total(b)) | HEAP_FREE |
                  (p->size & HEAP_PREV_FREE);
        struct MM_HEAP_BLOCK *m = block_next(p);
        m->prev = p;
        b = p;
    }
    freelist_push(b);
}

static void block_trim_used(struct MM_HEAP_BLOCK *b, size_t need) {
    if (block_total(b) < HEAP_HDR + need + HEAP_MIN_BLOCK) {
        return;
    }
    struct MM_HEAP_BLOCK *t =
        (struct MM_HEAP_BLOCK *)((char *)b + HEAP_HDR + need);
    t->size = block_total(b) - HEAP_HDR - need;
    t->prev = b;
    b->size = (HEAP_HDR + need) | (b->size & HEAP_PREV_FREE);
    struct MM_HEAP_BLOCK *m = block_next(t);
    m->prev = t;
    block_release(t);
}

static void *block_acquire(size_t payload) {
    size_t total = payload + HEAP_HDR;
    int fl, sl;
    if (map_request(total, &fl, &sl) != 0) {
        return NULL;
    }
    struct MM_HEAP_BLOCK *b = list_take(fl, sl, total);
    if (b == NULL) {
        return NULL;
    }
    b->size &= ~HEAP_FREE;
    struct MM_HEAP_BLOCK *n = block_next(b);
    n->size &= ~HEAP_PREV_FREE;
    block_trim_used(b, payload);
    return block_user(b);
}

static void kheap_area_add(void *mem, size_t bytes) {
    bytes &= ~(HEAP_ALIGN - 1);
    struct MM_HEAP_BLOCK *b = (struct MM_HEAP_BLOCK *)mem;
    b->size = bytes - HEAP_HDR;
    b->prev = NULL;
    struct MM_HEAP_BLOCK *s = block_next(b);
    s->size = 0;
    s->prev = b;
    kheap_areas[kheap_area_count].mem = mem;
    kheap_areas[kheap_area_count].bytes = bytes;
    kheap_area_count++;
    block_release(b);
}

static int kheap_grow(size_t need) {
    if (kheap_area_count >= KHEAP_MAX_AREAS) {
        return 0;
    }
    uint32_t pages = DIV_ROUND_UP(need + 3 * HEAP_HDR, (size_t)PAGE_SIZE);
    if (pages < KHEAP_AREA_MIN_PAGES) {
        pages = KHEAP_AREA_MIN_PAGES;
    }
    if (pages > KHEAP_AREA_MAX_PAGES) {
        return 0;
    }
    void *mem = get_kernel_pages(pages);
    if (mem == NULL) {
        return 0;
    }
    kheap_area_add(mem, (size_t)pages * PAGE_SIZE);
    return 1;
}

static int block_own(void *ptr, struct MM_HEAP_BLOCK *b) {
    if (((uintptr_t)ptr & (HEAP_ALIGN - 1)) != 0) {
        return 0;
    }
    for (uint32_t i = 0; i < kheap_area_count; i++) {
        uintptr_t lo = (uintptr_t)kheap_areas[i].mem;
        uintptr_t hi = lo + kheap_areas[i].bytes;
        if ((uintptr_t)b < lo || (uintptr_t)b >= hi) {
            continue;
        }
        size_t total = block_total(b);
        return total >= HEAP_MIN_BLOCK && (total & (HEAP_ALIGN - 1)) == 0 &&
               (uintptr_t)block_next(b) <= hi;
    }
    return 0;
}

static void block_bad(const char *what, void *ptr) {
    kprintf("[kheap] %s: bad ptr=%#x areas=%u\n", what, (uint32_t)(uintptr_t)ptr,
            kheap_area_count);
}

static void *heap_alloc(size_t size) {
    size_t payload = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
    if (size == 0 || payload >= HEAP_MAX_BYTES) {
        return NULL;
    }
    void *p = block_acquire(payload);
    for (int i = 0; p == NULL && i < 2; i++) {
        if (!kheap_grow(payload)) {
            break;
        }
        p = block_acquire(payload);
    }
    return p;
}

static void stats_gain(struct MM_HEAP_BLOCK *b) {
    uint32_t total = (uint32_t)block_total(b);
    kheap_used_bytes += total;
    if (kheap_used_bytes > kheap_peak_bytes) {
        kheap_peak_bytes = kheap_used_bytes;
    }
    kheap_alloc_count++;
}

void *kmalloc(size_t size) {
    lock_acquire(&kheap_lock);
    void *p = heap_alloc(size);
    if (p != NULL) {
        stats_gain(block_from(p));
    }
    lock_release(&kheap_lock);
    return p;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void *kcalloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > HEAP_MAX_BYTES / nmemb) {
        return NULL;
    }
    return kzalloc(nmemb * size);
}

void *kmalloc_aligned(size_t size, size_t align) {
    if (size == 0) {
        return NULL;
    }
    if (align < HEAP_ALIGN) {
        align = HEAP_ALIGN;
    }
    if ((align & (align - 1)) != 0) {
        return NULL;
    }
    lock_acquire(&kheap_lock);
    void *p = NULL;
    if (align == HEAP_ALIGN) {
        p = heap_alloc(size);
    } else {
        p = heap_alloc(size + align + 2 * HEAP_HDR);
    }
    if (p != NULL && align != HEAP_ALIGN) {
        struct MM_HEAP_BLOCK *b = block_from(p);
        char *a = (char *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
        size_t gap = (size_t)(a - (char *)p);
        if (gap != 0 && gap < HEAP_MIN_BLOCK) {
            a += align;
            gap += align;
        }
        if (gap != 0) {
            struct MM_HEAP_BLOCK *c =
                (struct MM_HEAP_BLOCK *)(a - (ptrdiff_t)HEAP_HDR);
            c->size = block_total(b) - gap;
            c->prev = b;
            struct MM_HEAP_BLOCK *m = block_next(c);
            m->prev = c;
            b->size = gap | (b->size & HEAP_PREV_FREE);
            block_release(b);
            b = c;
        }
        block_trim_used(b, (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1));
        p = (void *)a;
    }
    if (p != NULL) {
        stats_gain(block_from(p));
    }
    lock_release(&kheap_lock);
    return p;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    lock_acquire(&kheap_lock);
    struct MM_HEAP_BLOCK *b = block_from(ptr);
    if (!block_own(ptr, b)) {
        block_bad("free wild pointer", ptr);
        ASSERT(!"kfree: wild pointer");
        lock_release(&kheap_lock);
        return;
    }
    if (b->size & HEAP_FREE) {
        block_bad("double free", ptr);
        ASSERT(!"kfree: double free");
        lock_release(&kheap_lock);
        return;
    }
    kheap_used_bytes -= (uint32_t)block_total(b);
    kheap_alloc_count--;
    block_release(b);
    lock_release(&kheap_lock);
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    lock_acquire(&kheap_lock);
    struct MM_HEAP_BLOCK *b = block_from(ptr);
    if (!block_own(ptr, b) || (b->size & HEAP_FREE)) {
        block_bad("realloc invalid block", ptr);
        ASSERT(!"krealloc: invalid block");
        lock_release(&kheap_lock);
        return NULL;
    }
    size_t need = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
    size_t old_total = block_total(b);
    void *np = ptr;
    if (need <= old_total - HEAP_HDR) {
        block_trim_used(b, need);
    } else {
        struct MM_HEAP_BLOCK *n = block_next(b);
        size_t grown = old_total + block_total(n);
        if ((n->size & HEAP_FREE) && grown - HEAP_HDR >= need) {
            freelist_remove(n);
            b->size = grown | (b->size & HEAP_PREV_FREE);
            struct MM_HEAP_BLOCK *m = block_next(b);
            m->prev = b;
            m->size &= ~HEAP_PREV_FREE;
            block_trim_used(b, need);
        } else {
            np = heap_alloc(size);
            if (np != NULL) {
                memcpy(np, ptr, old_total - HEAP_HDR < size ? old_total - HEAP_HDR
                                                            : size);
                block_release(b);
            }
        }
    }
    if (np != NULL) {
        uint32_t now = (uint32_t)block_total(block_from(np));
        kheap_used_bytes = kheap_used_bytes + now - (uint32_t)old_total;
        if (kheap_used_bytes > kheap_peak_bytes) {
            kheap_peak_bytes = kheap_used_bytes;
        }
    }
    lock_release(&kheap_lock);
    return np;
}

void kheap_stats(struct KHEAP_STATS *out) {
    lock_acquire(&kheap_lock);
    out->area_count = kheap_area_count;
    out->used_bytes = kheap_used_bytes;
    out->peak_bytes = kheap_peak_bytes;
    out->alloc_count = kheap_alloc_count;
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < kheap_area_count; i++) {
        bytes += (uint32_t)kheap_areas[i].bytes;
    }
    out->area_bytes = bytes;
    lock_release(&kheap_lock);
}

void kheap_init(void) {
    ASSERT(sizeof(struct MM_HEAP_BLOCK) == 16);
    ASSERT(HEAP_MIN_BLOCK >= 2 * sizeof(void *) + HEAP_HDR);
    lock_init(&kheap_lock);
    memset(&kheap_ctl, 0, sizeof(kheap_ctl));
    kheap_area_count = 0;
    kheap_used_bytes = 0;
    kheap_peak_bytes = 0;
    kheap_alloc_count = 0;
    if (!kheap_grow(0)) {
        kprintf("[kheap] init FAILED: no page for first area\n");
        return;
    }
    kprintf("[OK] kheap ready: %u KB in %u area(s)\n",
            DIV_ROUND_UP(KHEAP_AREA_MIN_PAGES * PAGE_SIZE, 1024u), 1u);
}

#define KST_SLOTS 192
#define KST_OPS   3000
#define KST_SEED  0x19860726u

struct KST_SLOT {
    void *p;
    size_t size;
    uint8_t tag;
};

static uint32_t kst_pat[KST_SLOTS > 2048 ? KST_SLOTS : 2048];

static uint32_t kst_rand(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int kst_check(void *p, size_t n, uint8_t tag) {
    memset(kst_pat, tag, n > sizeof(kst_pat) ? sizeof(kst_pat) : n);
    return memcmp(p, kst_pat, n > sizeof(kst_pat) ? sizeof(kst_pat) : n) == 0;
}

static int kheap_audit(void) {
    for (uint32_t i = 0; i < kheap_area_count; i++) {
        struct MM_HEAP_BLOCK *b = (struct MM_HEAP_BLOCK *)kheap_areas[i].mem;
        uintptr_t hi = (uintptr_t)kheap_areas[i].mem + kheap_areas[i].bytes;
        struct MM_HEAP_BLOCK *prev = NULL;
        size_t sum = 0;
        while ((uintptr_t)b < hi) {
            size_t total = block_total(b);
            if (prev == NULL) {
                if (b->prev != NULL || (b->size & HEAP_PREV_FREE)) {
                    kprintf("[kheap] audit: area %u head flags bad\n", i);
                    return -1;
                }
            } else {
                if (b->prev != prev ||
                    !!(b->size & HEAP_PREV_FREE) != !!(prev->size & HEAP_FREE)) {
                    kprintf("[kheap] audit: area %u prev link bad at %#x\n", i,
                            (uint32_t)(uintptr_t)b);
                    return -1;
                }
            }
            if (total != 0 &&
                (total < HEAP_MIN_BLOCK || (total & (HEAP_ALIGN - 1)) != 0)) {
                kprintf("[kheap] audit: area %u block size bad %#x\n", i,
                        (uint32_t)total);
                return -1;
            }
            if (b->size & HEAP_FREE) {
                int fl, sl;
                map_insert(total, &fl, &sl);
                struct MM_HEAP_BLOCK *it = kheap_ctl.blocks[fl][sl];
                int found = 0;
                while (it != NULL) {
                    struct MM_HEAP_FREE_LINK *l = free_link(it);
                    if (l->prev != NULL &&
                        free_link(l->prev)->next != it) {
                        kprintf("[kheap] audit: list prev broken at %#x\n",
                                (uint32_t)(uintptr_t)it);
                        return -1;
                    }
                    if (it == b) {
                        found = 1;
                        break;
                    }
                    it = l->next;
                }
                if (!found) {
                    kprintf("[kheap] audit: free block %#x missing from list\n",
                            (uint32_t)(uintptr_t)b);
                    return -1;
                }
            }
            sum += total;
            prev = b;
            if (total == 0) {
                break;
            }
            b = block_next(b);
        }
        if (prev == NULL || block_total(prev) != 0 ||
            (uintptr_t)prev != hi - HEAP_HDR) {
            kprintf("[kheap] audit: area %u sentinel bad\n", i);
            return -1;
        }
        if (sum != kheap_areas[i].bytes - HEAP_HDR) {
            kprintf("[kheap] audit: area %u size sum %u != %u\n", i,
                    (uint32_t)sum,
                    (uint32_t)(kheap_areas[i].bytes - HEAP_HDR));
            return -1;
        }
    }
    return 0;
}

void kheap_selftest(void) {
    struct KST_SLOT slot[KST_SLOTS];
    uint32_t seed = KST_SEED;
    memset(slot, 0, sizeof(slot));
    ASSERT(heap_fls(32) == 5 && heap_fls(0x40000000u) == 30);
    ASSERT(kmalloc(0) == NULL);
    ASSERT(kcalloc((size_t)1 << 32, 4) == NULL);
    for (uint32_t op = 0; op < KST_OPS; op++) {
        uint32_t r = kst_rand(&seed);
        uint32_t idx = r % KST_SLOTS;
        struct KST_SLOT *s = &slot[idx];
        uint32_t act = (r >> 8) % 100;
        if (s->p != NULL && act < 45) {
            if (!kst_check(s->p, s->size, s->tag)) {
                kprintf("[kheap] selftest: stamping at op=%u slot=%u\n", op, idx);
                ASSERT(!"kheap selftest failed");
            }
            kfree(s->p);
            s->p = NULL;
        } else if (s->p != NULL && act < 60) {
            size_t nsz = 1 + kst_rand(&seed) % (s->size * 2 + 128);
            void *np = krealloc(s->p, nsz);
            if (np == NULL) {
                kprintf("[kheap] selftest: realloc fail op=%u\n", op);
                ASSERT(!"kheap selftest failed");
            }
            size_t keep = nsz < s->size ? nsz : s->size;
            if (!kst_check(np, keep, s->tag)) {
                kprintf("[kheap] selftest: realloc lost data op=%u\n", op);
                ASSERT(!"kheap selftest failed");
            }
            s->p = np;
            s->size = nsz;
            s->tag = (uint8_t)(s->tag + 1);
            memset(s->p, s->tag, nsz);
        } else if (s->p == NULL && act < 68) {
            size_t align = HEAP_ALIGN << (r % 9);
            size_t sz = 1 + kst_rand(&seed) % 2048;
            void *p = kmalloc_aligned(sz, align);
            if (p == NULL) {
                kprintf("[kheap] selftest: aligned alloc fail op=%u\n", op);
                ASSERT(!"kheap selftest failed");
            }
            if (((uintptr_t)p & (align - 1)) != 0) {
                kprintf("[kheap] selftest: misaligned %u at %#x\n",
                        (uint32_t)align, (uint32_t)(uintptr_t)p);
                ASSERT(!"kheap selftest failed");
            }
            s->p = p;
            s->size = sz;
            s->tag = (uint8_t)(idx * 37 + op * 17 + 1);
            memset(p, s->tag, sz);
        } else if (s->p == NULL) {
            uint32_t u = (r >> 16) & 0xffff;
            size_t sz = u < 0x8000 ? 1 + u % 128
                                   : (u < 0xc000 ? 128 + u % 1024
                                                 : 1024 + u % 6144);
            void *p = kmalloc(sz);
            if (p == NULL) {
                for (uint32_t j = 0; j < KST_SLOTS && p == NULL; j++) {
                    if (slot[j].p != NULL) {
                        kfree(slot[j].p);
                        slot[j].p = NULL;
                        p = kmalloc(sz);
                    }
                }
                if (p == NULL) {
                    kprintf("[kheap] selftest: OOM op=%u size=%u\n", op,
                            (uint32_t)sz);
                    ASSERT(!"kheap selftest failed");
                }
            }
            s->p = p;
            s->size = sz;
            s->tag = (uint8_t)(idx * 37 + op * 17 + 1);
            memset(p, s->tag, sz);
        }
        if (op % 512 == 511 && kheap_audit() != 0) {
            ASSERT(!"kheap audit failed");
        }
    }
    for (uint32_t i = 0; i < KST_SLOTS; i++) {
        if (slot[i].p != NULL) {
            kfree(slot[i].p);
        }
    }
    if (kheap_audit() != 0) {
        ASSERT(!"kheap audit failed");
    }
    struct KHEAP_STATS st;
    kheap_stats(&st);
    if (st.used_bytes != 0 || st.alloc_count != 0 || st.area_count == 0) {
        kprintf("[kheap] selftest: leak used=%u count=%u\n", st.used_bytes,
                st.alloc_count);
        ASSERT(!"kheap selftest failed");
    }
    kprintf("[OK] kheap selftest: %u ops peak=%u bytes areas=%u\n", KST_OPS,
            st.peak_bytes, st.area_count);
    kprintf("KHEAP_SELFTEST_OK\n");
}
