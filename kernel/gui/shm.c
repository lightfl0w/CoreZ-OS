#include "kernel/gui/shm.h"

#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/sync.h"

#define SHM_MAX_POOLS 32

static struct wl_buffer {
    uint8_t *data;
    uint32_t size;
    uint32_t pages;
    int refcnt;
    uint32_t state;
} buffers[SHM_MAX_POOLS];

static struct lock shm_lock;

void shm_init(void) {
    lock_init(&shm_lock);
    memset(buffers, 0, sizeof(buffers));
}

int wl_buffer_create(uint32_t size) {
    if (size == 0)
        return -1;
    lock_acquire(&shm_lock);
    for (int i = 0; i < SHM_MAX_POOLS; i++) {
        struct wl_buffer *b = &buffers[i];
        if (b->refcnt != 0)
            continue;
        uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
        if (mem == 0) {
            lock_release(&shm_lock);
            return -1;
        }
        memset(mem, 0, pages * PAGE_SIZE);
        b->data = mem;
        b->size = size;
        b->pages = pages;
        b->refcnt = 1;
        b->state = WLBUF_READY;
        lock_release(&shm_lock);
        return i;
    }
    lock_release(&shm_lock);
    return -1;
}

void *wl_buffer_map(int id) {
    if (id < 0 || id >= SHM_MAX_POOLS)
        return 0;
    return buffers[id].data;
}

void wl_buffer_ref(int id) {
    if (id < 0 || id >= SHM_MAX_POOLS)
        return;
    lock_acquire(&shm_lock);
    if (buffers[id].refcnt > 0)
        buffers[id].refcnt++;
    lock_release(&shm_lock);
}

void wl_buffer_release(int id) {
    if (id < 0 || id >= SHM_MAX_POOLS)
        return;
    lock_acquire(&shm_lock);
    struct wl_buffer *b = &buffers[id];
    if (b->refcnt == 0) {
        lock_release(&shm_lock);
        return;
    }
    b->refcnt--;
    if (b->refcnt == 0) {
        for (uint32_t i = 0; i < b->pages; i++)
            free_kernel_page((uint32_t)b->data + i * PAGE_SIZE);
        b->data = 0;
        b->size = 0;
        b->pages = 0;
        b->state = WLBUF_FREE;
    }
    lock_release(&shm_lock);
}

uint32_t wl_buffer_state(int id) {
    if (id < 0 || id >= SHM_MAX_POOLS)
        return WLBUF_FREE;
    return buffers[id].state;
}

void wl_buffer_set_state(int id, uint32_t st) {
    if (id < 0 || id >= SHM_MAX_POOLS)
        return;
    lock_acquire(&shm_lock);
    if (buffers[id].refcnt > 0)
        buffers[id].state = st;
    lock_release(&shm_lock);
}

struct shm_pool *shm_pool_create(uint32_t size) {
    static struct shm_pool pools[SHM_MAX_POOLS];
    int id = wl_buffer_create(size);
    if (id < 0)
        return 0;
    lock_acquire(&shm_lock);
    for (int i = 0; i < SHM_MAX_POOLS; i++) {
        if (pools[i].in_use)
            continue;
        pools[i].in_use = 1;
        pools[i].buf_id = id;
        pools[i].data = wl_buffer_map(id);
        pools[i].size = size;
        lock_release(&shm_lock);
        return &pools[i];
    }
    wl_buffer_release(id);
    lock_release(&shm_lock);
    return 0;
}

void shm_pool_destroy(struct shm_pool *pool) {
    if (pool == 0 || !pool->in_use)
        return;
    wl_buffer_release(pool->buf_id);
    pool->buf_id = -1;
    pool->data = 0;
    pool->size = 0;
    pool->in_use = 0;
}
