#include "lib/crypto/x25519.h"

#include <stdint.h>

#define MASK51 0x7ffffffffffffULL

static uint64_t load64(const uint8_t *s) {
    return (uint64_t)s[0] | ((uint64_t)s[1] << 8) | ((uint64_t)s[2] << 16) |
           ((uint64_t)s[3] << 24) | ((uint64_t)s[4] << 32) |
           ((uint64_t)s[5] << 40) | ((uint64_t)s[6] << 48) |
           ((uint64_t)s[7] << 56);
}

static void fe_frombytes(uint64_t *h, const uint8_t *s) {
    h[0] = load64(s) & MASK51;
    h[1] = (load64(s + 6) >> 3) & MASK51;
    h[2] = (load64(s + 12) >> 6) & MASK51;
    h[3] = (load64(s + 19) >> 1) & MASK51;
    h[4] = (load64(s + 24) >> 12) & MASK51;
}

static void fe_tobytes(uint8_t *s, const uint64_t *h) {
    uint64_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
    uint64_t q;

    q = (h0 + 19) >> 51;
    q = (h1 + q) >> 51;
    q = (h2 + q) >> 51;
    q = (h3 + q) >> 51;
    q = (h4 + q) >> 51;

    h0 += 19 * q;

    h1 += h0 >> 51; h0 &= MASK51;
    h2 += h1 >> 51; h1 &= MASK51;
    h3 += h2 >> 51; h2 &= MASK51;
    h4 += h3 >> 51; h3 &= MASK51;
    h0 += (h4 >> 51) * 19; h4 &= MASK51;
    h1 += h0 >> 51; h0 &= MASK51;

    s[0] = (uint8_t)(h0 >> 0);
    s[1] = (uint8_t)(h0 >> 8);
    s[2] = (uint8_t)(h0 >> 16);
    s[3] = (uint8_t)(h0 >> 24);
    s[4] = (uint8_t)(h0 >> 32);
    s[5] = (uint8_t)(h0 >> 40);
    s[6] = (uint8_t)((h0 >> 48) | (h1 << 3));
    s[7] = (uint8_t)(h1 >> 5);
    s[8] = (uint8_t)(h1 >> 13);
    s[9] = (uint8_t)(h1 >> 21);
    s[10] = (uint8_t)(h1 >> 29);
    s[11] = (uint8_t)(h1 >> 37);
    s[12] = (uint8_t)((h1 >> 45) | (h2 << 6));
    s[13] = (uint8_t)(h2 >> 2);
    s[14] = (uint8_t)(h2 >> 10);
    s[15] = (uint8_t)(h2 >> 18);
    s[16] = (uint8_t)(h2 >> 26);
    s[17] = (uint8_t)(h2 >> 34);
    s[18] = (uint8_t)(h2 >> 42);
    s[19] = (uint8_t)((h2 >> 50) | (h3 << 1));
    s[20] = (uint8_t)(h3 >> 7);
    s[21] = (uint8_t)(h3 >> 15);
    s[22] = (uint8_t)(h3 >> 23);
    s[23] = (uint8_t)(h3 >> 31);
    s[24] = (uint8_t)(h3 >> 39);
    s[25] = (uint8_t)((h3 >> 47) | (h4 << 4));
    s[26] = (uint8_t)(h4 >> 4);
    s[27] = (uint8_t)(h4 >> 12);
    s[28] = (uint8_t)(h4 >> 20);
    s[29] = (uint8_t)(h4 >> 28);
    s[30] = (uint8_t)(h4 >> 36);
    s[31] = (uint8_t)(h4 >> 44);
}

static void fe_add(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    h[0] = f[0] + g[0];
    h[1] = f[1] + g[1];
    h[2] = f[2] + g[2];
    h[3] = f[3] + g[3];
    h[4] = f[4] + g[4];
}

static void fe_sub(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    h[0] = f[0] + (MASK51 << 1) - 36 - g[0];
    h[1] = f[1] + (MASK51 << 1) - g[1];
    h[2] = f[2] + (MASK51 << 1) - g[2];
    h[3] = f[3] + (MASK51 << 1) - g[3];
    h[4] = f[4] + (MASK51 << 1) - g[4];
}

static void fe_mul(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    uint64_t g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3,
             g4_19 = 19 * g4;
    __uint128_t r0, r1, r2, r3, r4;
    uint64_t c;

    r0 = (__uint128_t)f0 * g0 + (__uint128_t)f1 * g4_19 +
         (__uint128_t)f2 * g3_19 + (__uint128_t)f3 * g2_19 +
         (__uint128_t)f4 * g1_19;
    r1 = (__uint128_t)f0 * g1 + (__uint128_t)f1 * g0 +
         (__uint128_t)f2 * g4_19 + (__uint128_t)f3 * g3_19 +
         (__uint128_t)f4 * g2_19;
    r2 = (__uint128_t)f0 * g2 + (__uint128_t)f1 * g1 +
         (__uint128_t)f2 * g0 + (__uint128_t)f3 * g4_19 +
         (__uint128_t)f4 * g3_19;
    r3 = (__uint128_t)f0 * g3 + (__uint128_t)f1 * g2 +
         (__uint128_t)f2 * g1 + (__uint128_t)f3 * g0 +
         (__uint128_t)f4 * g4_19;
    r4 = (__uint128_t)f0 * g4 + (__uint128_t)f1 * g3 +
         (__uint128_t)f2 * g2 + (__uint128_t)f3 * g1 +
         (__uint128_t)f4 * g0;

    r1 += (uint64_t)(r0 >> 51); r0 &= MASK51;
    r2 += (uint64_t)(r1 >> 51); r1 &= MASK51;
    r3 += (uint64_t)(r2 >> 51); r2 &= MASK51;
    r4 += (uint64_t)(r3 >> 51); r3 &= MASK51;
    c = (uint64_t)(r4 >> 51); r4 &= MASK51;
    r0 += (__uint128_t)19 * c;
    c = (uint64_t)(r0 >> 51); r0 &= MASK51;
    r1 += c;

    h[0] = (uint64_t)r0;
    h[1] = (uint64_t)r1;
    h[2] = (uint64_t)r2;
    h[3] = (uint64_t)r3;
    h[4] = (uint64_t)r4;
}

static void fe_sq(uint64_t *h, const uint64_t *f) {
    fe_mul(h, f, f);
}

static void fe_mul121665(uint64_t *h, const uint64_t *f) {
    __uint128_t r0 = (__uint128_t)f[0] * 121665;
    __uint128_t r1 = (__uint128_t)f[1] * 121665;
    __uint128_t r2 = (__uint128_t)f[2] * 121665;
    __uint128_t r3 = (__uint128_t)f[3] * 121665;
    __uint128_t r4 = (__uint128_t)f[4] * 121665;
    uint64_t c;

    r1 += (uint64_t)(r0 >> 51); r0 &= MASK51;
    r2 += (uint64_t)(r1 >> 51); r1 &= MASK51;
    r3 += (uint64_t)(r2 >> 51); r2 &= MASK51;
    r4 += (uint64_t)(r3 >> 51); r3 &= MASK51;
    c = (uint64_t)(r4 >> 51); r4 &= MASK51;
    r0 += (__uint128_t)19 * c;
    c = (uint64_t)(r0 >> 51); r0 &= MASK51;
    r1 += c;

    h[0] = (uint64_t)r0;
    h[1] = (uint64_t)r1;
    h[2] = (uint64_t)r2;
    h[3] = (uint64_t)r3;
    h[4] = (uint64_t)r4;
}

static void fe_invert(uint64_t *out, const uint64_t *z) {
    uint64_t t0[5], t1[5], t2[5], t3[5];
    int i;

    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (i = 1; i < 5; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 20; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 100; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 5; i++) fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

static void fe_cswap(uint64_t *f, uint64_t *g, uint64_t b) {
    uint64_t mask = 0 - b;
    int i;
    for (i = 0; i < 5; i++) {
        uint64_t x = (f[i] ^ g[i]) & mask;
        f[i] ^= x;
        g[i] ^= x;
    }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    uint64_t x1[5], x2[5], z2[5], x3[5], z3[5];
    uint64_t a[5], aa[5], b[5], bb[5], ee[5], c[5], d[5], da[5], cb[5], t[5];
    int i;
    unsigned tbit;
    uint64_t swap = 0;
    uint64_t kt;

    for (i = 0; i < 32; i++) e[i] = scalar[i];
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe_frombytes(x1, point);
    x2[0] = 1; x2[1] = 0; x2[2] = 0; x2[3] = 0; x2[4] = 0;
    z2[0] = 0; z2[1] = 0; z2[2] = 0; z2[3] = 0; z2[4] = 0;
    x3[0] = x1[0]; x3[1] = x1[1]; x3[2] = x1[2]; x3[3] = x1[3]; x3[4] = x1[4];
    z3[0] = 1; z3[1] = 0; z3[2] = 0; z3[3] = 0; z3[4] = 0;

    for (tbit = 254; tbit < 255; tbit--) {
        kt = (e[tbit >> 3] >> (tbit & 7)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(ee, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(t, da, cb);
        fe_sq(x3, t);
        fe_sub(t, da, cb);
        fe_sq(t, t);
        fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        fe_mul121665(t, ee);
        fe_add(t, aa, t);
        fe_mul(z2, ee, t);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32];
    int i;
    for (i = 0; i < 32; i++) base[i] = 0;
    base[0] = 9;
    x25519(out, scalar, base);
}
