#include "kernel/syscall/file_syscall.h"
#include "drivers/block/ide.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "kernel/fs/proc.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "drivers/net/socket.h"
#include "kernel/sched/thread.h"
struct linux_dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    char d_name[1];
};
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
static struct file *fd_lookup(int32_t fd) {
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC) {
        return NULL;
    }
    uint32_t global_fd = current->fd_table[fd];
    if (global_fd == (uint32_t)-1 || global_fd >= MAX_FILE_OPEN) {
        return NULL;
    }
    return file_get(global_fd);
}

int32_t sys_fstat(int32_t fd, void *buf) {
    if (buf == NULL) {
        return -1;
    }
    struct file *pf = fd_lookup(fd);
    if (pf == NULL || (pf->fd_inode == NULL && pf->proc_id == 0)) {
        return -1;
    }
    struct stat *st = (struct stat *)buf;
    memset(st, 0, sizeof(*st));
    if (pf->proc_id != 0) {
        return proc_fstat(pf, st);
    }
    st->st_ino = pf->fd_inode->i_no;
    st->st_size = pf->fd_inode->i_size;
    st->st_filetype = FT_REGULAR;
    return 0;
}
int32_t sys_dup(int32_t oldfd) {
    struct file *pf = fd_lookup(oldfd);
    if (pf == NULL) {
        return -1;
    }
    uint32_t global_fd = (uint32_t)(pf - file_table);
    lock_acquire(&file_table_lock);
    int newfd = fd_install((int32_t)global_fd);
    if (newfd == -1) {
        lock_release(&file_table_lock);
        return -1;
    }
    file_table_ref(global_fd);
    lock_release(&file_table_lock);
    return newfd;
}
int32_t sys_dup2(int32_t oldfd, int32_t newfd) {
    if (newfd < 0 || newfd >= (int32_t)MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    struct file *pf = fd_lookup(oldfd);
    if (pf == NULL) {
        return -1;
    }
    uint32_t global_fd = (uint32_t)(pf - file_table);

    lock_acquire(&file_table_lock);
    if (current->fd_table[newfd] != (uint32_t)-1) {
        close_file(newfd);
    }
    current->fd_table[newfd] = global_fd;
    file_table_ref(global_fd);
    lock_release(&file_table_lock);
    return newfd;
}
int32_t sys_fcntl(int32_t fd, int32_t cmd, uint32_t arg) {
    if (net_is_socket(fd))
        return net_fcntl(fd, cmd, arg);
    struct file *pf = fd_lookup(fd);
    if (pf == NULL) {
        return -1;
    }
    switch (cmd) {
    case F_DUPFD:
        (void)arg;
        return sys_dup(fd);
    case F_GETFD:
        return (int32_t)((current->fd_cloexec >> fd) & 1);
    case F_SETFD:
        if (arg & 1)
            current->fd_cloexec |= (1ull << fd);
        else
            current->fd_cloexec &= ~(1ull << fd);
        return 0;
    case F_GETFL:
        return (int32_t)pf->fd_flag;
    case F_SETFL:
        pf->fd_flag = arg;
        return 0;
    default:
        return -1;
    }
}
int32_t sys_getdents(int32_t fd, void *dirp, uint32_t count) {
    if (dirp == NULL) {
        return -1;
    }
    struct file *pf = fd_lookup(fd);
    if (pf == NULL || pf->fd_inode == NULL) {
        return -1;
    }
    uint32_t pos = 0;
    struct dir_entry de;
    uint32_t written = 0;
    while (ext2_dir_next(pf->fd_inode, &pos, &de) == 0) {
        uint32_t name_len = strlen(de.filename);
        uint16_t reclen = (uint16_t)(10u + name_len + 1u);
        if (written + reclen > count) {
            break;
        }
        struct linux_dirent *ld =
            (struct linux_dirent *)((uint8_t *)dirp + written);
        ld->d_ino = de.i_no;
        ld->d_off = written + reclen;
        ld->d_reclen = reclen;
        memcpy(ld->d_name, de.filename, name_len + 1);
        written += reclen;
    }
    return (int32_t)written;
}
int32_t sys_readlink(const char *path, char *buf, uint32_t bufsiz) {
    if (path == NULL || buf == NULL || bufsiz == 0) {
        return -1;
    }
    uint32_t ino = 0;
    int ft = 0;
    if (ext2_lookup_ftype(path, &ino, &ft, 0) || ft != FT_SYMLINK) {
        current->errno = 22;
        return -1;
    }
    char kbuf[MAX_PATH_LEN];
    int len = ext2_read_link_target(ino, kbuf, MAX_PATH_LEN);
    if (len < 0) {
        current->errno = 22;
        return -1;
    }
    uint32_t n = (uint32_t)len < bufsiz ? (uint32_t)len : bufsiz;
    memcpy(buf, kbuf, n);
    return (int32_t)n;
}
int32_t sys_access(const char *path, int32_t mode) {
    if (path == NULL) {
        return -1;
    }
    (void)mode;
    if (proc_access(path) == 0) {
        return 0;
    }
    int inode_no = search_file(path);
    if (inode_no == -1) {
        current->errno = 2;
        return -1;
    }
    return 0;
}
int32_t sys_rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    current->errno = 30;
    return -1;
}
int32_t sys_truncate(const char *path, int32_t length) {
    (void)path;
    (void)length;
    current->errno = 30;
    return -1;
}
int32_t sys_chmod(const char *path, uint32_t mode) {
    if (path == NULL) {
        return -1;
    }
    (void)mode;
    if (proc_access(path) == 0) {
        return 0;
    }
    int inode_no = search_file(path);
    if (inode_no == -1) {
        current->errno = 2;
        return -1;
    }
    return 0;
}
