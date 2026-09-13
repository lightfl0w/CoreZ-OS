#include "kernel/fs/inode.h"

#include "drivers/block/ide.h"
#include "kernel/asm_func.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/fs.h"

struct inode *inode_open(struct partition *part, uint32_t inode_no) {
    uint32_t old;
    struct RB_NODE *found;
    struct inode *inode;

    old = asm_save_eflags();
    asm_cli();
    found = rb_find(&part->open_inodes_rb, inode_no);
    if (found != NULL) {
        inode = rb_entry(found, struct inode, inode_rb_node);
        inode->i_open_cnt++;
        asm_restore_eflags(old);
        return inode;
    }
    asm_restore_eflags(old);

    inode = (struct inode *)get_kernel_pages(1);
    if (inode == NULL) {
        return NULL;
    }
    memset(inode, 0, PAGE_SIZE);
    if (ext2_read_inode(inode_no, inode)) {
        free_kernel_page((uint32_t)inode);
        return NULL;
    }
    inode->i_open_cnt = 1;
    inode->write_deny = 0;
    inode->inode_rb_node.key = inode_no;

    old = asm_save_eflags();
    asm_cli();
    found = rb_find(&part->open_inodes_rb, inode_no);
    if (found != NULL) {
        struct inode *existing = rb_entry(found, struct inode, inode_rb_node);
        existing->i_open_cnt++;
        asm_restore_eflags(old);
        free_kernel_page((uint32_t)inode);
        return existing;
    }
    rb_insert(&part->open_inodes_rb, &inode->inode_rb_node);
    asm_restore_eflags(old);
    return inode;
}

void inode_close(struct inode *inode) {
    if (inode == NULL || inode->i_open_cnt == 0) {
        return;
    }
    uint32_t old = asm_save_eflags();
    asm_cli();
    if (--inode->i_open_cnt == 0) {
        rb_erase(&cur_part->open_inodes_rb, &inode->inode_rb_node);
        inode->i_open_cnt = 0;
        free_kernel_page((uint32_t)inode);
    }
    asm_restore_eflags(old);
}
