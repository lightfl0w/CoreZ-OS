#ifndef MM_KHEAP_H
#define MM_KHEAP_H

#include <stddef.h>
#include <stdint.h>

struct KHEAP_STATS {
    uint32_t area_count;
    uint32_t area_bytes;
    uint32_t used_bytes;
    uint32_t peak_bytes;
    uint32_t alloc_count;
};

#define HEAP_SL_LOG2        4u
#define HEAP_SL_COUNT       (1u << HEAP_SL_LOG2)
#define HEAP_FL_COUNT       32u

struct MM_HEAP_BLOCK {
    struct MM_HEAP_BLOCK *prev;
    size_t size;
};

struct MM_HEAP_FREE_LINK {
    struct MM_HEAP_BLOCK *next;
    struct MM_HEAP_BLOCK *prev;
};

struct MM_HEAP {
    uint32_t fl_bitmap;
    uint32_t sl_bitmap[HEAP_FL_COUNT];
    struct MM_HEAP_BLOCK *blocks[HEAP_FL_COUNT][HEAP_SL_COUNT];
};

struct MM_HEAP_AREA {
    void *mem;
    size_t bytes;
};

void kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
void kheap_stats(struct KHEAP_STATS *out);
void kheap_selftest(void);

#endif
