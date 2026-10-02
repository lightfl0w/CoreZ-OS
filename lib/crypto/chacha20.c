#include "lib/crypto/chacha20.h"

#include <stdint.h>

#define CHACHA_MASK44 ((uint64_t)0xfffffffffff)
#define CHACHA_MASK42 ((uint64_t)0x3ffffffffff)

struct POLY_CTX {
    uint64_t r0, r1, r2, s1, s2;
    uint64_t h0, h1, h2;
    uint64_t pad0, pad1;
    uint8_t buf[16];
    uint32_t left;
};

static uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint64_t ld64(const uint8_t *p) {
    return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

static void st64(uint8_t *p, uint64_t v) {
    st32(p, (uint32_t)v);
    st32(p + 4, (uint32_t)(v >> 32));
}

#define CHACHA_QR(a, b, c, d)                                              \
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

void chacha20_block(const uint32_t key[8], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16];
    uint32_t x[16];

    s[0] = 0x61707865u;
    s[1] = 0x3320646eu;
    s[2] = 0x79622d32u;
    s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++)
        s[4 + i] = key[i];
    s[12] = counter;
    s[13] = ld32(nonce);
    s[14] = ld32(nonce + 4);
    s[15] = ld32(nonce + 8);

    for (int i = 0; i < 16; i++)
        x[i] = s[i];

    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8], x[12]);
        CHACHA_QR(x[1], x[5], x[9], x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8], x[13]);
        CHACHA_QR(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; i++)
        st32(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint32_t counter, const uint8_t *in, uint8_t *out,
                  uint32_t len) {
    uint32_t k[8];
    uint8_t ks[64];
    uint32_t off = 0;

    for (int i = 0; i < 8; i++)
        k[i] = ld32(key + 4 * i);

    while (off < len) {
        chacha20_block(k, counter, nonce, ks);
        counter++;
        for (int i = 0; i < 64 && off < len; i++, off++)
            out[off] = in[off] ^ ks[i];
    }
}

static void poly1305_init(struct POLY_CTX *ctx, const uint8_t key[32]) {
    uint64_t rlo = ld64(key) & (uint64_t)0x0ffffffc0fffffffull;
    uint64_t rhi = ld64(key + 8) & (uint64_t)0x0ffffffc0ffffffcull;

    ctx->r0 = rlo & CHACHA_MASK44;
    ctx->r1 = ((rlo >> 44) | (rhi << 20)) & CHACHA_MASK44;
    ctx->r2 = (rhi >> 24) & CHACHA_MASK42;
    ctx->s1 = ctx->r1 * 20;
    ctx->s2 = ctx->r2 * 20;
    ctx->h0 = 0;
    ctx->h1 = 0;
    ctx->h2 = 0;
    ctx->pad0 = ld64(key + 16);
    ctx->pad1 = ld64(key + 24);
    ctx->left = 0;
}

static void poly1305_block(struct POLY_CTX *ctx, const uint8_t *blk,
                           uint64_t hibit) {
    uint64_t t0 = ld64(blk);
    uint64_t t1 = ld64(blk + 8);
    uint64_t h0 = ctx->h0 + (t0 & CHACHA_MASK44);
    uint64_t h1 = ctx->h1 + (((t0 >> 44) | (t1 << 20)) & CHACHA_MASK44);
    uint64_t h2 = ctx->h2 + ((t1 >> 24) | hibit);
    __uint128_t d0;
    __uint128_t d1;
    __uint128_t d2;
    uint64_t c;

    d0 = (__uint128_t)h0 * ctx->r0 + (__uint128_t)h1 * ctx->s2 +
         (__uint128_t)h2 * ctx->s1;
    d1 = (__uint128_t)h0 * ctx->r1 + (__uint128_t)h1 * ctx->r0 +
         (__uint128_t)h2 * ctx->s2;
    d2 = (__uint128_t)h0 * ctx->r2 + (__uint128_t)h1 * ctx->r1 +
         (__uint128_t)h2 * ctx->r0;

    c = (uint64_t)(d0 >> 44);
    h0 = (uint64_t)d0 & CHACHA_MASK44;
    d1 += c;
    c = (uint64_t)(d1 >> 44);
    h1 = (uint64_t)d1 & CHACHA_MASK44;
    d2 += c;
    c = (uint64_t)(d2 >> 42);
    h2 = (uint64_t)d2 & CHACHA_MASK42;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= CHACHA_MASK44;
    h1 += c;

    ctx->h0 = h0;
    ctx->h1 = h1;
    ctx->h2 = h2;
}

static void poly1305_update(struct POLY_CTX *ctx, const uint8_t *data,
                            uint32_t len) {
    uint32_t i = 0;

    if (ctx->left) {
        while (i < len && ctx->left < 16)
            ctx->buf[ctx->left++] = data[i++];
        if (ctx->left == 16) {
            poly1305_block(ctx, ctx->buf, (uint64_t)1 << 40);
            ctx->left = 0;
        }
    }

    while (len - i >= 16) {
        poly1305_block(ctx, data + i, (uint64_t)1 << 40);
        i += 16;
    }

    while (i < len)
        ctx->buf[ctx->left++] = data[i++];
}

static void poly1305_final(struct POLY_CTX *ctx, uint8_t tag[16]) {
    uint64_t h0, h1, h2, c, g0, g1, g2;

    if (ctx->left) {
        uint32_t i = ctx->left;
        ctx->buf[i++] = 1;
        while (i < 16)
            ctx->buf[i++] = 0;
        poly1305_block(ctx, ctx->buf, 0);
    }

    h0 = ctx->h0;
    h1 = ctx->h1;
    h2 = ctx->h2;

    c = h1 >> 44;
    h1 &= CHACHA_MASK44;
    h2 += c;
    c = h2 >> 42;
    h2 &= CHACHA_MASK42;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= CHACHA_MASK44;
    h1 += c;
    c = h1 >> 44;
    h1 &= CHACHA_MASK44;
    h2 += c;
    c = h2 >> 42;
    h2 &= CHACHA_MASK42;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= CHACHA_MASK44;
    h1 += c;

    g0 = h0 + 5;
    c = g0 >> 44;
    g0 &= CHACHA_MASK44;
    g1 = h1 + c;
    c = g1 >> 44;
    g1 &= CHACHA_MASK44;
    g2 = h2 + c - ((uint64_t)1 << 42);

    c = (g2 >> 63) - 1;
    g0 &= c;
    g1 &= c;
    g2 &= c;
    c = ~c;
    h0 = (h0 & c) | g0;
    h1 = (h1 & c) | g1;
    h2 = (h2 & c) | g2;

    h0 += ctx->pad0 & CHACHA_MASK44;
    c = h0 >> 44;
    h0 &= CHACHA_MASK44;
    h1 += (((ctx->pad0 >> 44) | (ctx->pad1 << 20)) & CHACHA_MASK44) + c;
    c = h1 >> 44;
    h1 &= CHACHA_MASK44;
    h2 += ((ctx->pad1 >> 24) & CHACHA_MASK42) + c;
    h2 &= CHACHA_MASK42;

    h0 = h0 | (h1 << 44);
    h1 = (h1 >> 20) | (h2 << 24);

    st64(tag, h0);
    st64(tag + 8, h1);
}

void poly1305(const uint8_t key[32], const uint8_t *msg, uint32_t len,
              uint8_t tag[16]) {
    struct POLY_CTX ctx;

    poly1305_init(&ctx, key);
    poly1305_update(&ctx, msg, len);
    poly1305_final(&ctx, tag);
}

static void poly1305_mac(const uint8_t pkey[32], const uint8_t *aad,
                         uint32_t aad_len, const uint8_t *ct, uint32_t len,
                         uint8_t tag[16]) {
    struct POLY_CTX ctx;
    uint8_t zeros[16] = {0};
    uint8_t lens[16];
    uint32_t pad;

    poly1305_init(&ctx, pkey);
    poly1305_update(&ctx, aad, aad_len);
    pad = (16u - (aad_len & 15u)) & 15u;
    poly1305_update(&ctx, zeros, pad);
    poly1305_update(&ctx, ct, len);
    pad = (16u - (len & 15u)) & 15u;
    poly1305_update(&ctx, zeros, pad);
    st64(lens, (uint64_t)aad_len);
    st64(lens + 8, (uint64_t)len);
    poly1305_update(&ctx, lens, 16);
    poly1305_final(&ctx, tag);
}

static void chacha20_poly1305_setup(const uint8_t key[32], const uint8_t nonce[12],
                                    uint8_t pkey[32]) {
    uint32_t k[8];
    uint8_t block0[64];

    for (int i = 0; i < 8; i++)
        k[i] = ld32(key + 4 * i);
    chacha20_block(k, 0, nonce, block0);
    for (int i = 0; i < 32; i++)
        pkey[i] = block0[i];
}

static int ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t d = 0;

    for (uint32_t i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, uint32_t aad_len,
                               const uint8_t *in, uint8_t *out, uint32_t len,
                               uint8_t tag[16]) {
    uint8_t pkey[32];

    chacha20_poly1305_setup(key, nonce, pkey);
    chacha20_xor(key, nonce, 1, in, out, len);
    poly1305_mac(pkey, aad, aad_len, out, len, tag);
}

int chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, uint32_t aad_len,
                              const uint8_t *in, uint8_t *out, uint32_t len,
                              const uint8_t tag[16]) {
    uint8_t pkey[32];
    uint8_t expect[16];

    chacha20_poly1305_setup(key, nonce, pkey);
    poly1305_mac(pkey, aad, aad_len, in, len, expect);
    if (!ct_equal(expect, tag, 16))
        return -1;
    chacha20_xor(key, nonce, 1, in, out, len);
    return 0;
}
