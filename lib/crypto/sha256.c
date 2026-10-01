#include <stdint.h>
#include <stddef.h>
#include "lib/crypto/sha256.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void copy_mem(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

static void set_mem(void *dst, uint8_t value, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = value;
}

static void sha256_transform(struct sha256_ctx *c, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t big_s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + big_s1 + ch + sha256_k[i] + w[i];
        uint32_t big_s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

void sha256_init(struct sha256_ctx *c)
{
    c->h[0] = 0x6a09e667;
    c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372;
    c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f;
    c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab;
    c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->buf_len = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    c->len += len;

    if (c->buf_len != 0) {
        uint32_t need = 64 - c->buf_len;
        uint32_t take = (len < need) ? len : need;
        copy_mem(c->buf + c->buf_len, p, take);
        c->buf_len += take;
        p += take;
        len -= take;
        if (c->buf_len == 64) {
            sha256_transform(c, c->buf);
            c->buf_len = 0;
        }
    }

    while (len >= 64) {
        sha256_transform(c, p);
        p += 64;
        len -= 64;
    }

    if (len != 0) {
        copy_mem(c->buf, p, len);
        c->buf_len = len;
    }
}

void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t one = 0x80;
    uint8_t zero = 0x00;
    uint8_t lenbuf[8];
    int i;

    sha256_update(c, &one, 1);
    while (c->buf_len != 56)
        sha256_update(c, &zero, 1);

    for (i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenbuf, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[32])
{
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256_init(struct hmac_sha256_ctx *c, const uint8_t *key,
                      uint32_t key_len)
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    int i;

    if (key_len > 64) {
        sha256(key, key_len, k);
        set_mem(k + 32, 0, 32);
    } else {
        copy_mem(k, key, key_len);
        set_mem(k + key_len, 0, 64 - key_len);
    }

    for (i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    sha256_init(&c->inner);
    sha256_update(&c->inner, ipad, 64);
    sha256_init(&c->outer);
    sha256_update(&c->outer, opad, 64);
}

void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data,
                        uint32_t len)
{
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(struct hmac_sha256_ctx *c, uint8_t out[32])
{
    uint8_t inner_hash[32];
    sha256_final(&c->inner, inner_hash);
    sha256_update(&c->outer, inner_hash, 32);
    sha256_final(&c->outer, out);
}

void hmac_sha256(const uint8_t *key, uint32_t key_len, const uint8_t *msg,
                 uint32_t msg_len, uint8_t out[32])
{
    struct hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, key_len);
    hmac_sha256_update(&c, msg, msg_len);
    hmac_sha256_final(&c, out);
}

void hkdf_extract(const uint8_t *salt, uint32_t salt_len, const uint8_t *ikm,
                  uint32_t ikm_len, uint8_t out[32])
{
    static const uint8_t zero_salt[32] = { 0 };

    if (salt_len == 0)
        hmac_sha256(zero_salt, 32, ikm, ikm_len, out);
    else
        hmac_sha256(salt, salt_len, ikm, ikm_len, out);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t info_len,
                 uint8_t *out, uint32_t out_len)
{
    uint8_t t[32];
    uint32_t done = 0;
    uint8_t counter = 1;

    if (out_len > 255u * 32u)
        out_len = 255u * 32u;

    while (done < out_len) {
        struct hmac_sha256_ctx c;
        uint32_t n;

        hmac_sha256_init(&c, prk, 32);
        if (counter > 1)
            hmac_sha256_update(&c, t, 32);
        if (info_len != 0)
            hmac_sha256_update(&c, info, info_len);
        hmac_sha256_update(&c, &counter, 1);
        hmac_sha256_final(&c, t);

        n = out_len - done;
        if (n > 32)
            n = 32;
        copy_mem(out + done, t, n);
        done += n;
        counter++;
    }
}

void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, uint32_t context_len,
                       uint8_t *out, uint32_t out_len)
{
    uint8_t hkdf_label[2 + 1 + 255 + 1 + 255];
    uint32_t off = 0;
    uint32_t label_len = 0;
    uint32_t i;
    static const char prefix[6] = { 't', 'l', 's', '1', '3', ' ' };

    while (label[label_len] != '\0' && label_len < 249)
        label_len++;

    hkdf_label[off++] = (uint8_t)(out_len >> 8);
    hkdf_label[off++] = (uint8_t)(out_len);
    hkdf_label[off++] = (uint8_t)(6 + label_len);
    for (i = 0; i < 6; i++)
        hkdf_label[off++] = (uint8_t)prefix[i];
    for (i = 0; i < label_len; i++)
        hkdf_label[off++] = (uint8_t)label[i];

    if (context_len > 255)
        context_len = 255;
    hkdf_label[off++] = (uint8_t)context_len;
    for (i = 0; i < context_len; i++)
        hkdf_label[off++] = context[i];

    hkdf_expand(secret, hkdf_label, off, out, out_len);
}
