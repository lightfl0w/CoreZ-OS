#include "kernel/fs/fsapi.h"

#include "ops/block_ops.h"
#include "drivers/block/block.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/proc.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"

static const struct VFS_OPS ext2_ops = {
    ext2_init,           ext2_partition,      ext2_lookup,
    ext2_lookup_ftype,   ext2_abs_path,       ext2_read_link_target,
    ext2_read_inode,     ext2_read_from_inode, ext2_dir_next,
    ext2_new_inode,      ext2_free_inode,     ext2_write_inode,
    ext2_write_to_inode, ext2_truncate_inode, ext2_add_entry,
    ext2_add_entry_dt,   ext2_remove_entry,   ext2_statfs_info,
};

static const struct VFS_OPS ext4_ops = {
    ext4_init,           ext4_partition,      ext4_lookup,
    ext4_lookup_ftype,   ext4_abs_path,       ext4_read_link_target,
    ext4_read_inode,     ext4_read_from_inode, ext4_dir_next,
    ext4_new_inode,      ext4_free_inode,     ext4_write_inode,
    ext4_write_to_inode, ext4_truncate_inode, ext4_add_entry,
    ext4_add_entry_dt,   ext4_remove_entry,   ext4_statfs_info,
};

#define FS_DRV_NONE 0
#define FS_DRV_EXT2 1
#define FS_DRV_EXT4 2

static const struct VFS_OPS *g_root_ops = &ext2_ops;
static struct VFS_MOUNT g_mounts[VFS_MAX_MOUNTS];

static int fs_probe(void) {
    struct LIST_ELEM *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct DISK_PARTITION *p =
            list_entry(e, struct DISK_PARTITION, part_tag);
        uint8_t *buf = (uint8_t *)get_kernel_pages(1);
        if (buf == NULL) {
            return FS_DRV_NONE;
        }
        memset(buf, 0, PAGE_SIZE);
        BLOCK.read_sectors(p->my_disk, p->start_lba, buf, 4);
        uint8_t *sb = buf + 1024;
        int drv = FS_DRV_NONE;
        if (*(uint16_t *)(sb + 0x38) == EXT4_SUPER_MAGIC) {
            uint32_t incompat = *(uint32_t *)(sb + 0x60);
            uint32_t ro_compat = *(uint32_t *)(sb + 0x64);
            if ((incompat &
                 (EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT)) ||
                (ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
                drv = FS_DRV_EXT4;
            } else {
                drv = FS_DRV_EXT2;
            }
        }
        free_kernel_page((uint32_t)buf);
        if (drv != FS_DRV_NONE) {
            return drv;
        }
        e = e->next;
    }
    return FS_DRV_NONE;
}

static uint32_t vfs_prefix_match(const char *path, const char *mnt) {
    uint32_t n = 0;
    while (mnt[n] != 0) {
        if (path[n] != mnt[n]) {
            return 0;
        }
        n++;
    }
    if (n == 1 && mnt[0] == '/') {
        return 1;
    }
    if (path[n] == 0 || path[n] == '/') {
        return n;
    }
    return 0;
}

const struct VFS_OPS *vfs_ops_for(const char *path) {
    const struct VFS_OPS *best = NULL;
    uint32_t best_len = 0;
    if (path == NULL || path[0] != '/') {
        return g_root_ops;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].active) {
            continue;
        }
        uint32_t n = vfs_prefix_match(path, g_mounts[i].path);
        if (n > best_len) {
            best_len = n;
            best = g_mounts[i].ops;
        }
    }
    return best != NULL ? best : g_root_ops;
}

int vfs_match_fs(const char *path, const struct VFS_OPS *ops) {
    return vfs_ops_for(path) == ops;
}

const struct VFS_OPS *vfs_root_ops(void) {
    return g_root_ops;
}

const struct VFS_MOUNT *vfs_mount_at(int idx) {
    if (idx < 0 || idx >= VFS_MAX_MOUNTS || !g_mounts[idx].active) {
        return NULL;
    }
    return &g_mounts[idx];
}

const char *vfs_ops_name(const struct VFS_OPS *ops) {
    if (ops == &ext4_ops) {
        return "ext4";
    }
    if (ops == &ext2_ops) {
        return "ext2";
    }
    if (ops == proc_vfs_ops()) {
        return "proc";
    }
    return "unknown";
}

int vfs_mount(const char *path, const struct VFS_OPS *ops) {
    if (path == NULL || path[0] != '/' || ops == NULL) {
        return -1;
    }
    uint32_t len = (uint32_t)strlen(path);
    if (len == 0 || len >= VFS_MNT_PATH_MAX) {
        return -1;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].active && strcmp(g_mounts[i].path, path) == 0) {
            return -1;
        }
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].active) {
            g_mounts[i].active = 1;
            memcpy(g_mounts[i].path, path, len + 1);
            g_mounts[i].ops = ops;
            return 0;
        }
    }
    return -1;
}

int vfs_unmount(const char *path) {
    if (path == NULL) {
        return -1;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].active && strcmp(g_mounts[i].path, path) == 0) {
            g_mounts[i].active = 0;
            g_mounts[i].ops = NULL;
            return 0;
        }
    }
    return -1;
}

int vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        g_mounts[i].active = 0;
        g_mounts[i].path[0] = 0;
        g_mounts[i].ops = NULL;
    }
    if (vfs_mount("/", g_root_ops) != 0) {
        return -1;
    }
    (void)vfs_mount("/proc", proc_vfs_ops());
    return 0;
}

int fs_init(void) {
    g_root_ops = fs_probe() == FS_DRV_EXT4 ? &ext4_ops : &ext2_ops;
    vfs_init();
    return g_root_ops->init();
}

struct DISK_PARTITION *fs_partition(void) {
    return g_root_ops->partition();
}

int fs_lookup(const char *path, uint32_t *ino, int *is_dir) {
    return vfs_ops_for(path)->lookup(path, ino, is_dir);
}

int fs_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow) {
    return vfs_ops_for(path)->lookup_ftype(path, ino, ftype, follow);
}

int fs_abs_path(const char *path, char *out, uint32_t cap) {
    return g_root_ops->abs_path(path, out, cap);
}

int fs_read_link_target(uint32_t ino, char *buf, uint32_t cap) {
    return g_root_ops->read_link_target(ino, buf, cap);
}

int fs_read_inode(uint32_t ino, struct FS_INODE *out) {
    return g_root_ops->read_inode(ino, out);
}

int fs_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                       uint32_t count) {
    return g_root_ops->read_from_inode(ino, off, buf, count);
}

int fs_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                struct FS_DIRENT *out) {
    return g_root_ops->dir_next(dino, pos, out);
}

int fs_new_inode(uint32_t mode, struct FS_INODE *out) {
    return g_root_ops->new_inode(mode, out);
}

void fs_free_inode(uint32_t ino) {
    g_root_ops->free_inode(ino);
}

int fs_write_inode(uint32_t ino, const struct FS_INODE *in) {
    return g_root_ops->write_inode(ino, in);
}

int fs_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                      uint32_t count) {
    return g_root_ops->write_to_inode(ino, off, buf, count);
}

void fs_truncate_inode(struct FS_INODE *ino) {
    g_root_ops->truncate_inode(ino);
}

int fs_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                 int is_dir) {
    return g_root_ops->add_entry(dino, ino, name, is_dir);
}

int fs_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                    uint8_t dtype) {
    return g_root_ops->add_entry_dt(dino, ino, name, dtype);
}

int fs_remove_entry(struct FS_INODE *dino, const char *name) {
    return g_root_ops->remove_entry(dino, name);
}

void fs_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                    uint32_t *files, uint32_t *ffree) {
    g_root_ops->statfs_info(bsize, blocks, bfree, files, ffree);
}
