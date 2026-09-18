#ifndef FS_FILE_H
#define FS_FILE_H

#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "kernel/sched/sync.h"
#include <stdint.h>

#define MAX_FILE_OPEN 32

struct FILE {
    uint32_t fd_pos;
    uint32_t fd_flag;
    struct FS_INODE *fd_inode;
    uint32_t proc_id;
    uint32_t proc_aux;
    uint32_t ref_cnt;
};

extern struct FILE file_table[MAX_FILE_OPEN];
void file_table_init(void);

#define FILE_SLOT_RESERVED ((struct FS_INODE *)1)

/**
 * 分配一个全局 file 表槽位。
 *
 * @returns 槽位下标（0 ~ MAX_FILE_OPEN-1）；无空闲槽位返回 -1
 *
 * @remarks
 * 经占用位图 + CAS 完成，无锁且 O(1)。返回时 fd_inode 置为 FILE_SLOT_RESERVED，
 * 调用者须补齐 fd_pos / fd_flag / ref_cnt 后才能通过 fd_install 发布
 */
int file_table_alloc_slot(void);

/**
 * 归还全局 file 表槽位并清空其字段。
 *
 * @param idx 槽位下标；越界时不做任何事
 *
 * @warning
 * 只能在引用计数降到 0 之后调用，否则会与使用者竞争同一槽位
 */
void file_table_free_slot(int idx);

/**
 * 按下标取 file 表项。
 *
 * @param gfd 全局槽位下标
 * @returns 槽位指针；下标越界返回 NULL
 */
struct FILE *file_get(uint32_t gfd);

/**
 * 增加一个引用。
 *
 * @param gfd 全局槽位下标；越界时不做任何事
 *
 * @remarks
 * 原子自增，可在多核并发路径调用
 */
void file_table_ref(uint32_t gfd);

/**
 * 减少一个引用。
 *
 * @param gfd 全局槽位下标
 * @returns 递减后的引用计数（下限为 0）
 *
 * @remarks
 * 原子自减。返回 0 表示本线程是最后一个使用者，负责拆除资源并
 * file_table_free_slot；并发调用者中只有一个能观察到 0
 */
uint32_t file_table_unref(uint32_t gfd);

int fd_install(int32_t global_fd_idx);
int fd_release(uint32_t local_fd);
uint32_t fd_local2global(uint32_t local_fd);
uint32_t file_read(struct FILE *file, void *buf, uint32_t count);
uint32_t file_write(struct FILE *file, const void *buf, uint32_t count);

#endif
