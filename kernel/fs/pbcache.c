#include "kernel/fs/pbcache.h"
#include "drivers/char/console/io.h"
#include "kernel/asm_func.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/sync.h"
#include "lib/str/str.h"
#include "ops/block_ops.h"

#define PBC_DEV_SLOTS_MAX 512u

static struct PBC_SLOT s_slots[PBC_DEV_SLOTS_MAX];
static uint32_t s_nr_slots;
static uint8_t s_pool_ready;
static struct PBC_DEV s_devs[PBC_DEV_MAX];
static uint32_t s_nr_devs;
static uint32_t s_seq;
static struct SCHED_SPINLOCK pbc_lock;

static struct PBC_DEV *dev_of(uint32_t id) {
    for (uint32_t i = 0; i < s_nr_devs; i++) {
        if (s_devs[i].id == id) {
            return &s_devs[i];
        }
    }
    return NULL;
}

static int pbc_fill_from_disk(struct PBC_DEV *d, uint32_t blk, uint8_t *dst) {
    if (d == NULL || d->disk == NULL) {
        return -1;
    }
    BLOCK.read_sectors(d->disk, d->start + blk * d->spb, dst, d->spb);
    return 0;
}

static void pbc_write_to_disk(struct PBC_DEV *d, uint32_t blk,
                              const uint8_t *src) {
    if (d == NULL || d->disk == NULL) {
        return;
    }
    BLOCK.write_sectors(d->disk, d->start + blk * d->spb, (void *)src,
                        d->spb);
}

static inline uint32_t pbc_lk(void) {
    uint32_t f = asm_save_eflags();
    asm_cli();
    while (pbc_lock.locked) {
        asm_pause();
    }
    pbc_lock.locked = 1;
    return f;
}

static inline void pbc_unlk(uint32_t f) {
    pbc_lock.locked = 0;
    asm_restore_eflags(f);
}

int pbc_dev_register(uint32_t dev_id, struct DISK *d, uint32_t start_lba,
                     uint32_t sect_per_block, uint32_t block_size) {
    if (dev_of(dev_id) != NULL) {
        return 0;
    }
    if (s_nr_devs >= PBC_DEV_MAX) {
        return -1;
    }
    if (s_pool_ready == 0) {
        uint32_t want = PBC_DEV_SLOTS_MAX;
        while (want >= 64u) {
            uint8_t *base =
                (uint8_t *)get_kernel_pages(want * PBC_SLOT_SIZE / PAGE_SIZE);
            if (base != NULL) {
                for (uint32_t i = 0; i < want; i++) {
                    s_slots[i].data = base + i * PBC_SLOT_SIZE;
                }
                s_nr_slots = want;
                break;
            }
            want /= 2u;
        }
        if (want < 64u) {
            kprintf("pbc: disabled (no memory)\n");
            s_nr_slots = 0;
        }
        pbc_lock.locked = 0;
        s_pool_ready = 1;
    }
    struct PBC_DEV *dv = &s_devs[s_nr_devs++];
    dv->id = dev_id;
    dv->disk = d;
    dv->start = start_lba;
    dv->spb = sect_per_block;
    dv->bs = block_size;
    return 0;
}

static struct PBC_SLOT *pbc_find(uint32_t dev, uint32_t blk) {
    for (uint32_t i = 0; i < s_nr_slots; i++) {
        struct PBC_SLOT *s = &s_slots[i];
        if (s->valid && s->busy == 0 && s->dev == dev && s->blk == blk) {
            return s;
        }
    }
    return NULL;
}

static struct PBC_SLOT *pbc_alloc(uint32_t dev, uint32_t blk,
                                  uint32_t *wb_dev, uint32_t *wb_blk,
                                  uint8_t *wb_dirty) {
    struct PBC_SLOT *victim = NULL;
    for (uint32_t i = 0; i < s_nr_slots; i++) {
        if (s_slots[i].valid == 0 && s_slots[i].busy == 0) {
            victim = &s_slots[i];
            break;
        }
    }
    if (victim == NULL) {
        uint32_t best = 0;
        int have = 0;
        for (uint32_t i = 0; i < s_nr_slots; i++) {
            struct PBC_SLOT *s = &s_slots[i];
            if (s->busy) {
                continue;
            }
            if (have == 0 || s->last_used < s_slots[best].last_used) {
                best = i;
                have = 1;
            }
        }
        if (have == 0) {
            return NULL;
        }
        victim = &s_slots[best];
        *wb_dev = victim->dev;
        *wb_blk = victim->blk;
        *wb_dirty = victim->dirty;
    }
    victim->busy = 1;
    victim->dev = dev;
    victim->blk = blk;
    victim->valid = 0;
    return victim;
}

int pbc_read(uint32_t dev, uint32_t blk, void *buf) {
    if (s_nr_slots == 0) {
        struct PBC_DEV *d = dev_of(dev);
        return pbc_fill_from_disk(d, blk, (uint8_t *)buf);
    }
    uint32_t f = pbc_lk();
    struct PBC_SLOT *s = pbc_find(dev, blk);
    if (s != NULL) {
        s->busy = 1;
        s->last_used = ++s_seq;
        uint32_t bs = dev_of(dev)->bs;
        pbc_unlk(f);
        memcpy(buf, s->data, bs);
        f = pbc_lk();
        s->busy = 0;
        pbc_unlk(f);
        return 0;
    }
    uint32_t wb_dev = 0;
    uint32_t wb_blk = 0;
    uint8_t wb_dirty = 0;
    s = pbc_alloc(dev, blk, &wb_dev, &wb_blk, &wb_dirty);
    if (s == NULL) {
        pbc_unlk(f);
        return -1;
    }
    struct PBC_DEV *d = dev_of(dev);
    uint32_t bs = d->bs;
    uint32_t old_dev = wb_dev;
    uint32_t old_blk = wb_blk;
    struct PBC_DEV *od = wb_dirty ? dev_of(old_dev) : NULL;
    pbc_unlk(f);
    if (wb_dirty) {
        pbc_write_to_disk(od, old_blk, s->data);
    }
    int rc = pbc_fill_from_disk(d, blk, s->data);
    if (rc != 0) {
        f = pbc_lk();
        s->busy = 0;
        pbc_unlk(f);
        return rc;
    }
    memcpy(buf, s->data, bs);
    f = pbc_lk();
    s->valid = 1;
    s->dirty = 0;
    s->last_used = ++s_seq;
    s->busy = 0;
    pbc_unlk(f);
    return 0;
}

int pbc_write(uint32_t dev, uint32_t blk, const void *buf) {
    if (s_nr_slots == 0) {
        struct PBC_DEV *d = dev_of(dev);
        if (d == NULL || d->disk == NULL) {
            return -1;
        }
        BLOCK.write_sectors(d->disk, d->start + blk * d->spb, (void *)buf,
                            d->spb);
        return 0;
    }
    uint32_t bs = dev_of(dev)->bs;
    uint32_t f = pbc_lk();
    struct PBC_SLOT *s = pbc_find(dev, blk);
    if (s == NULL) {
        uint32_t wb_dev = 0;
        uint32_t wb_blk = 0;
        uint8_t wb_dirty = 0;
        s = pbc_alloc(dev, blk, &wb_dev, &wb_blk, &wb_dirty);
        if (s == NULL) {
            pbc_unlk(f);
            return -1;
        }
        struct PBC_DEV *od = wb_dirty ? dev_of(wb_dev) : NULL;
        pbc_unlk(f);
        if (wb_dirty) {
            pbc_write_to_disk(od, wb_blk, s->data);
        }
        memcpy(s->data, buf, bs);
        f = pbc_lk();
        s->valid = 1;
        s->dirty = 1;
        s->last_used = ++s_seq;
        s->busy = 0;
        pbc_unlk(f);
        return 0;
    }
    s->busy = 1;
    s->last_used = ++s_seq;
    pbc_unlk(f);
    memcpy(s->data, buf, bs);
    f = pbc_lk();
    s->valid = 1;
    s->dirty = 1;
    s->busy = 0;
    pbc_unlk(f);
    return 0;
}

static void pbc_flush_slot(struct PBC_SLOT *s) {
    uint32_t f = pbc_lk();
    if (s->valid == 0 || s->dirty == 0 || s->busy) {
        pbc_unlk(f);
        return;
    }
    struct PBC_DEV *d = dev_of(s->dev);
    if (d == NULL) {
        s->dirty = 0;
        pbc_unlk(f);
        return;
    }
    s->busy = 1;
    pbc_unlk(f);
    pbc_write_to_disk(d, s->blk, s->data);
    f = pbc_lk();
    s->dirty = 0;
    s->busy = 0;
    pbc_unlk(f);
}

void pbc_flush_all(void) {
    if (s_nr_slots == 0) {
        return;
    }
    for (uint32_t i = 0; i < s_nr_slots; i++) {
        pbc_flush_slot(&s_slots[i]);
    }
}

void pbc_flush_tick(void) {
    extern volatile uint32_t tick;
    static uint32_t last_flush;
    uint32_t now = tick;
    if ((uint32_t)(now - last_flush) < 500u) {
        return;
    }
    last_flush = now;
    pbc_flush_all();
}

uint32_t pbc_dirty_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_nr_slots; i++) {
        if (s_slots[i].valid && s_slots[i].dirty) {
            n++;
        }
    }
    return n;
}
