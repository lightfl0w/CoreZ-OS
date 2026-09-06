#include "kernel/fs/file.h"
#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "drivers/char/tty.h"
#include "lib/str/str.h"
struct file file_table[MAX_FILE_OPEN];

struct lock file_table_lock;

void file_table_init(void) {
    lock_init(&file_table_lock);
    for (uint32_t i = 0; i < 3; i++) {
        file_table[i].fd_inode = FILE_SLOT_RESERVED;
        file_table[i].ref_cnt = 1;
    }
}

int file_table_alloc_slot(void) {
    lock_acquire(&file_table_lock);
    for (uint32_t i = 0; i < MAX_FILE_OPEN; i++) {
        if (file_table[i].fd_inode == NULL) {
            file_table[i].fd_inode = FILE_SLOT_RESERVED;
            lock_release(&file_table_lock);
            return (int)i;
        }
    }
    lock_release(&file_table_lock);
    return -1;
}

void file_table_free_slot(int idx) {
    if (idx < 0 || idx >= (int)MAX_FILE_OPEN) {
        return;
    }
    lock_acquire(&file_table_lock);
    file_table[idx].fd_inode = NULL;
    file_table[idx].fd_pos = 0;
    file_table[idx].fd_flag = 0;
    file_table[idx].proc_id = 0;
    file_table[idx].ref_cnt = 0;
    lock_release(&file_table_lock);
}

struct file *file_get(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return NULL;
    }
    return &file_table[gfd];
}

void file_table_ref(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return;
    }
    lock_acquire(&file_table_lock);
    file_table[gfd].ref_cnt++;
    lock_release(&file_table_lock);
}
int fd_install(int32_t global_fd_idx) {
    uint32_t local_fd = 3;
    while (local_fd < MAX_FILES_OPEN_PER_PROC) {
        if (current->fd_table[local_fd] == (uint32_t)-1) {
            current->fd_table[local_fd] = (uint32_t)global_fd_idx;
            return local_fd;
        }
        local_fd++;
    }
    return -1;
}
int fd_release(uint32_t local_fd) {
    if (local_fd >= MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }
    current->fd_table[local_fd] = (uint32_t)-1;
    return 0;
}
uint32_t fd_local2global(uint32_t local_fd) {
    if (local_fd >= MAX_FILES_OPEN_PER_PROC) {
        return (uint32_t)-1;
    }
    return current->fd_table[local_fd];
}
static int chardev_tty(const struct inode *ino) {
    return (ino->i_block[0] >> 8) == 5u;
}
static uint32_t chardev_read(const struct inode *ino, void *buf,
                             uint32_t count) {
    if (ino->i_block[0] >> 8 == 5u)
        return (uint32_t)TTY.read((char *)buf, count);
    if (ino->i_block[0] == 0x0105u)
        memset(buf, 0, count);
    return ino->i_block[0] == 0x0105u ? count : 0;
}
static uint32_t chardev_write(const struct inode *ino, const void *buf,
                              uint32_t count) {
    if (ino->i_block[0] >> 8 == 5u)
        return (uint32_t)TTY.write((const char *)buf, count);
    return count;
}
uint32_t file_read(struct file *file, void *buf, uint32_t count) {
    if (fs_is_chardev(file->fd_inode))
        return chardev_read(file->fd_inode, buf, count);
    int r = ext2_read_from_inode(file->fd_inode, file->fd_pos, buf, count);
    file->fd_pos += (uint32_t)r;
    return (uint32_t)r;
}
uint32_t file_write(struct file *file, const void *buf, uint32_t count) {
    if (fs_is_chardev(file->fd_inode))
        return chardev_write(file->fd_inode, buf, count);
    int r = ext2_write_to_inode(file->fd_inode, file->fd_pos, buf, count);
    file->fd_pos += (uint32_t)r;
    return (uint32_t)r;
}
