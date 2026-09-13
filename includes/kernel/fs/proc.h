#ifndef FS_PROC_H
#define FS_PROC_H
#include <stdint.h>
struct FILE;
struct FS_STAT;
int proc_match(const char *path);
int proc_open(const char *path, uint8_t flags);
uint32_t proc_read(struct FILE *file, void *buf, uint32_t count);
int proc_stat(const char *path, struct FS_STAT *buf);
int proc_fstat(struct FILE *file, struct FS_STAT *buf);
int proc_access(const char *path);
int proc_lseek(struct FILE *file, int32_t offset, uint8_t whence);
#endif
