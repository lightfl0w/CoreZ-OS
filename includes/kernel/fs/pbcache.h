#ifndef PBCACHE_H
#define PBCACHE_H

#include <stdint.h>

struct DISK;

struct PBC_SLOT {
    uint8_t *data;
    uint32_t dev;
    uint32_t blk;
    uint32_t last_used;
    uint8_t valid;
    uint8_t dirty;
    uint8_t busy;
};

struct PBC_DEV {
    uint32_t id;
    struct DISK *disk;
    uint32_t start;
    uint32_t spb;
    uint32_t bs;
};

#define PBC_SLOT_SIZE 4096u
#define PBC_DEV_MAX 4u

int pbc_dev_register(uint32_t dev_id, struct DISK *d, uint32_t start_lba,
                     uint32_t sect_per_block, uint32_t block_size);
int pbc_read(uint32_t dev_id, uint32_t blk, void *buf);
int pbc_read_run(uint32_t dev_id, uint32_t blk, uint32_t cnt, void *buf);
int pbc_write(uint32_t dev_id, uint32_t blk, const void *buf);
void pbc_flush_all(void);
void pbc_flush_tick(void);
uint32_t pbc_dirty_count(void);

#endif
