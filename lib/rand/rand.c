#include "lib/rand/rand.h"

#include "arch/cpu.h"

#define CHACHA_RESEED_BYTES (1u << 20)

static int has_rdrand;
static int has_rdseed;

static uint32_t ckey[8];
static uint32_t cnonce[2];
static uint32_t ccounter;
static uint8_t cbuf[64];
static uint32_t cbuf_pos = 64; 
static uint32_t bytes_left;

static uint64_t entropy_pool;

static void cpuid_leaf(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                       uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static int hw_rdrand(uint64_t *out) {
    if (!has_rdrand)
        return 0;
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        uint8_t ok;
        __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static int hw_rdseed(uint64_t *out) {
    if (!has_rdseed)
        return 0;
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        uint8_t ok;
        __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static uint64_t mix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void entropy_add(uint64_t v) {
    entropy_pool ^= v;
    mix(&entropy_pool);
}

static uint64_t entropy_get(void) {
    uint64_t r;
    if (hw_rdseed(&r))
        entropy_add(r);
    if (hw_rdrand(&r))
        entropy_add(r);
    uint64_t t0 = cpu_rdtsc();
    for (volatile int i = 0; i < 8; i++)
        cpu_pause();
    entropy_add(cpu_rdtsc() ^ t0);
    entropy_add((uint64_t)(uintptr_t)&r);
    return mix(&entropy_pool);
}

static uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d)                                                     \
    do {                                                                   \
        a += b;                                                            \
        d ^= a;                                                            \
        d = rotl32(d, 16);                                                 \
        c += d;                                                            \
        b ^= c;                                                            \
        b = rotl32(b, 12);                                                 \
        a += b;                                                            \
        d ^= a;                                                            \
        d = rotl32(d, 8);                                                  \
        c += d;                                                            \
        b ^= c;                                                            \
        b = rotl32(b, 7);                                                  \
    } while (0)

static void chacha20_block(void) {
    uint32_t s[16];
    uint32_t x[16];

    s[0] = 0x61707865u;
    s[1] = 0x3320646eu;
    s[2] = 0x79622d32u;
    s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++)
        s[4 + i] = ckey[i];
    s[12] = ccounter++;
    s[13] = cnonce[0];
    s[14] = cnonce[1];
    s[15] = 0;

    for (int i = 0; i < 16; i++)
        x[i] = s[i];

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        cbuf[i * 4 + 0] = (uint8_t)v;
        cbuf[i * 4 + 1] = (uint8_t)(v >> 8);
        cbuf[i * 4 + 2] = (uint8_t)(v >> 16);
        cbuf[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    cbuf_pos = 0;
}

static void csprng_reseed(void) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = entropy_get();
        ckey[i * 2] = (uint32_t)v;
        ckey[i * 2 + 1] = (uint32_t)(v >> 32);
    }
    uint64_t n = entropy_get();
    cnonce[0] = (uint32_t)n;
    cnonce[1] = (uint32_t)(n >> 32);
    ccounter = 0;
    cbuf_pos = 64;
    bytes_left = CHACHA_RESEED_BYTES;
}

static uint8_t csprng_byte(void) {
    if (bytes_left == 0)
        csprng_reseed();
    if (cbuf_pos >= 64)
        chacha20_block();
    bytes_left--;
    return cbuf[cbuf_pos++];
}

void rand_init(void) {
    uint32_t a, b, c, d;
    cpuid_leaf(1, 0, &a, &b, &c, &d);
    has_rdrand = (int)((c >> 30) & 1u);
    cpuid_leaf(7, 0, &a, &b, &c, &d);
    has_rdseed = (int)((b >> 18) & 1u);

    entropy_pool = cpu_rdtsc();
    for (int i = 0; i < 4; i++)
        csprng_reseed();
}

uint64_t rand_u64(void) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)csprng_byte() << (i * 8);
    return v;
}

uint32_t rand_u32(void) {
    return (uint32_t)(rand_u64() >> 32);
}

void rand_bytes(void *buf, uint32_t len) {
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++)
        p[i] = csprng_byte();
}
