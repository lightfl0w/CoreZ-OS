#include "kernel/shell/shell.h"

#include "drivers/char/console/io.h"
#include "kernel/assert.h"
#include "kernel/fs/fs.h"
#include "kernel/gui/gui.h"
#include "kernel/sched/thread.h"
#include "kernel/shell/buildin_cmd.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"
#include "libc/user/syscall.h"

#define MAX_ARG_NR 16

#define KBD_CTRL_U 0x01
#define KBD_CTRL_L 0x0C

static char cmd_line[MAX_PATH_LEN] = {0};

char final_path[MAX_PATH_LEN] = {0};

static char cwd_cache[64] = {0};

char *argv[MAX_ARG_NR];
static int32_t argc = -1;

void print_prompt(void) {
    printf("[corez@corez %s]$ ", cwd_cache);
}

static void shell_erase(void) {
    int x = get_cursor_x();
    int y = get_cursor_y();
    if (x <= 0)
        return;
    set_cursor(x - 8, y);
    console_putc(' ');
    set_cursor(x - 8, y);
}

static void readline(char *buf, int32_t count) {
    ASSERT(buf != NULL && count > 0);
    char *pos = buf;
    while (read(0, pos, 1) != -1 && (pos - buf) < count) {
        switch (*pos) {
        case '\n':
        case '\r':
            *pos = 0;
            putchar('\n');
            return;
        case '\b':
            if (pos > buf) {
                shell_erase();
                --pos;
                *pos = 0;
            }
            break;
        case KBD_CTRL_L:
            *pos = 0;
            clear();
            print_prompt();
            printf("%s", buf);
            break;
        case KBD_CTRL_U:
            while (pos > buf) {
                shell_erase();
                --pos;
            }
            *pos = 0;
            break;
        default:
            putchar(*pos);
            ++pos;
        }
    }
    printf("readline: can't find enter_key, max %d chars\n", count - 1);
}

static int32_t cmd_parse(char *cmd_str, char **argv, char token) {
    ASSERT(cmd_str != NULL);
    int32_t arg_idx = 0;
    while (arg_idx < MAX_ARG_NR)
        argv[arg_idx++] = NULL;

    char *next = cmd_str;
    int32_t argc = 0;
    while (*next) {
        while (*next == token)
            ++next;
        if (*next == 0)
            break;
        if (argc >= MAX_ARG_NR)
            return -1;
        argv[argc] = next;
        while (*next && *next != token)
            ++next;
        if (*next)
            *next++ = 0;
        ++argc;
    }
    return argc;
}

static void cmd_execute(int32_t argc, char **argv) {
    if (argc <= 0 || argv[0] == NULL)
        return;
    if (!strcmp("ls", argv[0])) {
        buildin_ls(argc, argv);
    } else if (!strcmp("cd", argv[0])) {
        if (buildin_cd(argc, argv) != NULL) {
            memset(cwd_cache, 0, sizeof(cwd_cache));
            strcpy(cwd_cache, final_path);
        }
    } else if (!strcmp("pwd", argv[0])) {
        buildin_pwd(argc, argv);
    } else if (!strcmp("ps", argv[0])) {
        buildin_ps(argc, argv);
    } else if (!strcmp("clear", argv[0])) {
        buildin_clear(argc, argv);
    } else if (!strcmp("mkdir", argv[0])) {
        buildin_mkdir(argc, argv);
    } else if (!strcmp("rmdir", argv[0])) {
        buildin_rmdir(argc, argv);
    } else if (!strcmp("rm", argv[0])) {
        buildin_rm(argc, argv);
    } else if (!strcmp("gui", argv[0])) {
        gui_start();
    } else if (!strcmp("shutdown", argv[0])) {
        buildin_shutdown(argc, argv);
    } else if (!strcmp("smash", argv[0])) {
        buildin_smash(argc, argv);
    } else {
        make_clear_abs_path(argv[0], final_path);
        char *prog_path = final_path;
        struct stat file_stat;
        memset(&file_stat, 0, sizeof(struct stat));
        if (stat(prog_path, &file_stat) == -1 && !strchr(argv[0], '.') &&
            strlen(prog_path) + 4 < MAX_PATH_LEN) {
            strcat(prog_path, ".elf");
        }
        if (stat(prog_path, &file_stat) == -1) {
            printf("my_shell: cannot access %s: No such file or directory\n",
                   argv[0]);
            return;
        }
        int32_t pid = fork();
        if (pid > 0) {
            foreground_pid = (uint32_t)pid;
            int32_t status = 0;
            int32_t child_pid = wait(&status);
            foreground_pid = (uint32_t)-1;

            printf("\n[prog %d exited, status %d]\n", (int)child_pid,
                   (int)status);
        } else if (pid == 0) {
            execv(prog_path, (const char **)argv);
            printf("execv %s failed.\n", prog_path);
            exit(-1);
        } else {
            printf("fork failed.\n");
        }
    }
}

static void run_line(char *cmd_line) {

    char *pipe_symbol = strchr(cmd_line, '|');
    if (pipe_symbol) {
        char *segments[MAX_ARG_NR];
        int nseg = 0;
        char *each_cmd = cmd_line;
        segments[nseg++] = each_cmd;
        while (nseg < MAX_ARG_NR && (pipe_symbol = strchr(each_cmd, '|'))) {
            *pipe_symbol = 0;
            each_cmd = pipe_symbol + 1;
            segments[nseg++] = each_cmd;
        }

        int32_t prev_read_fd = -1;
        for (int i = 0; i < nseg; i++) {
            int32_t wr_fd = -1;
            int32_t next_read_fd = -1;
            if (i < nseg - 1) {
                int32_t pfd[2] = {-1, -1};
                if (pipe(pfd) == -1) {
                    printf("my_shell: pipe create failed.\n");
                    break;
                }
                wr_fd = pfd[1];
                next_read_fd = pfd[0];
            }
            if (prev_read_fd != -1)
                fd_redirect(0, (uint32_t)prev_read_fd);
            if (wr_fd != -1)
                fd_redirect(1, (uint32_t)wr_fd);

            argc = -1;
            argc = cmd_parse(segments[i], argv, ' ');
            if (argc != -1)
                cmd_execute(argc, argv);

            if (wr_fd != -1) {
                fd_redirect(1, 1);
                close(wr_fd);
            }
            if (prev_read_fd != -1) {
                fd_redirect(0, 0);
                close(prev_read_fd);
            }
            prev_read_fd = next_read_fd;
        }
        if (prev_read_fd != -1)
            close(prev_read_fd);
        fd_redirect(0, 0);
        fd_redirect(1, 1);
    } else {
        argc = cmd_parse(cmd_line, argv, ' ');
        if (argc == -1) {
            printf("num of arguments exceed %d\n", MAX_ARG_NR);
            return;
        }
        cmd_execute(argc, argv);
    }
}

static void autoexec(void) {
    int32_t fd = open("/autoexec", 0);
    if (fd == -1)
        return;
    char buf[MAX_PATH_LEN];
    int32_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    char *p = buf;
    while (p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        run_line(p);
        p = nl ? nl + 1 : NULL;
    }
}

void my_shell(void *arg) {
    (void)arg;
    clear();
    cwd_cache[0] = '/';
    cwd_cache[1] = 0;
    autoexec();
    for (;;) {
        print_prompt();
        memset(final_path, 0, MAX_PATH_LEN);
        memset(cmd_line, 0, MAX_PATH_LEN);
        readline(cmd_line, MAX_PATH_LEN);
        if (cmd_line[0] == 0)
            continue;
        run_line(cmd_line);
    }
}
