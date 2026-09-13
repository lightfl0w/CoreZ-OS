#include <stdint.h>
#include "arch/cpu.h"

#define asm_cli cpu_cli
#define asm_sti cpu_sti
#define asm_hlt cpu_hlt
#define asm_pause cpu_pause
#define asm_xchg cpu_xchg32
#define outb cpu_outb
#define inb cpu_inb
#define outw cpu_outw
#define inw cpu_inw
#define outl cpu_outl
#define inl cpu_inl

extern void asm_stihlt(void);

extern void insw(uint16_t port, void *buf, int words);
extern void outsw(uint16_t port, const void *buf, int words);
extern void outl(uint16_t port, uint32_t value);

extern uint64_t asm_read_cr0(void);
extern void asm_write_cr0(uint64_t cr0);
extern void asm_write_cr3(uint64_t cr3);

extern uint64_t asm_read_cr4(void);
extern void asm_write_cr4(uint64_t cr4);
extern uint64_t asm_read_cr3(void);
extern uint64_t asm_rdmsr(uint32_t msr);
extern void asm_wrmsr(uint32_t msr, uint64_t value);

extern uint64_t asm_read_cr2(void);
extern uint64_t asm_save_eflags(void);
extern void asm_restore_eflags(uint64_t eflags);

extern void asm_lgdt(uint64_t gdtr_ptr);
extern void asm_ltr(uint16_t sel);
extern uint16_t asm_str(void);

extern int detect_64bit(void);

extern int asm_mwait_supported(void);
extern void asm_sti_mwait(uint64_t addr);
