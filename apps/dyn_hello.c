#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *p = malloc(32);
    snprintf(p, 32, "hi-%d", 42);
    printf("DYN_HELLO_TAG=%s\n", "libc.so");
    printf("DYN_HELLO_MSG=%s len=%d\n", p, (int)strlen(p));
    free(p);
    return 0;
}
