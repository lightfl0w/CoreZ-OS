#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MY_TIOCGPTN 0x8004540Fu
#define MY_TIOCSPTLCK 0x40045411u

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        struct timespec ts = {0, 20000000};
        for (int i = 0; i < 50; i++)
            nanosleep(&ts, NULL);
        write(2, "jc: child SURVIVED\n", 19);
        _exit(0x11);
    }
    struct timespec ts = {0, 50000000};
    nanosleep(&ts, NULL);
    kill(-pid, SIGTERM);
    int st = 0;
    wait(&st);
    int term = WIFSIGNALED(st) ? WTERMSIG(st) : -1;
    char b[48];
    int k = snprintf(b, sizeof b, "jc: term=%d exited=%d code=0x%x\n", term,
                     WIFEXITED(st), WEXITSTATUS(st));
    write(2, b, (size_t)k);
    _exit(term == SIGTERM ? 0 : 1);
}
