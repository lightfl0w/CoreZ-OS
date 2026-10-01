#ifndef FS_VFS_H
#define FS_VFS_H

#include <stdint.h>

struct FS_INODE;
struct FS_DIRENT;
struct DISK_PARTITION;

#define VFS_MAX_MOUNTS 8
#define VFS_MNT_PATH_MAX 32

struct VFS_OPS {
    int (*init)(void);
    struct DISK_PARTITION *(*partition)(void);
    int (*lookup)(const char *path, uint32_t *ino, int *is_dir);
    int (*lookup_ftype)(const char *path, uint32_t *ino, int *ftype,
                        int follow);
    int (*abs_path)(const char *path, char *out, uint32_t cap);
    int (*read_link_target)(uint32_t ino, char *buf, uint32_t cap);
    int (*read_inode)(uint32_t ino, struct FS_INODE *out);
    int (*read_from_inode)(const struct FS_INODE *ino, uint32_t off,
                           void *buf, uint32_t count);
    int (*dir_next)(const struct FS_INODE *dino, uint32_t *pos,
                    struct FS_DIRENT *out);
    int (*new_inode)(uint32_t mode, struct FS_INODE *out);
    void (*free_inode)(uint32_t ino);
    int (*write_inode)(uint32_t ino, const struct FS_INODE *in);
    int (*write_to_inode)(struct FS_INODE *ino, uint32_t off,
                          const void *buf, uint32_t count);
    void (*truncate_inode)(struct FS_INODE *ino);
    int (*add_entry)(struct FS_INODE *dino, uint32_t ino, const char *name,
                     int is_dir);
    int (*add_entry_dt)(struct FS_INODE *dino, uint32_t ino,
                        const char *name, uint8_t dtype);
    int (*remove_entry)(struct FS_INODE *dino, const char *name);
    void (*statfs_info)(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                        uint32_t *files, uint32_t *ffree);
};

struct VFS_MOUNT {
    int active;
    char path[VFS_MNT_PATH_MAX];
    const struct VFS_OPS *ops;
};

int vfs_init(void);
int vfs_mount(const char *path, const struct VFS_OPS *ops);
int vfs_unmount(const char *path);
const struct VFS_OPS *vfs_ops_for(const char *path);
int vfs_match_fs(const char *path, const struct VFS_OPS *ops);
const struct VFS_OPS *vfs_root_ops(void);

#endif
