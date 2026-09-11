#ifndef FS_FS_H
#define FS_FS_H

#include "drivers/block/ide.h"
#include <stdint.h>

#define MAX_FILES_PER_PART 4096
#define BITS_PER_SECTOR 4096
#define SECTOR_SIZE 512
#define BLOCK_SIZE SECTOR_SIZE
#define MAX_PATH_LEN 512

#define FS_MAGIC 0x19590318

enum file_types { FT_UNKNOWN, FT_REGULAR, FT_DIRECTORY, FT_CHARDEVICE,
                  FT_SYMLINK };

enum oflags { O_RDONLY, O_WRONLY, O_RDWR, O_CREAT = 4 };

enum whence { SEEK_SET = 1, SEEK_CUR, SEEK_END };

struct stat {
    uint32_t st_ino;
    uint32_t st_size;
    enum file_types st_filetype;
};

struct partition;
struct dir;
struct dir_entry;
struct inode;
extern struct partition *cur_part;

void filesys_init(void);

int32_t block_bitmap_alloc(struct partition *part);
void block_bitmap_free(struct partition *part, uint32_t lba);
int32_t inode_bitmap_alloc(struct partition *part);
void inode_bitmap_free(struct partition *part, uint32_t inode_no);

char *path_parse(char *pathname, char *name_store);
int search_dir_entry(struct partition *part, struct dir *pdir, const char *name,
                     struct dir_entry *dir_e);
int search_file(const char *pathname);
int create_file(const char *pathname);
int open_file(const char *pathname, uint8_t flags);
int close_file(int fd);
uint32_t read_file(int fd, void *buf, uint32_t count);
uint32_t write_file(int fd, const void *buf, uint32_t count);
int32_t sys_lseek(int32_t fd, int32_t offset, uint8_t whence);
int sys_unlink(const char *pathname);
int32_t sys_mkdir(const char *pathname);
struct dir *sys_opendir(const char *name);
int32_t sys_closedir(struct dir *dir);
struct dir_entry *sys_readdir(struct dir *dir);
void sys_rewinddir(struct dir *dir);
int32_t sys_rmdir(const char *pathname);
char *sys_getcwd(char *buf, uint32_t size);
int fs_cwd_abs_prefix(char *buf, uint32_t size);
int32_t sys_chdir(const char *path);
int32_t sys_stat(const char *path, struct stat *buf);
int32_t sys_mknod(const char *path, uint32_t mode, uint32_t dev);
int32_t sys_chown(const char *path, uint32_t uid, uint32_t gid);
int fs_stat_full(const char *path, uint32_t *ino_no, uint32_t *size,
                 uint32_t *mode, uint32_t *uid, uint32_t *gid);
int32_t sys_symlink(const char *target, const char *linkpath);
int fs_check_perm(const struct inode *ino, uint32_t bits);
int fs_is_chardev(const struct inode *ino);
uint32_t fs_chardev_dev(const struct inode *ino);

#endif
