#ifndef NR_MICRO_SHELL_PORT_H
#define NR_MICRO_SHELL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <unistd.h>

#define shell_putc(x) write(1, &(x), 1)

#define NR_SHELL_MAX_LINE_SZ  100
#define NR_SHELL_PROMPT       "corez"
#define NR_SHELL_MAX_PARAM_NUM 16

#define NR_SHELL_SHOW_LOGO

#define NR_SHELL_AUTO_COMPLETE_SUPPORT
#define NR_SHELL_HISTORY_CMD_SUPPORT
#define NR_SHELL_HISTORY_CMD_NUM  5
#define NR_SHELL_HISTORY_CMD_SZ  100

#ifdef __cplusplus
}

#endif
#endif /* NR_MICRO_SHELL_PORT_H */
