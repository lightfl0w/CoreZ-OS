#ifndef FS_INODE_H
#define FS_INODE_H

#include "lib/list/list.h"
#include "lib/rbtree/rbtree.h"
#include <stdint.h>

struct inode {
    uint32_t i_no;
    uint32_t i_size;
    uint32_t i_mode;
    uint32_t i_uid;
    uint32_t i_gid;
    uint32_t i_open_cnt;
    uint8_t write_deny;
    uint32_t i_block[15];
    struct RB_NODE inode_rb_node;
};

struct partition;

struct inode *inode_open(struct partition *part, uint32_t inode_no);
void inode_close(struct inode *inode);

#endif
