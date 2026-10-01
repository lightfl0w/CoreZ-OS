#include "lib/tls/x509.h"

#include <stdint.h>

#include "lib/crypto/p256.h"
#include "lib/crypto/rsa.h"
#include "lib/crypto/sha256.h"

static const uint8_t x5_oid_cn[3] = {0x55, 0x04, 0x03};
static const uint8_t x5_oid_bc[3] = {0x55, 0x1d, 0x13};
static const uint8_t x5_oid_ku[3] = {0x55, 0x1d, 0x0f};
static const uint8_t x5_oid_san[3] = {0x55, 0x1d, 0x11};
static const uint8_t x5_oid_eku[3] = {0x55, 0x1d, 0x25};
static const uint8_t x5_oid_rsa[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};
static const uint8_t x5_oid_rsa_sha256[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b};
static const uint8_t x5_oid_rsa_pss[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a};
static const uint8_t x5_oid_ec[7] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
static const uint8_t x5_oid_ec_p256[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
static const uint8_t x5_oid_ecdsa_sha256[8] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};
static const uint8_t x5_oid_serverauth[8] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
static const uint8_t x5_oid_anyeku[4] = {0x55, 0x1d, 0x25, 0x00};

struct x5_tlv {
    uint8_t tag;
    const uint8_t *hdr;
    const uint8_t *val;
    uint32_t len;
    uint32_t total;
};

static void x5_zero(void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

static void x5_copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static int x5_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint32_t i;
    uint8_t d = 0;
    for (i = 0; i < n; i++) {
        d = (uint8_t)(d | (uint8_t)(a[i] ^ b[i]));
    }
    return d == 0;
}

static uint32_t x5_slen(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int x5_get(const uint8_t *p, const uint8_t *end, struct x5_tlv *t) {
    uint32_t n;
    if (p >= end) {
        return X509_ERR_FORMAT;
    }
    t->tag = *p;
    t->hdr = p;
    if ((t->tag & 0x1fu) == 0x1fu) {
        return X509_ERR_FORMAT;
    }
    p++;
    if (p >= end) {
        return X509_ERR_FORMAT;
    }
    if (*p < 0x80u) {
        n = *p;
        p++;
    } else if (*p == 0x80u) {
        return X509_ERR_FORMAT;
    } else {
        uint32_t nb = (uint32_t)(*p & 0x7fu);
        uint32_t v = 0;
        uint32_t i;
        p++;
        if (nb == 0 || nb > 4) {
            return X509_ERR_FORMAT;
        }
        if ((uint32_t)(end - p) < nb) {
            return X509_ERR_FORMAT;
        }
        if (*p == 0x00u) {
            return X509_ERR_FORMAT;
        }
        for (i = 0; i < nb; i++) {
            v = (v << 8) | (uint32_t)p[i];
        }
        if (v < 0x80u) {
            return X509_ERR_FORMAT;
        }
        p += nb;
        n = v;
    }
    if ((uint32_t)(end - p) < n) {
        return X509_ERR_FORMAT;
    }
    t->val = p;
    t->len = n;
    t->total = (uint32_t)(p - t->hdr) + n;
    return X509_OK;
}

static int x5_oid_eq(const uint8_t *v, uint32_t len, const uint8_t *o, uint32_t olen) {
    return len == olen && x5_eq(v, o, len);
}

static int x5_digits(const uint8_t *p, uint32_t n, uint32_t *out) {
    uint32_t v = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (p[i] < (uint8_t)'0' || p[i] > (uint8_t)'9') {
            return -1;
        }
        v = v * 10u + (uint32_t)(p[i] - (uint8_t)'0');
    }
    *out = v;
    return 0;
}

static int64_t x5_days_from_civil(int64_t y, int64_t m, int64_t d) {
    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;
    y -= (m <= 2) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int x5_time(const struct x5_tlv *t, int64_t *out) {
    const uint8_t *p = t->val;
    uint32_t len = t->len;
    uint32_t i = 0;
    uint32_t y, mo, d, h, mi, s;
    if (t->tag == 0x17u) {
        if (len < 13) {
            return -1;
        }
        if (x5_digits(p, 2, &y) != 0) {
            return -1;
        }
        y += (y < 50) ? 2000u : 1900u;
        i = 2;
    } else if (t->tag == 0x18u) {
        if (len < 15) {
            return -1;
        }
        if (x5_digits(p, 4, &y) != 0) {
            return -1;
        }
        i = 4;
    } else {
        return -1;
    }
    if (x5_digits(p + i, 2, &mo) != 0) {
        return -1;
    }
    if (x5_digits(p + i + 2, 2, &d) != 0) {
        return -1;
    }
    if (x5_digits(p + i + 4, 2, &h) != 0) {
        return -1;
    }
    if (x5_digits(p + i + 6, 2, &mi) != 0) {
        return -1;
    }
    if (x5_digits(p + i + 8, 2, &s) != 0) {
        return -1;
    }
    i += 10;
    if (i < len && p[i] == (uint8_t)'.') {
        i++;
        while (i < len && p[i] >= (uint8_t)'0' && p[i] <= (uint8_t)'9') {
            i++;
        }
    }
    if (i >= len || p[i] != (uint8_t)'Z') {
        return -1;
    }
    i++;
    if (i != len) {
        return -1;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
        return -1;
    }
    *out = x5_days_from_civil((int64_t)y, (int64_t)mo, (int64_t)d) * 86400 +
           (int64_t)h * 3600 + (int64_t)mi * 60 + (int64_t)s;
    return 0;
}

static int x5_rsa_key(const uint8_t *der, uint32_t len, const uint8_t **n, uint32_t *nlen,
                      const uint8_t **e, uint32_t *elen) {
    struct x5_tlv seq, mi, ei;
    if (x5_get(der, der + len, &seq) != X509_OK) {
        return -1;
    }
    if (seq.tag != 0x30u) {
        return -1;
    }
    if (x5_get(seq.val, seq.val + seq.len, &mi) != X509_OK) {
        return -1;
    }
    if (mi.tag != 0x02u) {
        return -1;
    }
    {
        const uint8_t *p = mi.val;
        uint32_t l = mi.len;
        while (l > 1 && p[0] == 0x00u) {
            p++;
            l--;
        }
        if (l == 0) {
            return -1;
        }
        *n = p;
        *nlen = l;
    }
    if (x5_get(mi.val + mi.len, seq.val + seq.len, &ei) != X509_OK) {
        return -1;
    }
    if (ei.tag != 0x02u) {
        return -1;
    }
    {
        const uint8_t *p = ei.val;
        uint32_t l = ei.len;
        while (l > 1 && p[0] == 0x00u) {
            p++;
            l--;
        }
        if (l == 0) {
            return -1;
        }
        *e = p;
        *elen = l;
    }
    return 0;
}

static int x5_name_last_cn(const uint8_t *name, uint32_t name_len, const uint8_t **out_cn,
                           uint32_t *out_len) {
    struct x5_tlv seq, rdn, atv, oid, v;
    const uint8_t *p, *end;
    int found = 0;
    if (name == 0 || name_len == 0) {
        return 0;
    }
    if (x5_get(name, name + name_len, &seq) != X509_OK || seq.tag != 0x30u) {
        return 0;
    }
    end = seq.val + seq.len;
    p = seq.val;
    while (p < end) {
        const uint8_t *rp, *rend;
        if (x5_get(p, end, &rdn) != X509_OK) {
            return found;
        }
        if (rdn.tag != 0x31u) {
            p = rdn.val + rdn.len;
            continue;
        }
        rp = rdn.val;
        rend = rdn.val + rdn.len;
        while (rp < rend) {
            const uint8_t *ap, *aend;
            if (x5_get(rp, rend, &atv) != X509_OK) {
                break;
            }
            if (atv.tag != 0x30u) {
                rp = atv.val + atv.len;
                continue;
            }
            ap = atv.val;
            aend = atv.val + atv.len;
            if (x5_get(ap, aend, &oid) == X509_OK && oid.tag == 0x06u) {
                ap = oid.val + oid.len;
                if (x5_get(ap, aend, &v) == X509_OK &&
                    x5_oid_eq(oid.val, oid.len, x5_oid_cn, 3)) {
                    *out_cn = v.val;
                    *out_len = v.len;
                    found = 1;
                }
            }
            rp = atv.val + atv.len;
        }
        p = rdn.val + rdn.len;
    }
    return found;
}

static int x5_lower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + 32;
    }
    return c;
}

static int x5_host_match(const uint8_t *pat, uint32_t plen, const char *host, uint32_t hlen) {
    uint32_t i;
    if (plen >= 2 && pat[0] == (uint8_t)'*' && pat[1] == (uint8_t)'.') {
        uint32_t d = 0;
        while (d < hlen && host[d] != '.') {
            d++;
        }
        if (d == 0 || d >= hlen) {
            return 0;
        }
        if (plen - 2 != hlen - d - 1) {
            return 0;
        }
        for (i = 0; i < plen - 2; i++) {
            if (x5_lower((int)pat[i + 2]) != x5_lower((int)(uint8_t)host[d + 1 + i])) {
                return 0;
            }
        }
        return 1;
    }
    if (plen != hlen) {
        return 0;
    }
    for (i = 0; i < plen; i++) {
        if (x5_lower((int)pat[i]) != x5_lower((int)(uint8_t)host[i])) {
            return 0;
        }
    }
    return 1;
}

static uint32_t x5_pss_salt(const struct x5_tlv *params) {
    struct x5_tlv f;
    const uint8_t *p, *end;
    if (params->tag != 0x30u) {
        return 0;
    }
    end = params->val + params->len;
    p = params->val;
    while (p < end) {
        if (x5_get(p, end, &f) != X509_OK) {
            return 0;
        }
        if (f.tag == 0xa2u) {
            struct x5_tlv iv;
            if (x5_get(f.val, f.val + f.len, &iv) == X509_OK && iv.tag == 0x02u) {
                uint32_t v = 0;
                uint32_t i;
                for (i = 0; i < iv.len; i++) {
                    v = (v << 8) | (uint32_t)iv.val[i];
                }
                return v;
            }
        }
        p = f.val + f.len;
    }
    return 0;
}

static int x5_sig_alg(const struct x5_tlv *ai, int *alg, uint32_t *salt) {
    struct x5_tlv oid, par;
    const uint8_t *p, *end;
    *alg = X509_SIG_UNKNOWN;
    *salt = 0;
    if (ai->tag != 0x30u) {
        return -1;
    }
    end = ai->val + ai->len;
    p = ai->val;
    if (x5_get(p, end, &oid) != X509_OK || oid.tag != 0x06u) {
        return -1;
    }
    p = oid.val + oid.len;
    if (x5_oid_eq(oid.val, oid.len, x5_oid_rsa_sha256, 9)) {
        *alg = X509_SIG_RSA_PKCS1_SHA256;
        return 0;
    }
    if (x5_oid_eq(oid.val, oid.len, x5_oid_rsa_pss, 9)) {
        *alg = X509_SIG_RSA_PSS_SHA256;
        if (p < end && x5_get(p, end, &par) == X509_OK) {
            *salt = x5_pss_salt(&par);
        }
        return 0;
    }
    if (x5_oid_eq(oid.val, oid.len, x5_oid_ecdsa_sha256, 8)) {
        *alg = X509_SIG_ECDSA_SHA256;
        return 0;
    }
    *alg = X509_SIG_UNKNOWN;
    *salt = 0;
    return 0;
}

static int x5_spki(struct x509_cert *out, const struct x5_tlv *spki) {
    struct x5_tlv alg, oid, par, bs;
    const uint8_t *p, *end, *ap, *aend;
    if (spki->tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    end = spki->val + spki->len;
    p = spki->val;
    if (x5_get(p, end, &alg) != X509_OK || alg.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    p = alg.val + alg.len;
    if (x5_get(alg.val, alg.val + alg.len, &oid) != X509_OK || oid.tag != 0x06u) {
        return X509_ERR_FORMAT;
    }
    if (x5_get(p, end, &bs) != X509_OK || bs.tag != 0x03u) {
        return X509_ERR_FORMAT;
    }
    if (bs.len < 1 || bs.val[0] != 0x00u) {
        return X509_ERR_FORMAT;
    }
    out->pub = bs.val + 1;
    out->pub_len = bs.len - 1;
    ap = oid.val + oid.len;
    aend = alg.val + alg.len;
    if (x5_oid_eq(oid.val, oid.len, x5_oid_rsa, 9)) {
        const uint8_t *n, *e;
        uint32_t nl, el;
        if (ap < aend) {
            if (x5_get(ap, aend, &par) != X509_OK || par.tag != 0x05u) {
                return X509_ERR_UNSUPPORTED;
            }
        }
        if (x5_rsa_key(out->pub, out->pub_len, &n, &nl, &e, &el) != 0) {
            return X509_ERR_FORMAT;
        }
        out->pub_alg = X509_PUB_RSA;
        return X509_OK;
    }
    if (x5_oid_eq(oid.val, oid.len, x5_oid_ec, 7)) {
        if (ap >= aend) {
            return X509_ERR_UNSUPPORTED;
        }
        if (x5_get(ap, aend, &par) != X509_OK) {
            return X509_ERR_FORMAT;
        }
        if (par.tag != 0x06u || !x5_oid_eq(par.val, par.len, x5_oid_ec_p256, 8)) {
            return X509_ERR_UNSUPPORTED;
        }
        if (out->pub_len != 65 || out->pub[0] != 0x04u) {
            return X509_ERR_FORMAT;
        }
        out->pub_alg = X509_PUB_EC_P256;
        return X509_OK;
    }
    return X509_ERR_UNSUPPORTED;
}

static void x5_ext_one(struct x509_cert *out, const uint8_t *oid, uint32_t oidlen,
                       const uint8_t *v, uint32_t vlen) {
    if (x5_oid_eq(oid, oidlen, x5_oid_bc, 3)) {
        struct x5_tlv seq, f;
        if (x5_get(v, v + vlen, &seq) == X509_OK && seq.tag == 0x30u) {
            out->is_ca = 0;
            if (seq.len > 0 && x5_get(seq.val, seq.val + seq.len, &f) == X509_OK &&
                f.tag == 0x01u && f.len == 1 && f.val[0] != 0x00u) {
                out->is_ca = 1;
            }
        }
    } else if (x5_oid_eq(oid, oidlen, x5_oid_ku, 3)) {
        struct x5_tlv bs;
        if (x5_get(v, v + vlen, &bs) == X509_OK && bs.tag == 0x03u && bs.len >= 2) {
            out->key_cert_sign = ((bs.val[1] >> 2) & 1u) ? 1 : 0;
        }
    } else if (x5_oid_eq(oid, oidlen, x5_oid_eku, 3)) {
        struct x5_tlv seq, o;
        const uint8_t *p, *end;
        out->has_server_auth = 0;
        if (x5_get(v, v + vlen, &seq) == X509_OK && seq.tag == 0x30u) {
            end = seq.val + seq.len;
            p = seq.val;
            while (p < end) {
                if (x5_get(p, end, &o) != X509_OK) {
                    break;
                }
                if (o.tag == 0x06u &&
                    (x5_oid_eq(o.val, o.len, x5_oid_serverauth, 8) ||
                     x5_oid_eq(o.val, o.len, x5_oid_anyeku, 4))) {
                    out->has_server_auth = 1;
                }
                p = o.val + o.len;
            }
        }
    } else if (x5_oid_eq(oid, oidlen, x5_oid_san, 3)) {
        struct x5_tlv seq, gn;
        const uint8_t *p, *end;
        if (x5_get(v, v + vlen, &seq) != X509_OK || seq.tag != 0x30u) {
            return;
        }
        end = seq.val + seq.len;
        p = seq.val;
        while (p < end && out->san_count < X509_MAX_SAN) {
            uint32_t cl;
            if (x5_get(p, end, &gn) != X509_OK) {
                break;
            }
            if (gn.tag == 0x82u) {
                cl = gn.len;
                if (cl > X509_MAX_SAN_LEN - 1) {
                    cl = X509_MAX_SAN_LEN - 1;
                }
                x5_copy((uint8_t *)out->san[out->san_count], gn.val, cl);
                out->san[out->san_count][cl] = '\0';
                out->san_count++;
            }
            p = gn.val + gn.len;
        }
    }
}

static void x5_exts(struct x509_cert *out, const struct x5_tlv *wrap) {
    struct x5_tlv seq, ext, oid, crit, v;
    const uint8_t *p, *end;
    if (x5_get(wrap->val, wrap->val + wrap->len, &seq) != X509_OK || seq.tag != 0x30u) {
        return;
    }
    end = seq.val + seq.len;
    p = seq.val;
    while (p < end) {
        const uint8_t *ep, *eend;
        if (x5_get(p, end, &ext) != X509_OK) {
            return;
        }
        if (ext.tag != 0x30u) {
            p = ext.val + ext.len;
            continue;
        }
        ep = ext.val;
        eend = ext.val + ext.len;
        if (x5_get(ep, eend, &oid) != X509_OK || oid.tag != 0x06u) {
            p = ext.val + ext.len;
            continue;
        }
        ep = oid.val + oid.len;
        if (ep < eend && *ep == 0x01u) {
            if (x5_get(ep, eend, &crit) != X509_OK) {
                p = ext.val + ext.len;
                continue;
            }
            ep = crit.val + crit.len;
        }
        if (x5_get(ep, eend, &v) != X509_OK || v.tag != 0x04u) {
            p = ext.val + ext.len;
            continue;
        }
        x5_ext_one(out, oid.val, oid.len, v.val, v.len);
        p = ext.val + ext.len;
    }
}

static int x5_get_sig(const struct x509_cert *c, const uint8_t **sig, uint32_t *siglen) {
    struct x5_tlv cert, t;
    const uint8_t *p;
    uint32_t i;
    if (c->der == 0 || c->der_len == 0) {
        return -1;
    }
    if (x5_get(c->der, c->der + c->der_len, &cert) != X509_OK || cert.tag != 0x30u) {
        return -1;
    }
    p = cert.val;
    for (i = 0; i < 3; i++) {
        if (x5_get(p, cert.val + cert.len, &t) != X509_OK) {
            return -1;
        }
        p = t.val + t.len;
        if (i == 2) {
            if (t.tag != 0x03u || t.len < 1 || t.val[0] != 0x00u) {
                return -1;
            }
            *sig = t.val + 1;
            *siglen = t.len - 1;
            return 0;
        }
    }
    return -1;
}

int x509_parse(const uint8_t *der, uint32_t len, struct x509_cert *out) {
    struct x5_tlv cert, tbs, sigalg, sigval, f;
    const uint8_t *p, *e;
    int r;
    if (der == 0 || out == 0 || len == 0) {
        return X509_ERR_FORMAT;
    }
    x5_zero(out, (uint32_t)sizeof(*out));
    out->key_cert_sign = 1;
    out->has_server_auth = 1;
    if (x5_get(der, der + len, &cert) != X509_OK || cert.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    if (cert.total != len) {
        return X509_ERR_FORMAT;
    }
    out->der = der;
    out->der_len = len;
    p = cert.val;
    e = cert.val + cert.len;
    if (x5_get(p, e, &tbs) != X509_OK || tbs.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    p = tbs.val + tbs.len;
    if (x5_get(p, e, &sigalg) != X509_OK || sigalg.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    p = sigalg.val + sigalg.len;
    if (x5_get(p, e, &sigval) != X509_OK || sigval.tag != 0x03u) {
        return X509_ERR_FORMAT;
    }
    p = sigval.val + sigval.len;
    if (p != e) {
        return X509_ERR_FORMAT;
    }
    out->tbs = tbs.hdr;
    out->tbs_len = tbs.total;
    if (x5_sig_alg(&sigalg, &out->sig_alg, &out->pss_salt_len) != 0) {
        return X509_ERR_UNSUPPORTED;
    }
    p = tbs.val;
    e = tbs.val + tbs.len;
    if (x5_get(p, e, &f) != X509_OK) {
        return X509_ERR_FORMAT;
    }
    if (f.tag == 0xa0u) {
        p = f.val + f.len;
        if (x5_get(p, e, &f) != X509_OK) {
            return X509_ERR_FORMAT;
        }
    }
    if (f.tag != 0x02u) {
        return X509_ERR_FORMAT;
    }
    p = f.val + f.len;
    if (x5_get(p, e, &f) != X509_OK || f.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    p = f.val + f.len;
    if (x5_get(p, e, &f) != X509_OK || f.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    out->issuer = f.hdr;
    out->issuer_len = f.total;
    p = f.val + f.len;
    if (x5_get(p, e, &f) != X509_OK || f.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    {
        struct x5_tlv nb, na;
        const uint8_t *vp = f.val;
        const uint8_t *ve = f.val + f.len;
        if (x5_get(vp, ve, &nb) != X509_OK) {
            return X509_ERR_FORMAT;
        }
        if (x5_get(nb.val + nb.len, ve, &na) != X509_OK) {
            return X509_ERR_FORMAT;
        }
        if (x5_time(&nb, &out->not_before) != 0 || x5_time(&na, &out->not_after) != 0) {
            return X509_ERR_FORMAT;
        }
    }
    p = f.val + f.len;
    if (x5_get(p, e, &f) != X509_OK || f.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    out->subject = f.hdr;
    out->subject_len = f.total;
    p = f.val + f.len;
    if (x5_get(p, e, &f) != X509_OK || f.tag != 0x30u) {
        return X509_ERR_FORMAT;
    }
    r = x5_spki(out, &f);
    if (r != X509_OK) {
        return r;
    }
    p = f.val + f.len;
    while (p < e) {
        if (x5_get(p, e, &f) != X509_OK) {
            return X509_ERR_FORMAT;
        }
        if (f.tag == 0xa3u) {
            x5_exts(out, &f);
        } else if (f.tag != 0x81u && f.tag != 0x82u) {
            return X509_ERR_FORMAT;
        }
        p = f.val + f.len;
    }
    return X509_OK;
}

int x509_verify_signature(const struct x509_cert *subject, const struct x509_cert *issuer) {
    uint8_t hash[32];
    const uint8_t *sig;
    uint32_t siglen;
    if (subject == 0 || issuer == 0 || subject->tbs == 0) {
        return X509_ERR_UNSUPPORTED;
    }
    if (x5_get_sig(subject, &sig, &siglen) != 0) {
        return X509_ERR_SIG;
    }
    sha256(subject->tbs, subject->tbs_len, hash);
    if (subject->sig_alg == X509_SIG_RSA_PKCS1_SHA256 ||
        subject->sig_alg == X509_SIG_RSA_PSS_SHA256) {
        const uint8_t *n, *ee;
        uint32_t nl, el;
        if (issuer->pub_alg != X509_PUB_RSA) {
            return X509_ERR_UNSUPPORTED;
        }
        if (x5_rsa_key(issuer->pub, issuer->pub_len, &n, &nl, &ee, &el) != 0) {
            return X509_ERR_UNSUPPORTED;
        }
        if (subject->sig_alg == X509_SIG_RSA_PKCS1_SHA256) {
            if (rsa_pkcs1_v15_verify_sha256(n, nl, ee, el, sig, siglen, hash) == 0) {
                return X509_ERR_SIG;
            }
        } else {
            if (rsa_pss_verify_sha256(n, nl, ee, el, sig, siglen, hash,
                                      subject->pss_salt_len) == 0) {
                return X509_ERR_SIG;
            }
        }
        return X509_OK;
    }
    if (subject->sig_alg == X509_SIG_ECDSA_SHA256) {
        if (issuer->pub_alg != X509_PUB_EC_P256) {
            return X509_ERR_UNSUPPORTED;
        }
        if (issuer->pub == 0 || issuer->pub_len != 65 || issuer->pub[0] != 0x04u) {
            return X509_ERR_UNSUPPORTED;
        }
        if (p256_ecdsa_verify_der(issuer->pub + 1, hash, sig, siglen) != 0) {
            return X509_ERR_SIG;
        }
        return X509_OK;
    }
    return X509_ERR_UNSUPPORTED;
}

int x509_check_hostname(const struct x509_cert *cert, const char *hostname) {
    uint32_t i;
    uint32_t hlen;
    if (cert == 0 || hostname == 0) {
        return X509_ERR_HOSTNAME;
    }
    hlen = x5_slen(hostname);
    if (cert->san_count > 0) {
        for (i = 0; i < cert->san_count; i++) {
            const char *s = cert->san[i];
            if (x5_host_match((const uint8_t *)s, x5_slen(s), hostname, hlen)) {
                return X509_OK;
            }
        }
        return X509_ERR_HOSTNAME;
    }
    {
        const uint8_t *cn = 0;
        uint32_t cn_len = 0;
        if (x5_name_last_cn(cert->subject, cert->subject_len, &cn, &cn_len) &&
            x5_host_match(cn, cn_len, hostname, hlen)) {
            return X509_OK;
        }
    }
    return X509_ERR_HOSTNAME;
}

static int x5_anchor(const struct x509_cert *last, int64_t now) {
    struct x509_cert cand;
    unsigned i;
    int selfsigned;
    selfsigned = last->issuer != 0 && last->subject != 0 &&
                 last->issuer_len == last->subject_len &&
                 x5_eq(last->issuer, last->subject, last->issuer_len);
    for (i = 0; i < tls_builtin_roots_count; i++) {
        if (tls_builtin_roots[i].der_len == last->der_len &&
            x5_eq(tls_builtin_roots[i].der, last->der, last->der_len)) {
            int vr = x509_verify_signature(last, last);
            if (selfsigned && last->is_ca != 0 && last->key_cert_sign != 0 &&
                (vr == X509_OK || vr == X509_ERR_UNSUPPORTED)) {
                if (now < last->not_before || now > last->not_after) {
                    return X509_ERR_TIME;
                }
                return X509_OK;
            }
        }
    }
    for (i = 0; i < tls_builtin_roots_count; i++) {
        if (x509_parse(tls_builtin_roots[i].der, tls_builtin_roots[i].der_len, &cand) != X509_OK) {
            continue;
        }
        if (cand.subject_len != last->issuer_len ||
            !x5_eq(cand.subject, last->issuer, last->issuer_len)) {
            continue;
        }
        if (cand.is_ca == 0 || cand.key_cert_sign == 0) {
            continue;
        }
        if (x509_verify_signature(last, &cand) != X509_OK) {
            continue;
        }
        if (now < cand.not_before || now > cand.not_after) {
            return X509_ERR_TIME;
        }
        return X509_OK;
    }
    return X509_ERR_NO_ROOT;
}

int x509_verify_chain(const struct x509_cert *chain, uint32_t chain_len, const char *hostname,
                      int64_t now) {
    uint32_t i;
    if (chain == 0 || hostname == 0) {
        return X509_ERR_FORMAT;
    }
    if (chain_len == 0 || chain_len > X509_MAX_CHAIN) {
        return X509_ERR_FORMAT;
    }
    for (i = 0; i < chain_len; i++) {
        if (now < chain[i].not_before || now > chain[i].not_after) {
            return X509_ERR_TIME;
        }
    }
    if (chain[0].is_ca != 0 || chain[0].has_server_auth == 0) {
        return X509_ERR_CA;
    }
    if (x509_check_hostname(&chain[0], hostname) != X509_OK) {
        return X509_ERR_HOSTNAME;
    }
    for (i = 0; i + 1 < chain_len; i++) {
        int vr;
        if (chain[i].issuer_len != chain[i + 1].subject_len ||
            !x5_eq(chain[i].issuer, chain[i + 1].subject, chain[i].issuer_len)) {
            return X509_ERR_CA;
        }
        if (chain[i + 1].is_ca == 0 || chain[i + 1].key_cert_sign == 0) {
            return X509_ERR_CA;
        }
        vr = x509_verify_signature(&chain[i], &chain[i + 1]);
        if (vr != X509_OK) {
            return vr;
        }
    }
    return x5_anchor(&chain[chain_len - 1], now);
}
