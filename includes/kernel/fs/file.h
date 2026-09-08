#ifndef FS_FILE_H
#define FS_FILE_H

#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "kernel/sched/sync.h"
#include <stdint.h>

#define MAX_FILE_OPEN 32

struct file {
    uint32_t fd_pos;
    uint32_t fd_flag;
    struct inode *fd_inode;
    uint32_t proc_id;
    uint32_t proc_aux;
    uint32_t ref_cnt;
};

extern struct file file_table[MAX_FILE_OPEN];
extern struct lock file_table_lock;
void file_table_init(void);

#define FILE_SLOT_RESERVED ((struct inode *)1)

int file_table_alloc_slot(void);
void file_table_free_slot(int idx);
struct file *file_get(uint32_t gfd);
void file_table_ref(uint32_t gfd);

int fd_install(int32_t global_fd_idx);
int fd_release(uint32_t local_fd);
uint32_t fd_local2global(uint32_t local_fd);
uint32_t file_read(struct file *file, void *buf, uint32_t count);
uint32_t file_write(struct file *file, const void *buf, uint32_t count);

#endif
