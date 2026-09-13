#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

static inline void cpu_cli(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void cpu_sti(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline void cpu_hlt(void) {
    __asm__ volatile("hlt");
}

static inline void cpu_pause(void) {
    __asm__ volatile("pause");
}

static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t cpu_eflags(void) {
    uint64_t v;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(v));
    return (uint32_t)v;
}

static inline void cpu_set_eflags(uint32_t v) {
    __asm__ volatile("pushq %0\n\tpopfq" :: "r"((uint64_t)v) : "memory");
}

static inline uint32_t cpu_xchg32(volatile uint32_t *addr, uint32_t v) {
    uint32_t out;
    __asm__ volatile("xchg %0, %1" : "=r"(out), "+m"(*addr) : "0"(v) : "memory");
    return out;
}

static inline void cpu_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t cpu_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t cpu_inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t cpu_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_ins(uint16_t port, void *buf, int count, int width) {
    if (width == 4)
        __asm__ volatile("rep insl" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
    else if (width == 2)
        __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
    else
        __asm__ volatile("rep insb" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void cpu_outs(uint16_t port, const void *buf, int count, int width) {
    if (width == 4)
        __asm__ volatile("rep outsl" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
    else if (width == 2)
        __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
    else
        __asm__ volatile("rep outsb" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

#endif /* ARCH_CPU_H */
