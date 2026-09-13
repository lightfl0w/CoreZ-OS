#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define STACK_SIZE 0x4000

static int shared = 0;

int main(void) {
    char *stk = (char *)malloc(STACK_SIZE);
    int32_t pid = clone(CLONE_VM | CLONE_FS | CLONE_FILES, stk + STACK_SIZE);
    if (pid > 0) {
        shared = 100;
        printf("parent: clone returned %d, shared=%d\n", pid, shared);
    } else if (pid == 0) {
        shared = 200;
        printf("child: clone returned 0, my pid=%d, shared=%d\n", (int)getpid(),
               shared);
    } else {
        printf("clone failed\n");
        exit(1);
    }
    exit(0);
    return 0;
}
