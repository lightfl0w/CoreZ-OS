#include "nr_micro_shell.h"
#include <unistd.h>

int main(void) {
    unsigned char c;

    shell_init();
    for (;;) {
        if (read(0, &c, 1) == 1) {
            shell(c);
        }
    }
    return 0;
}
