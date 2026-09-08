#include "kernel/fs/proc.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"

enum { PROC_NONE, PROC_DIR, PROC_MEMINFO, PROC_STAT, PROC_STATUS };

int proc_match(const char *path) {
    if (path == NULL) {
        return 0;
    }
    if (strcmp(path, "/proc") == 0) {
        return 1;
    }
    return strncmp(path, "/proc/", 6) == 0;
}

static uint32_t proc_pid;
static uint32_t proc_pid_valid;

static uint32_t proc_task_of(uint32_t pid) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].slot_used && task_table[i].status != TASK_DIED &&
            task_table[i].pid == pid)
            return i;
    }
    return MAX_TASKS;
}

static int proc_node_of(const char *path) {
    if (strcmp(path, "/proc") == 0) {
        return PROC_DIR;
    }
    if (strcmp(path, "/proc/meminfo") == 0) {
        return PROC_MEMINFO;
    }
    proc_pid_valid = 0;
    const char *p = path + 6;
    uint32_t pid = 0;
    if (strncmp(p, "self", 4) == 0 && current) {
        pid = current->pid;
        p += 4;
    } else {
        if (*p < '0' || *p > '9')
            return PROC_NONE;
        while (*p >= '0' && *p <= '9')
            pid = pid * 10u + (uint32_t)(*p++ - '0');
    }
    if (*p != '/')
        return PROC_NONE;
    p++;
    if (proc_task_of(pid) == MAX_TASKS)
        return PROC_NONE;
    proc_pid = pid;
    proc_pid_valid = 1;
    if (strcmp(p, "stat") == 0)
        return PROC_STAT;
    if (strcmp(p, "status") == 0)
        return PROC_STATUS;
    return PROC_NONE;
}

static uint32_t meminfo_build(char *dst, uint32_t cap) {
    uint32_t bytes = kernel_pool.pool_bitmap.btmp_bytes_len;
    uint32_t used = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        used += __builtin_popcount(kernel_pool.pool_bitmap.bits[i]);
    }
    uint32_t nframes = bytes * 8;
    uint32_t total_kb = nframes * (PAGE_SIZE / 1024);
    uint32_t free_kb = (nframes - used) * (PAGE_SIZE / 1024);
    uint32_t used_kb = used * (PAGE_SIZE / 1024);
    (void)cap;
    return sprintf(dst,
                   "MemTotal:     %d kB\n"
                   "MemFree:      %d kB\n"
                   "MemUsed:      %d kB\n",
                   total_kb, free_kb, used_kb);
}

static uint32_t procstat_build(char *dst, uint32_t cap, uint32_t slot) {
    struct task_struct *t = &task_table[slot];
    char state = t->status == TASK_RUNNING ? 'R'
                 : (t->status == TASK_HANGING || t->status == TASK_DIED)
                     ? 'Z'
                     : (t->status == TASK_STOPPED ? 'T' : 'S');
    return sprintf(dst, "%d (%s) %c %d %d %d 0 0 0 0 0 0 0 0 %d 0 0 0\n",
                   t->pid, t->name, state,
                   t->parent_pid > 0 ? t->parent_pid : 1,
                   t->pgid ? t->pgid : t->pid, t->sid ? t->sid : t->pid,
                   t->elapsed_ticks);
}
static uint32_t procstatus_build(char *dst, uint32_t cap, uint32_t slot) {
    struct task_struct *t = &task_table[slot];
    return sprintf(dst,
                   "Name:\t%s\nPid:\t%d\nPPid:\t%d\nUid:\t%d %d %d\n"
                   "Gid:\t%d %d %d\n",
                   t->name, t->pid, t->parent_pid > 0 ? t->parent_pid : 1,
                   t->uid, t->euid, t->suid, t->gid, t->egid, t->sgid);
}
static uint32_t proc_size(int node) {
    char buf[256];
    if (node == PROC_MEMINFO) {
        return meminfo_build(buf, sizeof(buf));
    }
    if (node == PROC_STAT && proc_pid_valid) {
        return procstat_build(buf, sizeof(buf), proc_task_of(proc_pid));
    }
    if (node == PROC_STATUS && proc_pid_valid) {
        return procstatus_build(buf, sizeof(buf), proc_task_of(proc_pid));
    }
    return 0;
}

int proc_open(const char *path, uint8_t flags) {
    int node = proc_node_of(path);
    if (node == PROC_NONE) {
        return -1;
    }
    int gfd = file_table_alloc_slot();
    if (gfd == -1) {
        return -1;
    }
    struct file *file = file_get((uint32_t)gfd);
    file->fd_pos = 0;
    file->fd_flag = flags;
    file->fd_inode = NULL;
    file->proc_id = (uint32_t)node;
    file->proc_aux = (node == PROC_STAT || node == PROC_STATUS)
                         ? proc_pid
                         : 0;
    file->ref_cnt = 1;
    int fd = fd_install(gfd);
    if (fd == -1) {
        file_table_free_slot(gfd);
        return -1;
    }
    return fd;
}

uint32_t proc_read(struct file *file, void *buf, uint32_t count) {
    char info[256];
    uint32_t len;
    if (file->proc_id == PROC_MEMINFO) {
        len = meminfo_build(info, sizeof(info));
    } else if (file->proc_id == PROC_STAT || file->proc_id == PROC_STATUS) {
        uint32_t slot = proc_task_of(file->proc_aux);
        if (slot == MAX_TASKS)
            return 0;
        len = file->proc_id == PROC_STAT
                  ? procstat_build(info, sizeof(info), slot)
                  : procstatus_build(info, sizeof(info), slot);
    } else {
        return 0;
    }
    if (file->fd_pos >= len) {
        return 0;
    }
    uint32_t remain = len - file->fd_pos;
    uint32_t n = (count < remain) ? count : remain;
    memcpy(buf, info + file->fd_pos, n);
    file->fd_pos += n;
    return n;
}

int proc_stat(const char *path, struct stat *buf) {
    int node = proc_node_of(path);
    if (node == PROC_NONE) {
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->st_ino = 1;
    if (node == PROC_DIR) {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_size = 0;
    } else {
        buf->st_filetype = FT_REGULAR;
        buf->st_size = proc_size(node);
    }
    return 0;
}

int proc_fstat(struct file *file, struct stat *buf) {
    if (file->proc_id == PROC_NONE) {
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->st_ino = 1;
    if (file->proc_id == PROC_DIR) {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_size = 0;
    } else {
        buf->st_filetype = FT_REGULAR;
        buf->st_size = proc_size(file->proc_id);
    }
    return 0;
}

int proc_access(const char *path) {
    return proc_match(path) ? 0 : -1;
}

int proc_lseek(struct file *file, int32_t offset, uint8_t whence) {
    int32_t size = (int32_t)proc_size(file->proc_id);
    int32_t new_pos = 0;
    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos = (int32_t)file->fd_pos + offset;
    } else if (whence == SEEK_END) {
        new_pos = size + offset;
    } else {
        return -1;
    }
    if (new_pos < 0) {
        return -1;
    }
    file->fd_pos = (uint32_t)new_pos;
    return (int32_t)file->fd_pos;
}
