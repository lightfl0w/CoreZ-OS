#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("[abi] start argc=%d argv0=%s argv1=%s\n", argc,
           argc > 0 ? argv[0] : "", argc > 1 ? argv[1] : "");
    if (argc > 1 && strcmp(argv[1], "envcheck") == 0) {
        printf("[abi] env FOO=%s\n", getenv("FOO"));
        return 66;
    }
    DIR *d;
    int nd;
    int pfd[2];
    int fd = open("/cat.elf", O_RDONLY);
    printf("[abi] openat fd=%d\n", fd);
    char buf[8];
    ssize_t n = read(fd, buf, 4);
    printf("[abi] read n=%d elf=%c%c\n", (int)n, buf[0], buf[1]);
    printf("[abi] lseek=%d\n", (int)lseek(fd, 0, SEEK_SET));
    struct stat st;
    int r = fstat(fd, &st);
    printf("[abi] fstat r=%d size=%d reg=%d\n", r, (int)st.st_size,
           S_ISREG(st.st_mode));
    r = stat("/cat.elf", &st);
    printf("[abi] stat r=%d size=%d reg=%d\n", r, (int)st.st_size,
           S_ISREG(st.st_mode));
    write(1, "[abi] S1 close\n", 15);
    close(fd);
    write(1, "[abi] S2 opendir\n", 17);
    int dd = open("/", O_RDONLY | O_DIRECTORY);
    printf("[abi] open / dir fd=%d\n", dd);
    if (dd >= 0)
        close(dd);
    d = opendir("/");
    write(1, "[abi] S3 readdir\n", 17);
    nd = 0;
    if (d != NULL) {
        while (readdir(d) != NULL) {
            nd++;
        }
        closedir(d);
    } else {
        write(1, "[abi] opendir NULL\n", 19);
    }
    printf("[abi] readdir n=%d\n", nd);
    write(1, "[abi] S4 pipe\n", 14);
    pfd[0] = -1;
    pfd[1] = -1;
    printf("[abi] pipe r=%d\n", pipe(pfd));
    write(pfd[1], "pipe-ok", 7);
    char pb[8];
    read(pfd[0], pb, 7);
    pb[7] = 0;
    printf("[abi] pipe data=%s\n", pb);
    close(pfd[0]);
    close(pfd[1]);
    int f1 = open("/cat.elf", O_RDONLY);
    int f2 = dup2(f1, 7);
    printf("[abi] dup2 r=%d\n", f2);
    close(f1);
    close(f2);
    char *cargv[] = {(char *)"musl_abi_test", (char *)"envcheck", NULL};
    char *cenv[] = {(char *)"FOO=bar42", NULL};
    pid_t pid = fork();
    printf("[abi] fork pid=%d\n", (int)pid);
    if (pid == 0) {
        execve("/musl_abi_test.elf", cargv, cenv);
        printf("[abi] execve errno=%d\n", errno);
        _exit(127);
    }
    int ws = 0;
    int wr = waitpid(pid, &ws, 0);
    printf("[abi] waitpid r=%d raw=0x%x status=%d\n", wr, (unsigned)ws,
           WEXITSTATUS(ws));
    int mk = mknod("/dev/apitest", S_IFCHR | 0666, makedev(1, 5));
    printf("[abi] mknod r=%d errno=%d\n", mk, errno);
    int zf = mk == 0 ? open("/dev/apitest", O_RDONLY) : -1;
    char ab[4] = {1, 1, 1, 1};
    int rn = zf >= 0 ? (int)read(zf, ab, 4) : -1;
    printf("[abi] mknod-dev read r=%d zero=%d\n", rn,
           ab[0] == 0 && ab[3] == 0);
    if (zf >= 0) close(zf);
    printf("[abi] unlink r=%d errno=%d\n", unlink("/dev/apitest"), errno);
    printf("[abi] ALL PASS\n");
    return 0;
}
