#include "kernel/shell/pipe.h"
#include "drivers/char/ioqueue.h"
#include "kernel/fs/file.h"
#include "kernel/sched/sync.h"
#include "kernel/asm_func.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
int32_t is_pipe(uint32_t local_fd) {
    struct FILE *file = file_get(fd_local2global(local_fd));
    if (file == NULL) {
        return 0;
    }
    return file->fd_flag == PIPE_FLAG;
}

int32_t sys_pipe(int32_t pipefd[2]) {
    int32_t global_fd = file_table_alloc_slot();
    if (global_fd == -1) {
        return -1;
    }
    struct FILE *file = file_get((uint32_t)global_fd);

    file->fd_flag = PIPE_FLAG;
    file->ref_cnt = 2;
    void *buf = get_kernel_pages(1);
    if (buf == NULL) {
        file_table_free_slot(global_fd);
        return -1;
    }
    ioq_init((struct TTY_IOQUEUE *)buf);
    file->fd_inode = (struct FS_INODE *)buf;
    file->fd_pos = 0;
    pipefd[0] = fd_install(global_fd);
    pipefd[1] = fd_install(global_fd);
    if (pipefd[0] == -1 || pipefd[1] == -1) {
        if (pipefd[0] != -1)
            fd_release((uint32_t)pipefd[0]);
        if (pipefd[1] != -1)
            fd_release((uint32_t)pipefd[1]);
        free_kernel_page((uint32_t)buf);
        file_table_free_slot(global_fd);
        return -1;
    }
    return 0;
}

uint32_t pipe_read(int32_t fd, void *buf, uint32_t count) {
    uint32_t global_fd = fd_local2global(fd);
    struct FILE *file = file_get(global_fd);
    if (file == NULL || file->fd_inode == NULL) {
        return 0;
    }
    struct TTY_IOQUEUE *ioq = (struct TTY_IOQUEUE *)file->fd_inode;
    uint32_t ioq_len = ioq_length(ioq);
    uint32_t size = (ioq_len > count) ? count : ioq_len;
    char *buffer = (char *)buf;
    uint32_t bytes_read = 0;
    asm_cli();
    while (bytes_read < size) {
        buffer[bytes_read] = ioq_getchar(ioq);
        ++bytes_read;
    }
    asm_sti();
    return bytes_read;
}

uint32_t pipe_write(int32_t fd, const void *buf, uint32_t count) {
    uint32_t global_fd = fd_local2global(fd);
    struct FILE *file = file_get(global_fd);
    if (file == NULL || file->fd_inode == NULL) {
        return 0;
    }
    struct TTY_IOQUEUE *ioq = (struct TTY_IOQUEUE *)file->fd_inode;
    uint32_t ioq_left = BUFSIZE - ioq_length(ioq);
    uint32_t size = (ioq_left > count) ? count : ioq_left;
    const char *buffer = (const char *)buf;
    uint32_t bytes_write = 0;
    asm_cli();
    while (bytes_write < size) {
        ioq_putchar(ioq, buffer[bytes_write]);
        ++bytes_write;
    }
    asm_sti();
    return bytes_write;
}

void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd) {
    struct TASK *cur = current;
    if (new_local_fd < 3) {
        cur->fd_table[old_local_fd] = new_local_fd;
    } else {
        uint32_t new_global_fd = cur->fd_table[new_local_fd];
        cur->fd_table[old_local_fd] = new_global_fd;
    }
}
