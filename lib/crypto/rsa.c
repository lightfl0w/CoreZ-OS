#include <stdint.h>
#include <stddef.h>
#include "lib/crypto/sha256.h"

#define RSA_NW 128
#define RSA_MAXB 512

static const uint8_t rsa_sha256_der[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};

static void rsa_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = 0;
}

static void rsa_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

static void rsa_from_be(uint32_t *out, const uint8_t *in, uint32_t len, int k)
{
    int i, j;
    uint32_t pos = len;
    for (i = 0; i < k; i++) {
        uint32_t v = 0;
        for (j = 0; j < 4; j++) {
            if (pos == 0)
                break;
            pos--;
            v |= (uint32_t)in[pos] << (8 * j);
        }
        out[i] = v;
    }
}

static void rsa_to_be(uint8_t *out, const uint32_t *x, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint32_t b = len - 1 - i;
        out[i] = (uint8_t)((x[b / 4] >> ((b % 4) * 8)) & 0xff);
    }
}

static int rsa_lt(const uint32_t *a, const uint32_t *b, int k)
{
    int i;
    for (i = k - 1; i >= 0; i--) {
        if (a[i] < b[i])
            return 1;
        if (a[i] > b[i])
            return 0;
    }
    return 0;
}

static uint32_t rsa_n0inv(uint32_t m0)
{
    uint32_t inv = 1;
    int i;
    for (i = 0; i < 5; i++)
        inv = inv * (2u - m0 * inv);
    return (uint32_t)(0u - inv);
}

static void rsa_dbl_mod(uint32_t *x, const uint32_t *m, int k)
{
    uint32_t carry = 0;
    int i, ge = 1;
    for (i = 0; i < k; i++) {
        uint32_t v = (x[i] << 1) | carry;
        carry = x[i] >> 31;
        x[i] = v;
    }
    if (!carry) {
        for (i = k - 1; i >= 0; i--) {
            if (x[i] > m[i]) {
                ge = 1;
                break;
            }
            if (x[i] < m[i]) {
                ge = 0;
                break;
            }
        }
    }
    if (ge) {
        uint32_t borrow = 0;
        for (i = 0; i < k; i++) {
            uint64_t s = (uint64_t)x[i] - m[i] - borrow;
            x[i] = (uint32_t)s;
            borrow = (uint32_t)((s >> 32) & 1u);
        }
    }
}

static void rsa_mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b,
                         const uint32_t *m, int k, uint32_t n0)
{
    uint32_t t[RSA_NW * 2 + 1];
    int i, j, ge;
    uint32_t hi;
    for (i = 0; i < 2 * k + 1; i++)
        t[i] = 0;
    for (i = 0; i < k; i++) {
        uint64_t carry = 0;
        uint32_t ai = a[i];
        for (j = 0; j < k; j++) {
            uint64_t s = (uint64_t)t[i + j] + (uint64_t)ai * b[j] + carry;
            t[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        j = i + k;
        while (carry) {
            uint64_t s = (uint64_t)t[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
            j++;
        }
    }
    for (i = 0; i < k; i++) {
        uint32_t u = (uint32_t)(t[i] * n0);
        uint64_t carry = 0;
        for (j = 0; j < k; j++) {
            uint64_t s = (uint64_t)t[i + j] + (uint64_t)u * m[j] + carry;
            t[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        j = i + k;
        while (carry) {
            uint64_t s = (uint64_t)t[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
            j++;
        }
    }
    for (i = 0; i < k; i++)
        r[i] = t[k + i];
    hi = t[2 * k];
    if (hi != 0) {
        ge = 1;
    } else {
        ge = 1;
        for (i = k - 1; i >= 0; i--) {
            if (r[i] > m[i]) {
                ge = 1;
                break;
            }
            if (r[i] < m[i]) {
                ge = 0;
                break;
            }
        }
    }
    if (ge) {
        uint64_t borrow = 0;
        for (i = 0; i < k; i++) {
            uint64_t s = (uint64_t)r[i] - m[i] - borrow;
            r[i] = (uint32_t)s;
            borrow = (s >> 32) & 1u;
        }
    }
}

static void rsa_modexp(uint32_t *out, const uint32_t *base, const uint32_t *exp,
                       int exp_limbs, const uint32_t *m, int k, uint32_t n0)
{
    uint32_t one[RSA_NW];
    uint32_t r[RSA_NW];
    uint32_t bm[RSA_NW];
    uint32_t sq[RSA_NW];
    int i, bit, started = 0;
    rsa_zero(one, (uint32_t)(k * 4));
    one[0] = 1;
    rsa_copy(r, one, (uint32_t)(k * 4));
    for (i = 0; i < 32 * k; i++)
        rsa_dbl_mod(r, m, k);
    rsa_copy(bm, base, (uint32_t)(k * 4));
    for (i = 0; i < 32 * k; i++)
        rsa_dbl_mod(bm, m, k);
    for (bit = exp_limbs * 32 - 1; bit >= 0; bit--) {
        if (started) {
            rsa_mont_mul(sq, r, r, m, k, n0);
            rsa_copy(r, sq, (uint32_t)(k * 4));
        }
        if ((exp[bit >> 5] >> (bit & 31)) & 1u) {
            rsa_mont_mul(sq, r, bm, m, k, n0);
            rsa_copy(r, sq, (uint32_t)(k * 4));
            started = 1;
        }
    }
    rsa_mont_mul(out, r, one, m, k, n0);
}

static int rsa_pub_exp(uint8_t *em, uint32_t n_len, const uint8_t *n,
                       const uint8_t *e, uint32_t e_len, const uint8_t *sig,
                       uint32_t sig_len)
{
    uint32_t nlimb[RSA_NW];
    uint32_t elimb[RSA_NW];
    uint32_t slimb[RSA_NW];
    uint32_t out[RSA_NW];
    uint32_t n0;
    int k, ek;
    if (n_len < 64 || n_len > RSA_MAXB)
        return 0;
    if (sig_len != n_len)
        return 0;
    if (e_len == 0 || e_len > RSA_MAXB)
        return 0;
    k = (int)((n_len + 3) / 4);
    ek = (int)((e_len + 3) / 4);
    rsa_from_be(nlimb, n, n_len, k);
    rsa_from_be(elimb, e, e_len, ek);
    rsa_from_be(slimb, sig, sig_len, k);
    if ((nlimb[0] & 1u) == 0)
        return 0;
    if (!rsa_lt(slimb, nlimb, k))
        return 0;
    n0 = rsa_n0inv(nlimb[0]);
    rsa_modexp(out, slimb, elimb, ek, nlimb, k, n0);
    rsa_to_be(em, out, n_len);
    return 1;
}

static int rsa_pkcs1_check(const uint8_t *em, uint32_t len,
                           const uint8_t hash[32])
{
    uint32_t i;
    int j;
    if (len < 62)
        return 0;
    if (em[0] != 0x00 || em[1] != 0x01)
        return 0;
    i = 2;
    while (i < len && em[i] == 0xff)
        i++;
    if (i - 2 < 8)
        return 0;
    if (i >= len || em[i] != 0x00)
        return 0;
    i++;
    if (len - i != 51)
        return 0;
    for (j = 0; j < 19; j++)
        if (em[i + (uint32_t)j] != rsa_sha256_der[j])
            return 0;
    for (j = 0; j < 32; j++)
        if (em[i + 19 + (uint32_t)j] != hash[j])
            return 0;
    return 1;
}

static void rsa_mgf1(uint8_t *mask, uint32_t mask_len, const uint8_t *seed,
                     uint32_t seed_len)
{
    uint8_t buf[64];
    uint8_t dig[32];
    uint32_t counter = 0;
    uint32_t i, n;
    while (mask_len > 0) {
        rsa_copy(buf, seed, seed_len);
        buf[seed_len] = (uint8_t)((counter >> 24) & 0xff);
        buf[seed_len + 1] = (uint8_t)((counter >> 16) & 0xff);
        buf[seed_len + 2] = (uint8_t)((counter >> 8) & 0xff);
        buf[seed_len + 3] = (uint8_t)(counter & 0xff);
        sha256(buf, seed_len + 4, dig);
        n = mask_len < 32 ? mask_len : 32;
        for (i = 0; i < n; i++)
            mask[i] = dig[i];
        mask += n;
        mask_len -= n;
        counter++;
    }
}

static int rsa_pss_check(const uint8_t *em, uint32_t n_len, int mod_bits,
                         const uint8_t hash[32], uint32_t salt_len)
{
    uint8_t db[512];
    uint8_t dbmask[512];
    uint8_t mp[520];
    uint8_t h2[32];
    const uint8_t *ep;
    uint32_t em_bits, em_len, db_len, left_bits, prefix, i;
    int j;
    if (n_len < 64 || n_len > RSA_MAXB)
        return 0;
    if (mod_bits < 8)
        return 0;
    em_bits = (uint32_t)(mod_bits - 1);
    em_len = (em_bits + 7) / 8;
    if (em_len > n_len)
        return 0;
    prefix = n_len - em_len;
    for (i = 0; i < prefix; i++)
        if (em[i] != 0)
            return 0;
    ep = em + prefix;
    if (em_len < 32 + salt_len + 2)
        return 0;
    if (ep[em_len - 1] != 0xbc)
        return 0;
    db_len = em_len - 33;
    if (db_len == 0)
        return 0;
    left_bits = 8 * em_len - em_bits;
    if (left_bits > 0) {
        uint8_t mask = (uint8_t)(0xffu << (8 - left_bits));
        if (ep[0] & mask)
            return 0;
    }
    rsa_mgf1(dbmask, db_len, ep + db_len, 32);
    for (i = 0; i < db_len; i++)
        db[i] = (uint8_t)(ep[i] ^ dbmask[i]);
    if (left_bits > 0)
        db[0] &= (uint8_t)(0xffu >> left_bits);
    for (i = 0; i + salt_len + 1 < db_len; i++)
        if (db[i] != 0)
            return 0;
    if (db[db_len - salt_len - 1] != 0x01)
        return 0;
    for (i = 0; i < 8; i++)
        mp[i] = 0;
    rsa_copy(mp + 8, hash, 32);
    rsa_copy(mp + 40, db + (db_len - salt_len), salt_len);
    sha256(mp, 40 + salt_len, h2);
    for (j = 0; j < 32; j++)
        if (h2[j] != ep[db_len + (uint32_t)j])
            return 0;
    return 1;
}

static int rsa_bitlen(const uint8_t *n, uint32_t n_len)
{
    uint32_t i;
    for (i = 0; i < n_len; i++) {
        if (n[i] != 0) {
            int b = 0;
            uint8_t v = n[i];
            while (v) {
                b++;
                v = (uint8_t)(v >> 1);
            }
            return (int)((n_len - i - 1) * 8 + (uint32_t)b);
        }
    }
    return 0;
}

int rsa_pkcs1_v15_verify_sha256(const uint8_t *n, uint32_t n_len,
                                const uint8_t *e, uint32_t e_len,
                                const uint8_t *sig, uint32_t sig_len,
                                const uint8_t hash[32])
{
    uint8_t em[512];
    if (!rsa_pub_exp(em, n_len, n, e, e_len, sig, sig_len))
        return 0;
    return rsa_pkcs1_check(em, n_len, hash);
}

int rsa_pss_verify_sha256(const uint8_t *n, uint32_t n_len, const uint8_t *e,
                          uint32_t e_len, const uint8_t *sig, uint32_t sig_len,
                          const uint8_t hash[32], uint32_t salt_len)
{
    uint8_t em[512];
    int mod_bits;
    if (!rsa_pub_exp(em, n_len, n, e, e_len, sig, sig_len))
        return 0;
    mod_bits = rsa_bitlen(n, n_len);
    if (mod_bits < 8)
        return 0;
    if (rsa_pss_check(em, n_len, mod_bits, hash, salt_len))
        return 1;
    if (salt_len == 0) {
        if (rsa_pss_check(em, n_len, mod_bits, hash, 32))
            return 1;
        if (rsa_pss_check(em, n_len, mod_bits, hash, 20))
            return 1;
    }
    return 0;
}
