#include "lib/crypto/p256.h"

#include <stdint.h>

static const uint32_t P256_P[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
                                   0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu};
static const uint32_t P256_N[8] = {0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
                                   0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu};
static const uint32_t P256_B[8] = {0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
                                   0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u};
static const uint32_t P256_GX[8] = {0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
                                    0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u};
static const uint32_t P256_GY[8] = {0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
                                    0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u};
static const uint32_t P256_A[8] = {0xFFFFFFFCu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
                                   0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu};
static const uint32_t P256_PM2[8] = {0xFFFFFFFDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
                                     0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu};
static const uint32_t P256_NM2[8] = {0xFC63254Fu, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
                                     0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu};
static const uint32_t P256_K3[8] = {3u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
static const uint32_t P256_K4[8] = {4u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
static const uint32_t P256_K8[8] = {8u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

struct p256_pt {
    uint32_t X[8];
    uint32_t Y[8];
    uint32_t Z[8];
};
typedef struct p256_pt p256_pt;

static uint32_t p256_is_zero32(uint32_t x) {
    uint32_t t = x | (0u - x);
    return 1u ^ (t >> 31);
}

static uint32_t p256_fe_is_zero(const uint32_t a[8]) {
    uint32_t x = 0;
    int i;
    for (i = 0; i < 8; i++) {
        x |= a[i];
    }
    return p256_is_zero32(x);
}

static uint32_t p256_fe_eq(const uint32_t a[8], const uint32_t b[8]) {
    uint32_t x = 0;
    int i;
    for (i = 0; i < 8; i++) {
        x |= a[i] ^ b[i];
    }
    return p256_is_zero32(x);
}

static void p256_fe_cselect(uint32_t r[8], const uint32_t a[8], const uint32_t b[8],
                            uint32_t mask) {
    int i;
    for (i = 0; i < 8; i++) {
        r[i] = (a[i] & mask) | (b[i] & ~mask);
    }
}

static void p256_pt_cselect(p256_pt *r, const p256_pt *a, const p256_pt *b, uint32_t mask) {
    p256_fe_cselect(r->X, a->X, b->X, mask);
    p256_fe_cselect(r->Y, a->Y, b->Y, mask);
    p256_fe_cselect(r->Z, a->Z, b->Z, mask);
}

static int p256_cmp_lt(const uint32_t a[8], const uint32_t b[8]) {
    int i;
    for (i = 7; i >= 0; i--) {
        if (a[i] < b[i]) {
            return 1;
        }
        if (a[i] > b[i]) {
            return 0;
        }
    }
    return 0;
}

static void p256_mul_wide(uint32_t r[16], const uint32_t a[8], const uint32_t b[8]) {
    int i, j;
    for (i = 0; i < 16; i++) {
        r[i] = 0;
    }
    for (i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (j = 0; j < 8; j++) {
            uint64_t cur = (uint64_t)r[i + j] + (uint64_t)a[i] * (uint64_t)b[j] + carry;
            r[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        r[i + 8] = (uint32_t)carry;
    }
}

static void p256_fe_reduce(uint32_t r[8], const uint32_t t[16]) {
    uint32_t w[16];
    int i, j, iter;
    for (i = 0; i < 16; i++) {
        w[i] = t[i];
    }
    for (iter = 0; iter < 12; iter++) {
        int64_t a[16];
        int64_t carry = 0;
        uint32_t hi = 0;
        for (j = 8; j < 16; j++) {
            hi |= w[j];
        }
        if (hi == 0) {
            break;
        }
        for (i = 0; i < 16; i++) {
            a[i] = 0;
        }
        for (i = 0; i < 8; i++) {
            a[i] += (int64_t)w[i];
        }
        for (j = 0; j < 8; j++) {
            int64_t b = (int64_t)w[j + 8];
            a[j + 7] += b;
            a[j + 6] -= b;
            a[j + 3] -= b;
            a[j] += b;
        }
        for (i = 0; i < 16; i++) {
            int64_t v = a[i] + carry;
            uint32_t word = (uint32_t)(v & 0xFFFFFFFFLL);
            w[i] = word;
            carry = (v - (int64_t)word) / 4294967296LL;
        }
    }
    for (i = 0; i < 8; i++) {
        r[i] = w[i];
    }
    for (iter = 0; iter < 2; iter++) {
        uint32_t borrow = 0, d[8], m;
        for (i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)r[i] - P256_P[i] - borrow;
            d[i] = (uint32_t)v;
            borrow = (uint32_t)((v >> 63) & 1u);
        }
        m = 0u - (1u ^ borrow);
        p256_fe_cselect(r, d, r, m);
    }
}

static void p256_fe_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t t[16];
    p256_mul_wide(t, a, b);
    p256_fe_reduce(r, t);
}

static void p256_fe_sqr(uint32_t r[8], const uint32_t a[8]) {
    p256_fe_mul(r, a, a);
}

static void p256_fe_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t s[8];
    uint64_t carry = 0;
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        v = (uint64_t)a[i] + (uint64_t)b[i] + carry;
        s[i] = (uint32_t)v;
        carry = v >> 32;
    }
    if (carry != 0) {
        uint64_t bw = 0;
        for (i = 0; i < 8; i++) {
            uint64_t x = (uint64_t)s[i] - P256_P[i] - bw;
            s[i] = (uint32_t)x;
            bw = (x >> 63) & 1u;
        }
    } else {
        uint32_t borrow = 0, d[8], m;
        for (i = 0; i < 8; i++) {
            uint64_t x = (uint64_t)s[i] - P256_P[i] - borrow;
            d[i] = (uint32_t)x;
            borrow = (uint32_t)((x >> 63) & 1u);
        }
        m = 0u - (1u ^ borrow);
        p256_fe_cselect(s, d, s, m);
    }
    for (i = 0; i < 8; i++) {
        r[i] = s[i];
    }
}

static void p256_fe_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t d[8];
    uint32_t borrow = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t v = (uint64_t)a[i] - (uint64_t)b[i] - borrow;
        d[i] = (uint32_t)v;
        borrow = (uint32_t)((v >> 63) & 1u);
    }
    if (borrow != 0) {
        uint64_t carry = 0;
        for (i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)d[i] + P256_P[i] + carry;
            d[i] = (uint32_t)v;
            carry = v >> 32;
        }
    }
    for (i = 0; i < 8; i++) {
        r[i] = d[i];
    }
}

static void p256_load_be(uint32_t r[8], const uint8_t b[32]) {
    int i, j;
    for (i = 0; i < 8; i++) {
        uint32_t w = 0;
        for (j = 0; j < 4; j++) {
            w = (w << 8) | (uint32_t)b[(7 - i) * 4 + j];
        }
        r[i] = w;
    }
}

static void p256_fe_to_bytes(uint8_t b[32], const uint32_t r[8]) {
    int i;
    for (i = 0; i < 8; i++) {
        uint32_t w = r[7 - i];
        b[i * 4 + 0] = (uint8_t)(w >> 24);
        b[i * 4 + 1] = (uint8_t)(w >> 16);
        b[i * 4 + 2] = (uint8_t)(w >> 8);
        b[i * 4 + 3] = (uint8_t)w;
    }
}

static int p256_fe_from_bytes(uint32_t r[8], const uint8_t b[32]) {
    p256_load_be(r, b);
    return p256_cmp_lt(r, P256_P);
}

static void p256_mod_n(uint32_t r[8], const uint32_t t[16]) {
    uint32_t rem[9], d[9];
    int i, j;
    for (j = 0; j < 9; j++) {
        rem[j] = 0;
    }
    for (i = 511; i >= 0; i--) {
        uint32_t carry = (t[i >> 5] >> (i & 31)) & 1u;
        uint32_t borrow = 0;
        uint32_t ge;
        for (j = 0; j < 9; j++) {
            uint32_t nc = rem[j] >> 31;
            rem[j] = (rem[j] << 1) | carry;
            carry = nc;
        }
        for (j = 0; j < 8; j++) {
            uint64_t v = (uint64_t)rem[j] - P256_N[j] - borrow;
            d[j] = (uint32_t)v;
            borrow = (uint32_t)((v >> 63) & 1u);
        }
        d[8] = rem[8] - borrow;
        ge = (rem[8] != 0) ? 1u : (uint32_t)(1u ^ borrow);
        if (ge != 0) {
            for (j = 0; j < 9; j++) {
                rem[j] = d[j];
            }
        }
    }
    for (j = 0; j < 8; j++) {
        r[j] = rem[j];
    }
}

static void p256_scalar_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t t[16];
    p256_mul_wide(t, a, b);
    p256_mod_n(r, t);
}

static void p256_fe_inv(uint32_t r[8], const uint32_t a[8]) {
    uint32_t result[8] = {1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t base[8], tmp[8];
    int i;
    for (i = 0; i < 8; i++) {
        base[i] = a[i];
    }
    for (i = 0; i < 256; i++) {
        uint32_t bit = (P256_PM2[i >> 5] >> (i & 31)) & 1u;
        p256_fe_mul(tmp, result, base);
        p256_fe_cselect(result, tmp, result, 0u - bit);
        p256_fe_sqr(base, base);
    }
    for (i = 0; i < 8; i++) {
        r[i] = result[i];
    }
}

static void p256_scalar_inv(uint32_t r[8], const uint32_t a[8]) {
    uint32_t result[8] = {1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t base[8], tmp[8];
    int i;
    for (i = 0; i < 8; i++) {
        base[i] = a[i];
    }
    for (i = 0; i < 256; i++) {
        uint32_t bit = (P256_NM2[i >> 5] >> (i & 31)) & 1u;
        p256_scalar_mul(tmp, result, base);
        p256_fe_cselect(result, tmp, result, 0u - bit);
        p256_scalar_mul(base, base, base);
    }
    for (i = 0; i < 8; i++) {
        r[i] = result[i];
    }
}

static int p256_scalar_valid(const uint32_t a[8]) {
    uint32_t x = 0;
    int i;
    for (i = 0; i < 8; i++) {
        x |= a[i];
    }
    if (x == 0) {
        return 0;
    }
    return p256_cmp_lt(a, P256_N);
}

static void p256_pt_set_inf(p256_pt *p) {
    int i;
    for (i = 0; i < 8; i++) {
        p->X[i] = 1u;
        p->Y[i] = 1u;
        p->Z[i] = 0u;
    }
}

static void p256_pt_dbl(p256_pt *r, const p256_pt *p) {
    uint32_t X1[8], Y1[8], Z1[8], delta[8], gamma[8], beta[8], alpha[8];
    uint32_t X3[8], Y3[8], Z3[8], t1[8], t2[8];
    int i;
    for (i = 0; i < 8; i++) {
        X1[i] = p->X[i];
        Y1[i] = p->Y[i];
        Z1[i] = p->Z[i];
    }
    p256_fe_sqr(delta, Z1);
    p256_fe_sqr(gamma, Y1);
    p256_fe_mul(beta, X1, gamma);
    p256_fe_sub(t1, X1, delta);
    p256_fe_add(t2, X1, delta);
    p256_fe_mul(t1, t1, t2);
    p256_fe_mul(alpha, t1, P256_K3);
    p256_fe_sqr(t1, alpha);
    p256_fe_mul(t2, beta, P256_K8);
    p256_fe_sub(X3, t1, t2);
    p256_fe_add(t1, Y1, Z1);
    p256_fe_sqr(t1, t1);
    p256_fe_sub(t1, t1, gamma);
    p256_fe_sub(Z3, t1, delta);
    p256_fe_mul(t1, beta, P256_K4);
    p256_fe_sub(t1, t1, X3);
    p256_fe_mul(t1, alpha, t1);
    p256_fe_sqr(t2, gamma);
    p256_fe_mul(t2, t2, P256_K8);
    p256_fe_sub(Y3, t1, t2);
    for (i = 0; i < 8; i++) {
        r->X[i] = X3[i];
        r->Y[i] = Y3[i];
        r->Z[i] = Z3[i];
    }
}

static void p256_pt_add(p256_pt *r, const p256_pt *p, const p256_pt *q) {
    uint32_t X1[8], Y1[8], Z1[8], X2[8], Y2[8], Z2[8];
    uint32_t Z1Z1[8], Z2Z2[8], U1[8], U2[8], S1[8], S2[8], H[8];
    uint32_t I[8], J[8], rr[8], V[8], t1[8], t2[8];
    p256_pt G, D, T, O;
    uint32_t m_z1, m_z2, m_h, m_seq;
    int i;
    for (i = 0; i < 8; i++) {
        X1[i] = p->X[i];
        Y1[i] = p->Y[i];
        Z1[i] = p->Z[i];
        X2[i] = q->X[i];
        Y2[i] = q->Y[i];
        Z2[i] = q->Z[i];
        O.X[i] = 1u;
        O.Y[i] = 1u;
        O.Z[i] = 0u;
    }
    p256_fe_sqr(Z1Z1, Z1);
    p256_fe_sqr(Z2Z2, Z2);
    p256_fe_mul(U1, X1, Z2Z2);
    p256_fe_mul(U2, X2, Z1Z1);
    p256_fe_mul(S1, Y1, Z2);
    p256_fe_mul(S1, S1, Z2Z2);
    p256_fe_mul(S2, Y2, Z1);
    p256_fe_mul(S2, S2, Z1Z1);
    p256_fe_sub(H, U2, U1);
    p256_fe_add(t1, H, H);
    p256_fe_sqr(I, t1);
    p256_fe_mul(J, H, I);
    p256_fe_sub(t2, S2, S1);
    p256_fe_add(rr, t2, t2);
    p256_fe_mul(V, U1, I);
    p256_fe_sqr(t1, rr);
    p256_fe_sub(t1, t1, J);
    p256_fe_add(t2, V, V);
    p256_fe_sub(G.X, t1, t2);
    p256_fe_sub(t1, V, G.X);
    p256_fe_mul(t1, rr, t1);
    p256_fe_mul(t2, S1, J);
    p256_fe_add(t2, t2, t2);
    p256_fe_sub(G.Y, t1, t2);
    p256_fe_add(t1, Z1, Z2);
    p256_fe_sqr(t1, t1);
    p256_fe_sub(t1, t1, Z1Z1);
    p256_fe_sub(t1, t1, Z2Z2);
    p256_fe_mul(G.Z, t1, H);
    p256_pt_dbl(&D, p);
    m_seq = 0u - p256_fe_eq(S1, S2);
    m_h = 0u - p256_fe_is_zero(H);
    m_z2 = 0u - p256_fe_is_zero(Z2);
    m_z1 = 0u - p256_fe_is_zero(Z1);
    p256_pt_cselect(&T, &D, &O, m_seq);
    p256_pt_cselect(&T, &T, &G, m_h);
    p256_pt_cselect(&T, p, &T, m_z2);
    p256_pt_cselect(&T, q, &T, m_z1);
    for (i = 0; i < 8; i++) {
        r->X[i] = T.X[i];
        r->Y[i] = T.Y[i];
        r->Z[i] = T.Z[i];
    }
}

static void p256_pt_mul(p256_pt *r, const p256_pt *p, const uint32_t k[8]) {
    p256_pt R, T;
    int i;
    p256_pt_set_inf(&R);
    for (i = 255; i >= 0; i--) {
        uint32_t bit = (k[i >> 5] >> (i & 31)) & 1u;
        p256_pt_dbl(&R, &R);
        p256_pt_add(&T, &R, p);
        p256_pt_cselect(&R, &T, &R, 0u - bit);
    }
    for (i = 0; i < 8; i++) {
        r->X[i] = R.X[i];
        r->Y[i] = R.Y[i];
        r->Z[i] = R.Z[i];
    }
}

static void p256_pt_to_affine(uint32_t x[8], uint32_t y[8], const p256_pt *p) {
    uint32_t zi[8], zi2[8], t[8];
    p256_fe_inv(zi, p->Z);
    p256_fe_sqr(zi2, zi);
    p256_fe_mul(x, p->X, zi2);
    p256_fe_mul(t, zi2, zi);
    p256_fe_mul(y, p->Y, t);
}

static int p256_on_curve(const uint32_t x[8], const uint32_t y[8]) {
    uint32_t lhs[8], t[8], ax[8];
    p256_fe_sqr(lhs, y);
    p256_fe_sqr(t, x);
    p256_fe_mul(t, t, x);
    p256_fe_mul(ax, P256_A, x);
    p256_fe_add(t, t, ax);
    p256_fe_add(t, t, P256_B);
    return (int)p256_fe_eq(lhs, t);
}

int p256_pub_from_priv(uint8_t pub[64], const uint8_t priv[32]) {
    uint32_t d[8], x[8], y[8];
    p256_pt G, R;
    int i;
    p256_load_be(d, priv);
    if (!p256_scalar_valid(d)) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        G.X[i] = P256_GX[i];
        G.Y[i] = P256_GY[i];
        G.Z[i] = 0u;
    }
    G.Z[0] = 1u;
    p256_pt_mul(&R, &G, d);
    if (p256_fe_is_zero(R.Z)) {
        return -1;
    }
    p256_pt_to_affine(x, y, &R);
    p256_fe_to_bytes(pub, x);
    p256_fe_to_bytes(pub + 32, y);
    return 0;
}

int p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[64]) {
    uint32_t d[8], x[8], y[8], rx[8], ry[8];
    p256_pt Q, R;
    int i;
    p256_load_be(d, priv);
    if (!p256_scalar_valid(d)) {
        return -1;
    }
    if (!p256_fe_from_bytes(x, pub)) {
        return -1;
    }
    if (!p256_fe_from_bytes(y, pub + 32)) {
        return -1;
    }
    if (!p256_on_curve(x, y)) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        Q.X[i] = x[i];
        Q.Y[i] = y[i];
        Q.Z[i] = 0u;
    }
    Q.Z[0] = 1u;
    p256_pt_mul(&R, &Q, d);
    if (p256_fe_is_zero(R.Z)) {
        return -1;
    }
    p256_pt_to_affine(rx, ry, &R);
    p256_fe_to_bytes(out, rx);
    return 0;
}

int p256_ecdsa_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]) {
    uint32_t rr[8], ss[8], z[8], w[8], u1[8], u2[8], e[8];
    uint32_t x[8], y[8], rx[8], ry[8];
    uint32_t t[16];
    p256_pt G, Q, P1, P2, R;
    int i;
    p256_load_be(rr, sig);
    p256_load_be(ss, sig + 32);
    if (!p256_scalar_valid(rr) || !p256_scalar_valid(ss)) {
        return -1;
    }
    if (!p256_fe_from_bytes(x, pub)) {
        return -1;
    }
    if (!p256_fe_from_bytes(y, pub + 32)) {
        return -1;
    }
    if (!p256_on_curve(x, y)) {
        return -1;
    }
    p256_load_be(z, hash);
    p256_scalar_inv(w, ss);
    p256_scalar_mul(u1, z, w);
    p256_scalar_mul(u2, rr, w);
    for (i = 0; i < 8; i++) {
        G.X[i] = P256_GX[i];
        G.Y[i] = P256_GY[i];
        G.Z[i] = 0u;
        Q.X[i] = x[i];
        Q.Y[i] = y[i];
        Q.Z[i] = 0u;
        t[i] = 0u;
    }
    G.Z[0] = 1u;
    Q.Z[0] = 1u;
    for (i = 8; i < 16; i++) {
        t[i] = 0u;
    }
    p256_pt_mul(&P1, &G, u1);
    p256_pt_mul(&P2, &Q, u2);
    p256_pt_add(&R, &P1, &P2);
    if (p256_fe_is_zero(R.Z)) {
        return -1;
    }
    p256_pt_to_affine(rx, ry, &R);
    for (i = 0; i < 8; i++) {
        t[i] = rx[i];
    }
    p256_mod_n(e, t);
    return p256_fe_eq(e, rr) ? 0 : -1;
}

static int p256_der_int(uint8_t out[32], const uint8_t *p, uint32_t len) {
    uint32_t i = 0, vlen, pad, j;
    if (len == 0 || len > 33) {
        return -1;
    }
    while (i < len && p[i] == 0) {
        i++;
    }
    vlen = len - i;
    if (vlen > 32) {
        return -1;
    }
    pad = 32 - vlen;
    for (j = 0; j < pad; j++) {
        out[j] = 0u;
    }
    for (j = 0; j < vlen; j++) {
        out[pad + j] = p[i + j];
    }
    return 0;
}

int p256_ecdsa_verify_der(const uint8_t pub[64], const uint8_t hash[32], const uint8_t *der,
                          uint32_t der_len) {
    uint32_t off, seq, rlen, slen;
    uint8_t sig[64];
    if (der_len < 8) {
        return -1;
    }
    if (der[0] != 0x30) {
        return -1;
    }
    if (der[1] < 0x80) {
        seq = der[1];
        off = 2;
    } else if (der[1] == 0x81) {
        if (der_len < 3) {
            return -1;
        }
        seq = der[2];
        off = 3;
    } else {
        return -1;
    }
    if (off + seq != der_len) {
        return -1;
    }
    if (off + 2 > der_len || der[off] != 0x02) {
        return -1;
    }
    rlen = der[off + 1];
    off += 2;
    if (off + rlen > der_len || p256_der_int(sig, der + off, rlen) != 0) {
        return -1;
    }
    off += rlen;
    if (off + 2 > der_len || der[off] != 0x02) {
        return -1;
    }
    slen = der[off + 1];
    off += 2;
    if (off + slen > der_len || p256_der_int(sig + 32, der + off, slen) != 0) {
        return -1;
    }
    off += slen;
    if (off != der_len) {
        return -1;
    }
    return p256_ecdsa_verify(pub, hash, sig);
}
