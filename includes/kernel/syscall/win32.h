#ifndef SYSCALL_WIN32_H
#define SYSCALL_WIN32_H

#include "kernel/asm/stub.h"
#include <stdint.h>

#define WIN32_SYSCALL_BASE 0x60000u
#define WIN_THUNK_SIZE 16u
#define WIN_THUNK_SLOTS 256u
#define WIN_SHADOW 0x20u
#define WIN_STACK_BASE (WIN_SHADOW + 8u)
#define WIN_FMT_MAX 256u
#define WIN_NAME_MAX 96u
#define WIN_IO_BUF 512u
#define WIN_STR_CHUNK 64u
#define WIN_STR_MAX 4096u
#define WIN_STD_INPUT 0xfffffff6u
#define WIN_STD_OUTPUT 0xfffffff5u
#define WIN_STD_ERROR 0xfffffff4u
#define WIN_PAGE_EXECUTE_READWRITE 0x40u
#define WIN_MEMORY_BASIC_SIZE 0x30u

void win32_thunk_init(uint32_t base);
uint32_t win32_lookup(const char *name);
uint32_t win32_resolve(const char *name);
int64_t win32_handler(struct ARCH_REGS *r);

#endif