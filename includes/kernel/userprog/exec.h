#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>

struct X86_REGS;
int32_t sys_execv(const char *path, const char *argv[], struct X86_REGS *regs);
int32_t sys_execve(const char *path, const char *argv[], const char *envp[],
                   struct X86_REGS *regs);

#endif
