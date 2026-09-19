#include "kernel/userprog/wait_exit.h"
#include "kernel/signal.h"
#include "kernel/syscall/linux_abi.h"
#include "kernel/assert.h"
#include "kernel/fs/file.h"
#include "kernel/mm/access.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/syscall/futex.h"
#include "kernel/userprog/process.h"
#include "lib/list/list.h"

static void release_prog_resource(struct TASK *release_thread) {
    if (release_thread->clear_child_tid != 0) {
        uint32_t addr = release_thread->clear_child_tid;
        release_thread->clear_child_tid = 0;
        if (access_ok((const void *)(uintptr_t)addr, 4, 1)) {
            *(volatile int32_t *)(uintptr_t)addr = 0;
            if (release_thread == current) {
                sys_futex(addr, FUTEX_WAKE, 0x7FFFFFFF, 0);
            }
        }
    }
    task_release_space(release_thread);
    for (uint32_t fd_idx = 3; fd_idx < MAX_FILES_OPEN_PER_PROC; fd_idx++) {
        if (release_thread->fd_table[fd_idx] != (uint32_t)-1) {
            close_file((int)fd_idx);
        }
    }
}

void kill_orphan_children(int32_t parent_pid) {
    struct LIST_ELEM *e = thread_all_list.head.next;
    while (e != &thread_all_list.tail) {
        struct TASK *t = list_entry(e, struct TASK, all_list_tag);
        struct LIST_ELEM *next = e->next;
        if (t->parent_pid == parent_pid && t->status != TASK_DIED) {
            if (t->status != TASK_HANGING)
                release_prog_resource(t);
            thread_exit(t, 0);
        }
        e = next;
    }
}

static int find_hanging_child(struct LIST_ELEM *pelem, int32_t ppid) {
    struct TASK *t = list_entry(pelem, struct TASK, all_list_tag);
    return (t->parent_pid == ppid && t->status == TASK_HANGING);
}

static int find_child(struct LIST_ELEM *pelem, int32_t ppid) {
    struct TASK *t = list_entry(pelem, struct TASK, all_list_tag);
    return (t->parent_pid == ppid);
}

pid_t sys_wait(int32_t *status) {
    struct TASK *parent = current;
    int32_t ignored_status;
    if (status == NULL) {
        status = &ignored_status;
    }
    for (;;) {
        struct LIST_ELEM *e = thread_all_list.head.next;
        while (e != &thread_all_list.tail) {
            struct LIST_ELEM *next = e->next;
            if (find_hanging_child(e, (int32_t)parent->pid)) {
                struct TASK *child =
                    list_entry(e, struct TASK, all_list_tag);
                *status = child->exit_status;
                uint32_t child_pid = child->pid;
                thread_exit(child, 0);
                return child_pid;
            }
            e = next;
        }
        struct LIST_ELEM *child = thread_all_list.head.next;
        while (child != &thread_all_list.tail) {
            if (find_child(child, (int32_t)parent->pid)) {
                break;
            }
            child = child->next;
        }
        if (child == &thread_all_list.tail) {
            return -1;
        }
        thread_block_with_status(TASK_WAITING);
    }
}

void proc_exit(struct TASK *cur, int status) {
    cur->exit_status = status;

    kill_orphan_children((int32_t)cur->pid);
    release_prog_resource(cur);
    struct TASK *parent = pid2thread(cur->parent_pid);
    if (parent && parent->status == TASK_WAITING) {
        thread_unblock(parent);
    }
    if (cur->parent_pid < 0) {
        thread_exit_current();
        return;
    }
    thread_block_with_status(TASK_HANGING);
}

void sys_exit(int32_t status) {
    proc_exit(current, status);
}

static int waitid_match(struct TASK *t, int idtype, int32_t id) {
    if (idtype == LINUX_P_PID) {
        return t->pid == (uint32_t)id;
    }
    if (idtype == LINUX_P_PGID) {
        return (t->pgid ? t->pgid : t->pid) == (uint32_t)id;
    }
    return 1;
}

int sys_waitid(int idtype, int32_t id, struct LINUX_SIGINFO *info,
               uint32_t options) {
    struct TASK *parent = current;
    for (;;) {
        int any_child = 0;
        struct LIST_ELEM *e = thread_all_list.head.next;
        while (e != &thread_all_list.tail) {
            struct TASK *t =
                list_entry(e, struct TASK, all_list_tag);
            struct LIST_ELEM *next = e->next;
            if (t->parent_pid != (int32_t)parent->pid ||
                t->status == TASK_DIED) {
                e = next;
                continue;
            }
            any_child = 1;
            if (waitid_match(t, idtype, id) && t->status == TASK_HANGING) {
                if (info != NULL) {
                    int sig = t->exit_status >= 128 ? t->exit_status - 128 : 0;
                    info->si_signo = SIGCHLD;
                    info->si_errno = 0;
                    info->si_code =
                        sig ? LINUX_CLD_KILLED : LINUX_CLD_EXITED;
                    info->si_pid = (int32_t)t->pid;
                    info->si_uid = 0;
                    info->si_status = sig ? sig : t->exit_status;
                    info->si_utime = t->elapsed_ticks;
                    info->si_stime = 0;
                }
                if (!(options & LINUX_WNOWAIT)) {
                    thread_exit(t, 0);
                }
                return 0;
            }
            e = next;
        }
        if (!any_child) {
            return -1;
        }
        if (options & LINUX_WNOHANG) {
            if (info != NULL) {
                info->si_signo = 0;
            }
            return 0;
        }
        thread_block_with_status(TASK_WAITING);
    }
}
