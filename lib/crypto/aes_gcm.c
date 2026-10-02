#include <stdint.h>
#include <stddef.h>

static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static uint32_t subword(uint32_t t) {
    return ((uint32_t)sbox[(t >> 24) & 0xff] << 24) |
           ((uint32_t)sbox[(t >> 16) & 0xff] << 16) |
           ((uint32_t)sbox[(t >> 8) & 0xff] << 8) |
           (uint32_t)sbox[t & 0xff];
}

static void aes_expand(const uint8_t *key, int nk, uint32_t *w) {
    int nr = nk + 6;
    int total = 4 * (nr + 1);
    int i;
    uint8_t rc = 1;
    for (i = 0; i < nk; i++)
        w[i] = ((uint32_t)key[4 * i] << 24) |
               ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) |
               (uint32_t)key[4 * i + 3];
    for (i = nk; i < total; i++) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = (t << 8) | (t >> 24);
            t = subword(t) ^ ((uint32_t)rc << 24);
            rc = xtime(rc);
        } else if (nk > 6 && i % nk == 4) {
            t = subword(t);
        }
        w[i] = w[i - nk] ^ t;
    }
}

static void add_rk(uint8_t s[16], const uint32_t *w, int round) {
    int c;
    for (c = 0; c < 4; c++) {
        uint32_t k = w[round * 4 + c];
        s[4 * c] ^= (uint8_t)(k >> 24);
        s[4 * c + 1] ^= (uint8_t)(k >> 16);
        s[4 * c + 2] ^= (uint8_t)(k >> 8);
        s[4 * c + 3] ^= (uint8_t)(k);
    }
}

static void shift_rows(uint8_t s[16]) {
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void mix_columns(uint8_t s[16]) {
    int c;
    for (c = 0; c < 4; c++) {
        uint8_t a0 = s[4 * c], a1 = s[4 * c + 1];
        uint8_t a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
        s[4 * c + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
        s[4 * c + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
        s[4 * c + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
    }
}

static void aes_block(const uint32_t *w, int nr, const uint8_t in[16],
                      uint8_t out[16]) {
    uint8_t s[16];
    int i, j;
    for (i = 0; i < 16; i++)
        s[i] = in[i];
    add_rk(s, w, 0);
    for (i = 1; i < nr; i++) {
        for (j = 0; j < 16; j++)
            s[j] = sbox[s[j]];
        shift_rows(s);
        mix_columns(s);
        add_rk(s, w, i);
    }
    for (j = 0; j < 16; j++)
        s[j] = sbox[s[j]];
    shift_rows(s);
    add_rk(s, w, nr);
    for (i = 0; i < 16; i++)
        out[i] = s[i];
}

static void gf_mul_x(uint8_t r[16], const uint8_t a[16]) {
    uint8_t carry = (uint8_t)(a[15] & 1);
    int i;
    for (i = 15; i > 0; i--)
        r[i] = (uint8_t)((a[i] >> 1) | (a[i - 1] << 7));
    r[0] = (uint8_t)(a[0] >> 1);
    if (carry)
        r[0] ^= 0xe1;
}

static void gf_mul_x4(uint8_t a[16]) {
    uint8_t t[16];
    int k, i;
    for (k = 0; k < 4; k++) {
        gf_mul_x(t, a);
        for (i = 0; i < 16; i++)
            a[i] = t[i];
    }
}

static void gf_mul(uint8_t z[16], const uint8_t x[16], const uint8_t h[16]) {
    uint8_t tab[16][16];
    uint8_t base[4][16];
    uint8_t acc[16];
    int i, n, m, k;
    for (i = 0; i < 16; i++)
        base[0][i] = h[i];
    for (m = 1; m < 4; m++)
        gf_mul_x(base[m], base[m - 1]);
    for (n = 0; n < 16; n++) {
        for (i = 0; i < 16; i++)
            tab[n][i] = 0;
        for (m = 0; m < 4; m++)
            if ((n >> (3 - m)) & 1)
                for (i = 0; i < 16; i++)
                    tab[n][i] ^= base[m][i];
    }
    for (i = 0; i < 16; i++)
        acc[i] = 0;
    for (k = 31; k >= 0; k--) {
        int nb;
        gf_mul_x4(acc);
        nb = (x[k >> 1] >> ((k & 1) ? 0 : 4)) & 0x0f;
        for (i = 0; i < 16; i++)
            acc[i] ^= tab[nb][i];
    }
    for (i = 0; i < 16; i++)
        z[i] = acc[i];
}

static void ghash_update(uint8_t y[16], const uint8_t h[16],
                         const uint8_t *data, uint32_t len) {
    uint8_t block[16];
    uint32_t i;
    while (len >= 16) {
        for (i = 0; i < 16; i++)
            y[i] ^= data[i];
        gf_mul(y, y, h);
        data += 16;
        len -= 16;
    }
    if (len) {
        for (i = 0; i < len; i++)
            block[i] = data[i];
        for (; i < 16; i++)
            block[i] = 0;
        for (i = 0; i < 16; i++)
            y[i] ^= block[i];
        gf_mul(y, y, h);
    }
}

static void inc32(uint8_t c[16]) {
    int i;
    for (i = 15; i >= 12; i--) {
        c[i]++;
        if (c[i])
            break;
    }
}

static int ct_equal(const uint8_t *a, const uint8_t *b, int n) {
    uint8_t d = 0;
    int i;
    for (i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void gcm_run(const uint8_t *key, uint32_t key_bits,
                    const uint8_t nonce[12], const uint8_t *aad,
                    uint32_t aad_len, const uint8_t *in, uint8_t *out,
                    uint32_t len, uint8_t tag[16], int enc) {
    uint32_t w[60];
    uint8_t h[16], zero[16], j0[16], ctr[16], ks[16], y[16], ek[16], lb[16];
    const uint8_t *ct = enc ? out : in;
    int nk = (int)(key_bits / 32);
    int nr = nk + 6;
    int i;
    uint32_t pos = 0;
    uint64_t abits = (uint64_t)aad_len * 8;
    uint64_t cbits = (uint64_t)len * 8;

    aes_expand(key, nk, w);
    for (i = 0; i < 16; i++) {
        zero[i] = 0;
        y[i] = 0;
    }
    aes_block(w, nr, zero, h);
    for (i = 0; i < 12; i++)
        j0[i] = nonce[i];
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
    for (i = 0; i < 16; i++)
        ctr[i] = j0[i];
    if (!enc) {
        ghash_update(y, h, aad, aad_len);
        ghash_update(y, h, ct, len);
    }
    while (pos < len) {
        uint32_t n = len - pos;
        if (n > 16)
            n = 16;
        inc32(ctr);
        aes_block(w, nr, ctr, ks);
        for (i = 0; i < (int)n; i++)
            out[pos + i] = in[pos + i] ^ ks[i];
        pos += n;
    }
    if (enc) {
        ghash_update(y, h, aad, aad_len);
        ghash_update(y, h, ct, len);
    }
    for (i = 0; i < 8; i++)
        lb[i] = (uint8_t)(abits >> (56 - 8 * i));
    for (i = 0; i < 8; i++)
        lb[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    ghash_update(y, h, lb, 16);
    aes_block(w, nr, j0, ek);
    for (i = 0; i < 16; i++)
        tag[i] = ek[i] ^ y[i];
}

void aes_gcm_encrypt(const uint8_t *key, uint32_t key_bits,
                     const uint8_t nonce[12], const uint8_t *aad,
                     uint32_t aad_len, const uint8_t *in, uint8_t *out,
                     uint32_t len, uint8_t tag[16]) {
    gcm_run(key, key_bits, nonce, aad, aad_len, in, out, len, tag, 1);
}

int aes_gcm_decrypt(const uint8_t *key, uint32_t key_bits,
                    const uint8_t nonce[12], const uint8_t *aad,
                    uint32_t aad_len, const uint8_t *in, uint8_t *out,
                    uint32_t len, const uint8_t tag[16]) {
    uint8_t t[16];
    gcm_run(key, key_bits, nonce, aad, aad_len, in, out, len, t, 0);
    return ct_equal(t, tag, 16) ? 0 : -1;
}
