#include "kernel/fs/dir.h"

#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"

struct dir root_dir;

void open_root_dir(struct partition *part) {
    root_dir.inode = inode_open(part, 2);
    root_dir.dir_pos = 0;
}

#define MAX_OPEN_DIRS 16
static struct dir *dir_table[MAX_OPEN_DIRS];

struct dir *dir_open(struct partition *part, uint32_t inode_no) {
    struct dir *pdir = (struct dir *)get_kernel_pages(1);
    if (pdir == NULL) {
        return NULL;
    }
    memset(pdir, 0, PAGE_SIZE);
    pdir->inode = inode_open(part, inode_no);
    pdir->dir_pos = 0;
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (dir_table[i] == NULL) {
            dir_table[i] = pdir;
            return (struct dir *)(uintptr_t)(i + 1);
        }
    }
    inode_close(pdir->inode);
    free_kernel_page((uint32_t)pdir);
    return NULL;
}

static struct dir *dir_handle_get(struct dir *handle) {
    uint32_t idx = (uint32_t)(uintptr_t)handle;
    if (idx == 0 || idx > MAX_OPEN_DIRS || dir_table[idx - 1] == NULL) {
        return NULL;
    }
    return dir_table[idx - 1];
}

void dir_close(struct dir *handle) {
    struct dir *d = dir_handle_get(handle);
    if (d == NULL) {
        return;
    }
    dir_table[(uint32_t)(uintptr_t)handle - 1] = NULL;
    inode_close(d->inode);
    free_kernel_page((uint32_t)d);
}

struct dir_entry *dir_read(struct dir *handle) {
    struct dir *d = dir_handle_get(handle);
    if (d == NULL) {
        return NULL;
    }
    struct dir_entry *dir_e = (struct dir_entry *)d->dir_buf;
    if (ext2_dir_next(d->inode, &d->dir_pos, dir_e)) {
        return NULL;
    }
    return dir_e;
}

void dir_rewind(struct dir *handle) {
    struct dir *d = dir_handle_get(handle);
    if (d != NULL) {
        d->dir_pos = 0;
    }
}
