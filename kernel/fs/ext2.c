#include "kernel/fs/ext2.h"

#include "ops/block_ops.h"
#include "kernel/sched/sync.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/fs/dir.h"

static struct lock ext2_lock;

static int ext2_read_inode_impl(uint32_t ino, struct inode *out);
static int ext2_write_inode_impl(uint32_t ino, const struct inode *in);
static int ext2_write_to_inode_impl(struct inode *ino, uint32_t off,
                                    const void *buf, uint32_t count);
static void ext2_truncate_inode_impl(struct inode *ino);
static uint32_t ext2_new_inode_impl(uint32_t mode, struct inode *out);
static int ext2_add_entry_impl(struct inode *dino, uint32_t ino,
                               const char *name, uint8_t dtype);
static int ext2_remove_entry_impl(struct inode *dino, const char *name);
static int ext2_read_from_inode_impl(const struct inode *ino, uint32_t off,
                                     void *buf, uint32_t count);
static int ext2_dir_next_impl(const struct inode *dino, uint32_t *pos,
                              struct dir_entry *out);
static int ext2_lookup_depth(const char *path, uint32_t *ino, int *ftype,
                             int follow, int depth);
static struct disk *disk = NULL;
static struct partition *part = NULL;
static uint32_t start = 0;
static uint32_t bs = 1024;
static uint32_t sect_per_block = 2;
static uint32_t inodes_per_group = 0;
static uint32_t inode_table_blk = 0;
static uint32_t first_block = 0;

static uint32_t block_bitmap_blk = 0;
static uint32_t inode_bitmap_blk = 0;
static uint32_t total_blocks = 0;
static uint32_t data_start = 0;
static uint32_t free_blocks = 0;
static uint32_t free_inodes = 0;

static int ext2_read_block(uint32_t blk, void *buf) {
    if (disk == NULL) {
        return -1;
    }
    if (blk >= total_blocks && total_blocks != 0) {
        kprintf("[ext2] read out-of-range block %d (total %d)\n", blk,
                total_blocks);
        return -1;
    }
    BLOCK.read_sectors(disk, start + blk * sect_per_block, buf,
                       sect_per_block);
    return 0;
}

static int ext2_write_block(uint32_t blk, const void *buf) {
    if (disk == NULL) {
        return -1;
    }
    BLOCK.write_sectors(disk, start + blk * sect_per_block, (void *)buf,
                        sect_per_block);
    return 0;
}

struct partition *ext2_partition(void) {
    return part;
}

int ext2_init(void) {
    lock_init(&ext2_lock);
    struct list_elem *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct partition *p = list_entry(e, struct partition, part_tag);
        uint8_t *buf = (uint8_t *)get_kernel_pages(1);
        if (buf == NULL) {
            return -1;
        }
        memset(buf, 0, 4096);
        BLOCK.read_sectors(p->my_disk, p->start_lba, buf, 4);
        struct EXT2_SURPER *sb = (struct EXT2_SURPER *)(buf + 1024);
        if (sb->s_magic == EXT2_SUPER_MAGIC) {
            disk = p->my_disk;
            part = p;
            start = part->start_lba;
            bs = 1024u << sb->s_log_block_size;
            sect_per_block = bs / 512u;
            inodes_per_group = sb->s_inodes_per_group;
            first_block = sb->s_first_data_block;
            total_blocks = sb->s_blocks_count;
            free_blocks = sb->s_free_blocks_count;
            free_inodes = sb->s_free_inodes_count;
            uint8_t *gb = (uint8_t *)get_kernel_pages(1);
            if (gb == NULL) {
                free_kernel_page((uint32_t)buf);
                return -1;
            }
            ext2_read_block(first_block + 1, gb);
            block_bitmap_blk = *(uint32_t *)(gb + 0);
            inode_bitmap_blk = *(uint32_t *)(gb + 4);
            inode_table_blk = *(uint32_t *)(gb + 8);
            uint32_t itable_blocks =
                (inodes_per_group * EXT2_INODE_SIZE + bs - 1) / bs;
            data_start = inode_table_blk + itable_blocks;
            free_kernel_page((uint32_t)gb);
            free_kernel_page((uint32_t)buf);
            kprintf("ext2 mounted on %s, block_size=%d, inodes_per_group=%d, "
                    "data_start=%d\n",
                    p->name, (int)bs, (int)inodes_per_group, (int)data_start);
            return 0;
        }
        free_kernel_page((uint32_t)buf);
        e = e->next;
    }
    kprintf("ext2: no ext2 filesystem found\n");
    return -1;
}

int ext2_read_inode(uint32_t ino, struct inode *out) {
    lock_acquire(&ext2_lock);
    int rc = ext2_read_inode_impl(ino, out);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_read_inode_impl(uint32_t ino, struct inode *out) {
    if (disk == NULL || ino == 0) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, 4096);
    uint32_t per_block = bs / EXT2_INODE_SIZE;
    uint32_t blk = inode_table_blk + (ino - 1) / per_block;
    uint32_t off = ((ino - 1) % per_block) * EXT2_INODE_SIZE;
    ext2_read_block(blk, buf);
    uint8_t *p = buf + off;
    memset(out, 0, sizeof(struct inode));
    out->i_no = ino;
    out->i_mode = (uint32_t)(*(uint16_t *)(p + 0));
    out->i_uid = *(uint16_t *)(p + 2);
    out->i_size = *(uint32_t *)(p + 4);
    out->i_gid = *(uint16_t *)(p + 24);
    uint32_t bi = 0;
    for (bi = 0; bi < 15; bi++) {
        out->i_block[bi] = *(uint32_t *)(p + 40 + 4 * bi);
    }
    free_kernel_page((uint32_t)buf);
    return 0;
}

static uint32_t ext2_alloc_block(void) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return 0;
    }
    for (uint32_t bb = 0; bb * bs * 8 < total_blocks; bb++) {
        uint32_t bbitblk = block_bitmap_blk + bb;
        memset(buf, 0, 4096);
        ext2_read_block(bbitblk, buf);
        uint32_t base = bb * bs * 8;
        uint32_t lo = data_start > base ? data_start - base : 0;
        uint32_t hi = total_blocks - base;
        if (hi > bs * 8) {
            hi = bs * 8;
        }
        if (lo >= hi) {
            continue;
        }
        uint32_t first = lo >> 3;
        uint32_t last = (hi - 1) >> 3;
        for (uint32_t i = first; i <= last; i++) {
            uint8_t inv = (uint8_t)~buf[i];
            if (i == first && (lo & 7)) {
                inv &= (uint8_t)(0xffu >> (lo & 7));
            }
            if (i == last && (hi & 7)) {
                inv &= (uint8_t)(0xffu << (8 - (hi & 7)));
            }
            if (inv == 0) {
                continue;
            }
            uint32_t bit = (uint32_t)__builtin_clz(inv) - 24;
            buf[i] |= (uint8_t)(0x80u >> bit);
            ext2_write_block(bbitblk, buf);
            free_blocks--;
            free_kernel_page((uint32_t)buf);
            return base + (i << 3) + bit;
        }
    }
    free_kernel_page((uint32_t)buf);
    return 0;
}

static int ext2_bitmap_clear(uint32_t blk, uint32_t bit) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    uint32_t byte = bit >> 3;
    uint8_t mask = (uint8_t)(0x80u >> (bit & 7));
    memset(buf, 0, 4096);
    ext2_read_block(blk, buf);
    int was_set = (buf[byte] & mask) != 0;
    if (was_set) {
        buf[byte] &= (uint8_t)~mask;
        ext2_write_block(blk, buf);
    }
    free_kernel_page((uint32_t)buf);
    return was_set ? 0 : 1;
}

void ext2_free_block(uint32_t blk) {
    if (blk == 0 || blk >= total_blocks) {
        return;
    }
    uint32_t per = bs * 8;
    if (ext2_bitmap_clear(block_bitmap_blk + blk / per, blk % per) == 0) {
        free_blocks++;
    }
}

static uint32_t ext2_alloc_inode(void) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return 0;
    }
    memset(buf, 0, 4096);
    ext2_read_block(inode_bitmap_blk, buf);
    for (uint32_t i = 0; i < bs; i++) {
        uint8_t inv = (uint8_t)~buf[i];
        if (inv == 0) {
            continue;
        }
        uint32_t bit = (uint32_t)__builtin_clz(inv) - 24;
        uint32_t idx = (i << 3) + bit + 1;
        if (idx > inodes_per_group) {
            break;
        }
        buf[i] |= (uint8_t)(0x80u >> bit);
        ext2_write_block(inode_bitmap_blk, buf);
        free_inodes--;
        free_kernel_page((uint32_t)buf);
        return idx;
    }
    free_kernel_page((uint32_t)buf);
    return 0;
}

void ext2_free_inode(uint32_t ino) {
    if (ino == 0 || ino > inodes_per_group) {
        return;
    }
    if (ext2_bitmap_clear(inode_bitmap_blk, ino - 1) == 0) {
        free_inodes++;
    }
}

int ext2_write_inode(uint32_t ino, const struct inode *in) {
    lock_acquire(&ext2_lock);
    int rc = ext2_write_inode_impl(ino, in);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_write_inode_impl(uint32_t ino, const struct inode *in) {
    if (disk == NULL || ino == 0 || ino > inodes_per_group) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, 4096);
    uint32_t per_block = bs / EXT2_INODE_SIZE;
    uint32_t blk = inode_table_blk + (ino - 1) / per_block;
    uint32_t off = ((ino - 1) % per_block) * EXT2_INODE_SIZE;
    ext2_read_block(blk, buf);
    uint8_t *p = buf + off;
    *(uint16_t *)(p + 0) = (uint16_t)in->i_mode;
    *(uint16_t *)(p + 2) = (uint16_t)in->i_uid;
    *(uint16_t *)(p + 24) = (uint16_t)in->i_gid;
    *(uint32_t *)(p + 4) = in->i_size;
    for (uint32_t bi = 0; bi < 15; bi++) {
        *(uint32_t *)(p + 40 + 4 * bi) = in->i_block[bi];
    }
    ext2_write_block(blk, buf);
    free_kernel_page((uint32_t)buf);
    return 0;
}

static uint32_t ext2_walk(uint32_t root, uint32_t fblk, uint32_t span,
                          uint8_t *buf, int alloc) {
    memset(buf, 0, 4096);
    ext2_read_block(root, buf);
    uint32_t idx = fblk / span;
    uint32_t child = *(uint32_t *)(buf + 4 * idx);
    if (child == 0) {
        if (!alloc) {
            return 0;
        }
        child = ext2_alloc_block();
        if (child == 0) {
            return 0;
        }
        *(uint32_t *)(buf + 4 * idx) = child;
        ext2_write_block(root, buf);
        if (span > 1) {
            memset(buf, 0, 4096);
            ext2_write_block(child, buf);
        }
    }
    if (span == 1) {
        return child;
    }
    return ext2_walk(child, fblk % span, span / (bs / 4u), buf, alloc);
}

static int ext2_block_of(struct inode *ino, uint32_t fblk, int alloc,
                         uint32_t *out) {
    uint32_t addrs = bs / 4u;
    if (fblk < 12) {
        uint32_t b = ino->i_block[fblk];
        if (b == 0 && alloc) {
            b = ext2_alloc_block();
            ino->i_block[fblk] = b;
        }
        if (b == 0) {
            return -1;
        }
        *out = b;
        return 0;
    }
    fblk -= 12;
    uint32_t slot = 12;
    uint32_t span = 1;
    if (fblk >= addrs) {
        fblk -= addrs;
        slot = 13;
        span = addrs;
    }
    uint32_t root = ino->i_block[slot];
    int fresh = 0;
    if (root == 0 && alloc) {
        root = ext2_alloc_block();
        ino->i_block[slot] = root;
        fresh = 1;
    }
    if (root == 0) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    if (fresh) {
        memset(buf, 0, 4096);
        ext2_write_block(root, buf);
    }
    uint32_t b = ext2_walk(root, fblk, span, buf, alloc);
    free_kernel_page((uint32_t)buf);
    if (b == 0) {
        return -1;
    }
    *out = b;
    return 0;
}

static uint32_t ext2_ensure_block(struct inode *ino, uint32_t fblk) {
    uint32_t b = 0;
    ext2_block_of(ino, fblk, 1, &b);
    return b;
}

int ext2_write_to_inode(struct inode *ino, uint32_t off, const void *buf,
                        uint32_t count) {
    lock_acquire(&ext2_lock);
    int rc = ext2_write_to_inode_impl(ino, off, buf, count);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_write_to_inode_impl(struct inode *ino, uint32_t off,
                                    const void *buf, uint32_t count) {
    if (disk == NULL || ino == 0 || ino->i_no == 0) {
        return 0;
    }
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return 0;
    }
    uint32_t done = 0;
    while (done < count) {
        uint32_t fblk = (off + done) / bs;
        uint32_t within = (off + done) % bs;
        uint32_t addr = ext2_ensure_block((struct inode *)ino, fblk);
        if (addr == 0) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t chunk = bs - within;
        if (chunk > count - done) {
            chunk = count - done;
        }
        memcpy(blk + within, (const uint8_t *)buf + done, chunk);
        ext2_write_block(addr, blk);
        done += chunk;
    }
    free_kernel_page((uint32_t)blk);
    if (off + done > ino->i_size) {
        ino->i_size = off + done;
    }
    if (done > 0) {
        ext2_write_inode(ino->i_no, (struct inode *)ino);
    }
    return (int)done;
}

void ext2_truncate_inode(struct inode *ino) {
    lock_acquire(&ext2_lock);
    ext2_truncate_inode_impl(ino);
    lock_release(&ext2_lock);
}
static void ext2_free_tree(uint32_t root, uint32_t span, uint8_t *buf) {
    memset(buf, 0, 4096);
    ext2_read_block(root, buf);
    for (uint32_t i = 0; i < bs / 4u; i++) {
        uint32_t child = *(uint32_t *)(buf + 4 * i);
        if (child == 0) {
            continue;
        }
        if (span == 1) {
            ext2_free_block(child);
        } else {
            ext2_free_tree(child, span / (bs / 4u), buf);
        }
    }
    ext2_free_block(root);
}
static void ext2_truncate_inode_impl(struct inode *ino) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return;
    }
    for (uint32_t i = 0; i < 12; i++) {
        if (ino->i_block[i]) {
            ext2_free_block(ino->i_block[i]);
            ino->i_block[i] = 0;
        }
    }
    if (ino->i_block[12]) {
        ext2_free_tree(ino->i_block[12], 1, buf);
        ino->i_block[12] = 0;
    }
    if (ino->i_block[13]) {
        ext2_free_tree(ino->i_block[13], bs / 4u, buf);
        ino->i_block[13] = 0;
    }
    if (ino->i_block[14]) {
        ext2_free_block(ino->i_block[14]);
        ino->i_block[14] = 0;
    }
    free_kernel_page((uint32_t)buf);
    memset(ino->i_block, 0, sizeof(ino->i_block));
    ino->i_size = 0;
}

static int ext2_map_block(const struct inode *ino, uint32_t fblk,
                          uint32_t *out);

int ext2_new_inode(uint32_t mode, struct inode *out) {
    lock_acquire(&ext2_lock);
    uint32_t rc = ext2_new_inode_impl(mode, out);
    lock_release(&ext2_lock);
    return rc;
}
static uint32_t ext2_new_inode_impl(uint32_t mode, struct inode *out) {
    uint32_t ino = ext2_alloc_inode();
    if (ino == 0) {
        return 0;
    }
    memset(out, 0, sizeof(struct inode));
    out->i_no = ino;
    out->i_mode = mode;
    out->i_size = 0;
    memset(out->i_block, 0, sizeof(out->i_block));
    if (ext2_write_inode(ino, out)) {
        ext2_free_inode(ino);
        return 0;
    }
    return (int)ino;
}

int ext2_add_entry(struct inode *dino, uint32_t ino, const char *name,
                   int is_dir) {
    return ext2_add_entry_dt(dino, ino, name,
                             is_dir ? (uint8_t)EXT2_DT_DIR : 1u);
}
int ext2_add_entry_dt(struct inode *dino, uint32_t ino, const char *name,
                      uint8_t dtype) {
    lock_acquire(&ext2_lock);
    int rc = ext2_add_entry_impl(dino, ino, name, dtype);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_add_entry_impl(struct inode *dino, uint32_t ino,
                               const char *name, uint8_t dtype) {
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl >= 255u) {
        return -1;
    }
    uint32_t need = (8u + nl + 3u) & ~3u;
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }

    for (uint32_t fblk = 0; fblk * bs < dino->i_size; fblk++) {
        uint32_t addr = 0;
        if (ext2_map_block((struct inode *)dino, fblk, &addr)) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t off = 0;
        uint32_t target = 0xFFFFFFFFu;
        uint32_t slot_rec = 0;
        while (off < bs) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u) {
                if (bs - off >= need) {
                    target = off;
                    slot_rec = bs - off;
                }
                break;
            }
            if (de->inode == 0 && rl >= need) {
                target = off;
                slot_rec = rl;
                break;
            }
            off += rl;
        }
        if (target != 0xFFFFFFFFu) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + target);
            de->inode = ino;
            de->rec_len = (uint16_t)slot_rec;
            de->name_len = (uint8_t)nl;
            de->file_type = dtype;
            memcpy(de->name, name, nl);
            ext2_write_block(addr, blk);
            free_kernel_page((uint32_t)blk);
            return 0;
        }
    }

    uint32_t nfblk = dino->i_size / bs;
    uint32_t naddr = ext2_ensure_block((struct inode *)dino, nfblk);
    if (naddr == 0) {
        free_kernel_page((uint32_t)blk);
        return -1;
    }
    memset(blk, 0, 4096);
    struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)blk;
    de->inode = ino;
    de->rec_len = (uint16_t)bs;
    de->name_len = (uint8_t)nl;
    de->file_type = dtype;
    memcpy(de->name, name, nl);
    ext2_write_block(naddr, blk);
    dino->i_size += bs;
    ext2_write_inode(dino->i_no, (struct inode *)dino);
    free_kernel_page((uint32_t)blk);
    return 0;
}

int ext2_remove_entry(struct inode *dino, const char *name) {
    lock_acquire(&ext2_lock);
    int rc = ext2_remove_entry_impl(dino, name);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_remove_entry_impl(struct inode *dino, const char *name) {
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl >= 255u) {
        return -1;
    }
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }
    for (uint32_t fblk = 0; fblk * bs < dino->i_size; fblk++) {
        uint32_t addr = 0;
        if (ext2_map_block((struct inode *)dino, fblk, &addr)) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t off = 0;
        while (off < bs) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u) {
                break;
            }
            if (de->inode != 0 && de->name_len == nl &&
                memcmp(de->name, name, nl) == 0) {
                de->inode = 0;
                de->name_len = 0;
                ext2_write_block(addr, blk);
                free_kernel_page((uint32_t)blk);
                return 0;
            }
            off += rl;
        }
        free_kernel_page((uint32_t)blk);
    }
    free_kernel_page((uint32_t)blk);
    return -1;
}

static int ext2_map_block(const struct inode *ino, uint32_t fblk,
                          uint32_t *out) {
    return ext2_block_of((struct inode *)ino, fblk, 0, out);
}

int ext2_read_from_inode(const struct inode *ino, uint32_t off, void *buf,
                         uint32_t count) {
    lock_acquire(&ext2_lock);
    int rc = ext2_read_from_inode_impl(ino, off, buf, count);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_read_from_inode_impl(const struct inode *ino, uint32_t off,
                                     void *buf, uint32_t count) {
    if (ino->i_no == 0 || off >= ino->i_size) {
        return 0;
    }
    if (off + count > ino->i_size) {
        count = ino->i_size - off;
    }
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return 0;
    }
    uint32_t done = 0;
    while (done < count) {
        uint32_t fblk = (off + done) / bs;
        uint32_t within = (off + done) % bs;
        uint32_t addr = 0;
        if (ext2_map_block(ino, fblk, &addr)) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t chunk = bs - within;
        if (chunk > count - done) {
            chunk = count - done;
        }
        memcpy((uint8_t *)buf + done, blk + within, chunk);
        done += chunk;
    }
    free_kernel_page((uint32_t)blk);
    return (int)done;
}

int ext2_dir_next(const struct inode *dino, uint32_t *pos,
                  struct dir_entry *out) {
    lock_acquire(&ext2_lock);
    int rc = ext2_dir_next_impl(dino, pos, out);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_dir_next_impl(const struct inode *dino, uint32_t *pos,
                              struct dir_entry *out) {
    while (*pos < dino->i_size) {
        uint32_t fblk = *pos / bs;
        uint32_t addr = 0;
        if (ext2_map_block(dino, fblk, &addr)) {
            *pos = (fblk + 1) * bs;
            continue;
        }
        uint8_t *blk = (uint8_t *)get_kernel_pages(1);
        if (blk == NULL) {
            return -1;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t off = 0;
        while (off < bs) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + off);
            uint32_t rec_len = de->rec_len;
            if (rec_len < 8) {
                break;
            }
            uint32_t abs = fblk * bs + off;
            if (de->inode != 0 && de->name_len > 0 && de->name_len < 255 &&
                abs >= *pos) {
                uint32_t nl = de->name_len;
                if (nl >= MAX_FILE_NAME_LEN) {
                    nl = MAX_FILE_NAME_LEN - 1;
                }
                memset(out->filename, 0, MAX_FILE_NAME_LEN);
                memcpy(out->filename, de->name, nl);
                out->i_no = de->inode;
                out->f_type = de->file_type == EXT2_DT_DIR  ? FT_DIRECTORY
                              : de->file_type == 2u         ? FT_CHARDEVICE
                              : de->file_type == 7u         ? FT_SYMLINK
                                                            : FT_REGULAR;
                *pos = abs + rec_len;
                free_kernel_page((uint32_t)blk);
                return 0;
            }
            off += rec_len;
        }
        free_kernel_page((uint32_t)blk);
        *pos = (fblk + 1) * bs;
    }
    return -1;
}

static int ext2_find_in_dir(const struct inode *dino, const char *name,
                            uint32_t *child, int *ftype) {
    uint32_t pos = 0;
    struct dir_entry de;
    while (ext2_dir_next(dino, &pos, &de) == 0) {
        if (strcmp(de.filename, name) == 0) {
            *child = de.i_no;
            *ftype = (int)de.f_type;
            return 0;
        }
    }
    return -1;
}

static int ext2_read_target(uint32_t ino, char *buf, uint32_t cap) {
    struct inode node;
    if (ext2_read_inode_impl(ino, &node) ||
        (node.i_mode & 0xF000u) != 0xA000u) {
        return -1;
    }
    uint32_t len = node.i_size < cap - 1 ? node.i_size : cap - 1;
    if (node.i_size < 60u) {
        memcpy(buf, &node.i_block[0], len);
    } else if (ext2_read_from_inode_impl(&node, 0, buf, len) != (int)len) {
        return -1;
    }
    buf[len] = 0;
    return (int)len;
}
int ext2_lookup(const char *path, uint32_t *ino, int *is_dir) {
    int ft = 0;
    lock_acquire(&ext2_lock);
    int rc = ext2_lookup_depth(path, ino, &ft, 1, 8);
    lock_release(&ext2_lock);
    *is_dir = (ft == FT_DIRECTORY);
    return rc;
}
int ext2_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow) {
    lock_acquire(&ext2_lock);
    int rc = ext2_lookup_depth(path, ino, ftype, follow, 8);
    lock_release(&ext2_lock);
    return rc;
}
int ext2_read_link_target(uint32_t ino, char *buf, uint32_t cap) {
    lock_acquire(&ext2_lock);
    int rc = ext2_read_target(ino, buf, cap);
    lock_release(&ext2_lock);
    return rc;
}
static int ext2_lookup_depth(const char *path, uint32_t *ino, int *ftype,
                             int follow, int depth) {
    if (disk == NULL || path == NULL || path[0] != '/') {
        return -1;
    }
    char cur[MAX_PATH_LEN];
    uint32_t clen = (uint32_t)strlen(path);
    if (clen >= MAX_PATH_LEN) {
        return -1;
    }
    memcpy(cur, path, clen + 1);
    for (;;) {
        if (--depth < 0) {
            return -1;
        }
        uint32_t cino = 2;
        int cdir = 1;
        *ino = 2;
        *ftype = FT_DIRECTORY;
        const char *p = cur;
        while (*p == '/') {
            p++;
        }
        if (*p == 0) {
            return 0;
        }
        struct inode node;
        if (ext2_read_inode_impl(2, &node)) {
            return -1;
        }
        for (;;) {
            const char *cstart = p;
            char comp[MAX_FILE_NAME_LEN];
            uint32_t ci = 0;
            while (*p && *p != '/' && ci < MAX_FILE_NAME_LEN - 1) {
                comp[ci++] = *p++;
            }
            comp[ci] = 0;
            if (ci == 0) {
                break;
            }
            uint32_t child = 0;
            int ft = 0;
            if (ext2_find_in_dir(&node, comp, &child, &ft)) {
                return -1;
            }
            if (ft == FT_SYMLINK) {
                char tgt[MAX_PATH_LEN];
                if (!follow || ext2_read_target(child, tgt, MAX_PATH_LEN) < 0) {
                    *ino = child;
                    *ftype = ft;
                    cdir = 0;
                    return 0;
                }
                char np[MAX_PATH_LEN];
                uint32_t plen = (uint32_t)(cstart - cur);
                uint32_t tlen = (uint32_t)strlen(tgt);
                if (tgt[0] == '/') {
                    plen = 0;
                } else {
                    while (plen > 1 && cur[plen - 1] == '/') {
                        plen--;
                    }
                }
                if (plen + tlen + 2 > MAX_PATH_LEN) {
                    return -1;
                }
                memcpy(np, cur, plen);
                if (plen > 1 || (plen == 1 && cur[0] != '/')) {
                    np[plen++] = '/';
                } else if (plen == 0) {
                    np[plen++] = '/';
                }
                memcpy(np + plen, tgt, tlen + 1);
                memcpy(cur, np, plen + tlen + 1);
                break;
            }
            cino = child;
            cdir = (ft == FT_DIRECTORY);
            while (*p == '/') {
                p++;
            }
            if (*p == 0) {
                *ino = cino;
                *ftype = ft;
                return 0;
            }
            if (!cdir || ext2_read_inode_impl(child, &node)) {
                return -1;
            }
        }
        *ino = cino;
        *ftype = cdir ? FT_DIRECTORY : FT_REGULAR;
    }
}

void ext2_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                      uint32_t *files, uint32_t *ffree) {
    if (bsize) *bsize = bs;
    if (blocks) *blocks = total_blocks;
    if (bfree) *bfree = free_blocks;
    if (files) *files = inodes_per_group;
    if (ffree) *ffree = free_inodes;
}
