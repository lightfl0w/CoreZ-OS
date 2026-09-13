#ifndef IDE_H
#define IDE_H

#include "kernel/fs/super_block.h"
#include "lib/list/list.h"
#include "lib/rbtree/rbtree.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "kernel/sched/sync.h"
#include <stdint.h>

struct DISK_PARTITION {
    uint32_t start_lba;
    uint32_t sec_cnt;
    struct DISK *my_disk;
    struct LIST_ELEM part_tag;
    char name[8];
    struct FS_SUPER_BLOCK *sb;
    struct MM_BITMAP block_bitmap;
    struct MM_BITMAP inode_bitmap;
    struct RB_ROOT open_inodes_rb;
};

struct DISK {
    char name[8];
    struct IDE_CHANNEL *my_channel;
    uint8_t dev_no;
    uint32_t max_lba;
    struct DISK_PARTITION prim_parts[4];
    struct DISK_PARTITION logic_parts[8];
};

struct IDE_CHANNEL {
    char name[8];
    uint16_t port_base;
    uint8_t irq_no;
    struct SCHED_LOCK lock;
    int expecting_intr;
    struct SCHED_SEMAPHORE disk_done;
    struct DISK devices[2];
};

void ide_read(struct DISK *hd, uint32_t lba, void *buf, uint32_t sec_cnt);
void ide_write(struct DISK *hd, uint32_t lba, void *buf, uint32_t sec_cnt);
void intr_hd_handler(uint8_t irq_no);
void ide_init(void);

extern struct IDE_CHANNEL channels[2];
extern struct LIST partition_list;

#endif
