#ifndef OPS_BLOCK_OPS_H
#define OPS_BLOCK_OPS_H

#include <stdint.h>

struct block_ops {
    int (*read_sectors)(void *dev, uint32_t lba, void *buf, uint32_t count);
    int (*write_sectors)(void *dev, uint32_t lba, const void *buf,
                         uint32_t count);
};

extern const struct block_ops BLOCK;

#endif /* OPS_BLOCK_OPS_H */
