#include "kernel/fs/file.h"
#include "kernel/asm_func.h"
#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "drivers/char/tty.h"
#include "lib/str/str.h"
struct FILE file_table[MAX_FILE_OPEN];

/**
 * 槽位占用位图：bit i 置位表示槽位 i 已分配，0/1/2 为保留槽位。
 *
 * @remarks
 * 取代原先"线性扫描 fd_inode == NULL"的分配方式：后者既 O(MAX_FILE_OPEN)，
 * 又与 proc 文件（fd_inode 合法地为 NULL）冲突。现在分配是一次 CAS + ctz 的
 * O(1) 操作，且不再依赖 fd_inode 的取值。并发修改仅置位/清位单个 bit
 */
static volatile uint32_t file_slot_used;

/**
 * 初始化 file 表。
 *
 * @remarks
 * 在 filesys_init 阶段调用一次；之后槽位状态只由 file_slot_used 与
 * file_table_alloc_slot / file_table_free_slot 维护
 */
void file_table_init(void) {
    file_slot_used = 0x7u;
    for (uint32_t i = 0; i < 3; i++) {
        file_table[i].fd_inode = FILE_SLOT_RESERVED;
        file_table[i].ref_cnt = 1;
    }
}

int file_table_alloc_slot(void) {
    for (;;) {
        uint32_t b = cpu_atomic_load32(&file_slot_used);
        uint32_t free_bits = ~b;
#if MAX_FILE_OPEN < 32
        free_bits &= (1u << MAX_FILE_OPEN) - 1u;
#endif
        if (free_bits == 0) {
            return -1;
        }
        uint32_t i = (uint32_t)__builtin_ctz(free_bits);
        if (cpu_cmpxchg32(&file_slot_used, b, b | (1u << i)) != b) {
            continue;
        }
        file_table[i].fd_pos = 0;
        file_table[i].fd_flag = 0;
        file_table[i].fd_inode = FILE_SLOT_RESERVED;
        file_table[i].proc_id = 0;
        file_table[i].proc_aux = 0;
        file_table[i].ref_cnt = 0;
        return (int)i;
    }
}

void file_table_free_slot(int idx) {
    if (idx < 0 || idx >= (int)MAX_FILE_OPEN) {
        return;
    }
    file_table[idx].fd_inode = NULL;
    file_table[idx].fd_pos = 0;
    file_table[idx].fd_flag = 0;
    file_table[idx].proc_id = 0;
    file_table[idx].proc_aux = 0;
    file_table[idx].ref_cnt = 0;
    for (;;) {
        uint32_t b = cpu_atomic_load32(&file_slot_used);
        if (!(b & (1u << idx))) {
            break;
        }
        if (cpu_cmpxchg32(&file_slot_used, b, b & ~(1u << idx)) == b) {
            break;
        }
    }
}

struct FILE *file_get(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return NULL;
    }
    return &file_table[gfd];
}

void file_table_ref(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return;
    }
    cpu_xadd32(&file_table[gfd].ref_cnt, 1);
}

uint32_t file_table_unref(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return 0;
    }
    uint32_t old = cpu_xadd32(&file_table[gfd].ref_cnt, (uint32_t)-1);
    if (old == 0) {
        file_table[gfd].ref_cnt = 0;
        return 0;
    }
    return old - 1;
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

static int chardev_tty(const struct FS_INODE *ino) {
    return (ino->i_block[0] >> 8) == 5u;
}

static uint32_t chardev_read(const struct FS_INODE *ino, void *buf,
                             uint32_t count) {
    if (ino->i_block[0] >> 8 == 5u)
        return (uint32_t)TTY.read((char *)buf, count);
    if (ino->i_block[0] == 0x0105u)
        memset(buf, 0, count);
    return ino->i_block[0] == 0x0105u ? count : 0;
}

static uint32_t chardev_write(const struct FS_INODE *ino, const void *buf,
                              uint32_t count) {
    if (ino->i_block[0] >> 8 == 5u)
        return (uint32_t)TTY.write((const char *)buf, count);
    return count;
}

uint32_t file_read(struct FILE *file, void *buf, uint32_t count) {
    if (fs_is_chardev(file->fd_inode))
        return chardev_read(file->fd_inode, buf, count);
    int r = ext2_read_from_inode(file->fd_inode, file->fd_pos, buf, count);
    file->fd_pos += (uint32_t)r;
    return (uint32_t)r;
}

uint32_t file_write(struct FILE *file, const void *buf, uint32_t count) {
    if (fs_is_chardev(file->fd_inode))
        return chardev_write(file->fd_inode, buf, count);
    int r = ext2_write_to_inode(file->fd_inode, file->fd_pos, buf, count);
    file->fd_pos += (uint32_t)r;
    return (uint32_t)r;
}
