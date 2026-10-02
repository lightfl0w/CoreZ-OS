#include <stddef.h>
#include <stdint.h>

#include "lib/crypto/aes_gcm.h"
#include "lib/crypto/chacha20.h"
#include "lib/crypto/p256.h"
#include "lib/crypto/rsa.h"
#include "lib/crypto/sha256.h"
#include "lib/crypto/x25519.h"
#include "lib/str/str.h"
#include "lib/tls/tls.h"

#define TLS_MAX_CONNS 1
#define TLS_REC_IN_SZ 16656
#define TLS_REC_OUT_SZ 16416
#define TLS_HS_BIG_SZ 16384
#define TLS_HS_SMALL_SZ 1024
#define TLS_MAX_FRAG 16384
#define TLS_HDR_LEN 5
#define TLS_TAG_LEN 16
#define TLS_IV_LEN 12
#define TLS_MAX_HOST 220
#define TLS_MAX_COOKIE 508

#define TLS_CT_CHANGE_CIPHER_SPEC 20
#define TLS_CT_ALERT 21
#define TLS_CT_HANDSHAKE 22
#define TLS_CT_APPLICATION_DATA 23

#define TLS_HS_CLIENT_HELLO 1
#define TLS_HS_SERVER_HELLO 2
#define TLS_HS_NEW_SESSION_TICKET 4
#define TLS_HS_ENCRYPTED_EXTENSIONS 8
#define TLS_HS_CERTIFICATE 11
#define TLS_HS_CERTIFICATE_REQUEST 13
#define TLS_HS_CERTIFICATE_VERIFY 15
#define TLS_HS_FINISHED 20
#define TLS_HS_KEY_UPDATE 24

#define TLS_EXT_SERVER_NAME 0x0000
#define TLS_EXT_SUPPORTED_GROUPS 0x000a
#define TLS_EXT_SIGNATURE_ALGORITHMS 0x000d
#define TLS_EXT_PRE_SHARED_KEY 0x0029
#define TLS_EXT_SUPPORTED_VERSIONS 0x002b
#define TLS_EXT_COOKIE 0x002c
#define TLS_EXT_KEY_SHARE 0x0033

#define TLS_GROUP_X25519 0x001d
#define TLS_GROUP_SECP256R1 0x0017

#define TLS_SIG_RSA_PSS_RSAE_SHA256 0x0804
#define TLS_SIG_ECDSA_SECP256R1_SHA256 0x0403

#define TLS_KEY_LEN_AES128 16
#define TLS_KEY_LEN_CHACHA 32

static const uint8_t hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
    0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
    0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static const char srv_cv_context[34] = "TLS 1.3, server CertificateVerify";

static const uint8_t s_zero32[32] = { 0 };

struct tls_keys {
    uint8_t key[32];
    uint8_t iv[TLS_IV_LEN];
};

struct tls_conn {
    struct tls_config cfg;
    int in_use;
    int closed;
    int established;
    int last_err;
    const char *err_msg;
    int cipher;
    int neg_cipher;
    int group;
    int sel_group;
    int want_client_cert;
    int hrr_done;
    uint64_t rd_seq;
    uint64_t wr_seq;
    struct tls_keys rd;
    struct tls_keys wr;
    struct sha256_ctx transcript;
    uint8_t early_secret[32];
    uint8_t hs_secret[32];
    uint8_t master_secret[32];
    uint8_t c_hs[32];
    uint8_t s_hs[32];
    uint8_t c_ap[32];
    uint8_t s_ap[32];
    uint8_t ecdhe[32];
    uint8_t cli_priv[32];
    uint8_t cli_pub[65];
    uint32_t cli_pub_len;
    uint8_t srv_pub[65];
    uint32_t srv_pub_len;
    uint8_t srv_random[32];
    uint8_t sid[32];
    uint32_t sid_len;
    uint8_t cookie[TLS_MAX_COOKIE];
    uint32_t cookie_len;
    uint32_t in_len;
    uint32_t in_off;
    int in_type;
    uint8_t *hs_msg;
    uint32_t hs_msg_len;
    uint8_t in[TLS_REC_IN_SZ];
    uint8_t out[TLS_REC_OUT_SZ];
    uint8_t hs_big[TLS_HS_BIG_SZ];
    uint8_t hs_small[TLS_HS_SMALL_SZ];
    struct x509_cert chain[X509_MAX_CHAIN];
    uint32_t chain_len;
};

static struct tls_conn g_conns[TLS_MAX_CONNS];
static const char *g_last_error;

static void put8(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; }

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static uint32_t get16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static uint32_t get24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static int fail(struct tls_conn *c, int err, const char *msg) {
    if (c) {
        c->last_err = err;
        c->err_msg = msg;
    }
    g_last_error = msg;
    return err;
}

static int tr_send(struct tls_conn *c, const uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        int n = c->cfg.tr.send(c->cfg.tr.ctx, buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}

static int tr_recv(struct tls_conn *c, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        int n = c->cfg.tr.recv(c->cfg.tr.ctx, buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}

static void make_nonce(uint8_t nonce[TLS_IV_LEN], const uint8_t iv[TLS_IV_LEN],
                       uint64_t seq) {
    int i;
    memcpy(nonce, iv, TLS_IV_LEN);
    for (i = 0; i < 8; i++)
        nonce[TLS_IV_LEN - 1 - i] ^= (uint8_t)(seq >> (8 * i));
}

static int aead_seal(struct tls_conn *c, const uint8_t *nonce, const uint8_t *aad,
                     uint8_t *buf, uint32_t len, uint8_t *tag) {
    if (c->cipher == TLS_CIPHER_AES_128_GCM_SHA256) {
        aes_gcm_encrypt(c->wr.key, 128, nonce, aad, TLS_HDR_LEN, buf, buf, len, tag);
        return 0;
    }
    chacha20_poly1305_encrypt(c->wr.key, nonce, aad, TLS_HDR_LEN, buf, buf, len, tag);
    return 0;
}

static int aead_open(struct tls_conn *c, const uint8_t *nonce, const uint8_t *aad,
                     uint8_t *buf, uint32_t len, const uint8_t *tag) {
    if (c->cipher == TLS_CIPHER_AES_128_GCM_SHA256)
        return aes_gcm_decrypt(c->rd.key, 128, nonce, aad, TLS_HDR_LEN, buf, buf,
                               len, tag);
    return chacha20_poly1305_decrypt(c->rd.key, nonce, aad, TLS_HDR_LEN, buf, buf,
                                     len, tag);
}

static int rec_read(struct tls_conn *c) {
    for (;;) {
        uint8_t hdr[TLS_HDR_LEN];
        uint8_t nonce[TLS_IV_LEN];
        uint32_t len;
        uint32_t n;
        int type;

        if (tr_recv(c, hdr, TLS_HDR_LEN) < 0)
            return fail(c, TLS_ERR_IO, "transport read failed");
        len = get16(hdr + 3);
        if (len > TLS_REC_IN_SZ)
            return fail(c, TLS_ERR_PROTOCOL, "record too large");
        if (len && tr_recv(c, c->in, len) < 0)
            return fail(c, TLS_ERR_IO, "transport read failed");
        type = hdr[0];
        if (type == TLS_CT_CHANGE_CIPHER_SPEC)
            continue;
        if (c->cipher) {
            uint32_t clen;
            if (type != TLS_CT_APPLICATION_DATA)
                return fail(c, TLS_ERR_PROTOCOL, "unexpected record type");
            if (len < TLS_TAG_LEN + 1)
                return fail(c, TLS_ERR_PROTOCOL, "short encrypted record");
            clen = len - TLS_TAG_LEN;
            make_nonce(nonce, c->rd.iv, c->rd_seq);
            if (aead_open(c, nonce, hdr, c->in, clen, c->in + clen) != 0)
                return fail(c, TLS_ERR_CRYPTO, "bad record authentication tag");
            c->rd_seq++;
            n = clen;
            while (n > 0 && c->in[n - 1] == 0)
                n--;
            if (n == 0)
                return fail(c, TLS_ERR_PROTOCOL, "empty inner plaintext");
            c->in_type = c->in[n - 1];
            c->in_len = n - 1;
        } else {
            c->in_type = type;
            c->in_len = len;
        }
        c->in_off = 0;
        if (c->in_type == TLS_CT_ALERT) {
            if (c->in_len >= 2 && c->in[1] == 0) {
                c->closed = 1;
                return fail(c, TLS_ERR_IO, "peer sent close_notify");
            }
            return fail(c, TLS_ERR_ALERT, "peer sent a fatal alert");
        }
        if (c->in_len == 0)
            continue;
        return 0;
    }
}

static int rec_pull(struct tls_conn *c, uint8_t *dst, uint32_t len, int type) {
    uint32_t got = 0;
    while (got < len) {
        uint32_t avail;
        uint32_t take;
        if (c->in_off >= c->in_len) {
            if (rec_read(c) < 0)
                return -1;
            if (c->in_type != type)
                return fail(c, TLS_ERR_PROTOCOL, "unexpected content type");
        }
        avail = c->in_len - c->in_off;
        take = len - got;
        if (take > avail)
            take = avail;
        memcpy(dst + got, c->in + c->in_off, take);
        c->in_off += take;
        got += take;
    }
    return 0;
}

static int rec_send(struct tls_conn *c, int type, uint32_t len) {
    uint8_t *p = c->out;
    uint8_t nonce[TLS_IV_LEN];
    uint32_t total;

    if (c->cipher) {
        p[TLS_HDR_LEN + len] = (uint8_t)type;
        total = len + 1;
        put8(p, TLS_CT_APPLICATION_DATA);
        put16(p + 1, 0x0303);
        put16(p + 3, total + TLS_TAG_LEN);
        make_nonce(nonce, c->wr.iv, c->wr_seq);
        aead_seal(c, nonce, p, p + TLS_HDR_LEN, total, p + TLS_HDR_LEN + total);
        c->wr_seq++;
        total += TLS_TAG_LEN;
    } else {
        put8(p, (uint32_t)type);
        put16(p + 1, 0x0303);
        put16(p + 3, len);
        total = len;
    }
    if (tr_send(c, p, TLS_HDR_LEN + total) < 0)
        return fail(c, TLS_ERR_IO, "transport write failed");
    return 0;
}

static int hs_read(struct tls_conn *c, uint32_t *type, const uint8_t **body,
                   uint32_t *blen) {
    uint8_t hdr[4];
    uint8_t *buf;
    uint32_t cap;
    uint32_t l;

    if (rec_pull(c, hdr, 4, TLS_CT_HANDSHAKE) < 0)
        return -1;
    l = get24(hdr + 1);
    if (hdr[0] == TLS_HS_CERTIFICATE) {
        buf = c->hs_big;
        cap = TLS_HS_BIG_SZ;
    } else {
        buf = c->hs_small;
        cap = TLS_HS_SMALL_SZ;
    }
    if (l > cap - 4)
        return fail(c, TLS_ERR_PROTOCOL, "handshake message too large");
    memcpy(buf, hdr, 4);
    if (l && rec_pull(c, buf + 4, l, TLS_CT_HANDSHAKE) < 0)
        return -1;
    c->hs_msg = buf;
    c->hs_msg_len = 4 + l;
    *type = hdr[0];
    *body = buf + 4;
    *blen = l;
    return 0;
}

static void transcript_add(struct tls_conn *c, const uint8_t *msg, uint32_t len) {
    sha256_update(&c->transcript, msg, len);
}

static void transcript_hash(struct tls_conn *c, uint8_t out[32]) {
    struct sha256_ctx tmp = c->transcript;
    sha256_final(&tmp, out);
}

static int gen_key_share(struct tls_conn *c, int group) {
    int i;
    c->group = group;
    if (group == TLS_GROUP_X25519) {
        c->cfg.rng(c->cli_priv, 32);
        x25519_base(c->cli_pub, c->cli_priv);
        c->cli_pub_len = 32;
        return 0;
    }
    if (group == TLS_GROUP_SECP256R1) {
        for (i = 0; i < 64; i++) {
            c->cfg.rng(c->cli_priv, 32);
            if (p256_pub_from_priv(c->cli_pub + 1, c->cli_priv) == 0) {
                c->cli_pub[0] = 0x04;
                c->cli_pub_len = 65;
                return 0;
            }
        }
        return fail(c, TLS_ERR_CRYPTO, "P-256 key generation failed");
    }
    return fail(c, TLS_ERR_PROTOCOL, "unsupported group offered");
}

static int compute_ecdh(struct tls_conn *c) {
    if (c->group == TLS_GROUP_X25519) {
        if (c->srv_pub_len != 32)
            return fail(c, TLS_ERR_PROTOCOL, "bad x25519 key share length");
        x25519(c->ecdhe, c->cli_priv, c->srv_pub);
        return 0;
    }
    if (c->group == TLS_GROUP_SECP256R1) {
        if (c->srv_pub_len != 65 || c->srv_pub[0] != 0x04)
            return fail(c, TLS_ERR_PROTOCOL, "bad P-256 key share");
        if (p256_ecdh(c->ecdhe, c->cli_priv, c->srv_pub + 1) != 0)
            return fail(c, TLS_ERR_CRYPTO, "P-256 key agreement failed");
        return 0;
    }
    return fail(c, TLS_ERR_PROTOCOL, "unsupported group");
}

static uint32_t build_client_hello(struct tls_conn *c) {
    uint8_t *m = c->out + TLS_HDR_LEN;
    uint32_t n = 4;
    uint32_t ext_off;
    uint32_t hlen = (uint32_t)strlen(c->cfg.hostname);

    m[n++] = 0x03;
    m[n++] = 0x03;
    c->cfg.rng(m + n, 32);
    n += 32;
    m[n++] = (uint8_t)c->sid_len;
    memcpy(m + n, c->sid, c->sid_len);
    n += c->sid_len;
    put16(m + n, 4);
    n += 2;
    put16(m + n, TLS_CIPHER_AES_128_GCM_SHA256);
    n += 2;
    put16(m + n, TLS_CIPHER_CHACHA20_POLY1305_SHA256);
    n += 2;
    m[n++] = 1;
    m[n++] = 0;
    ext_off = n;
    n += 2;

    put16(m + n, TLS_EXT_SERVER_NAME);
    n += 2;
    put16(m + n, hlen + 5);
    n += 2;
    put16(m + n, hlen + 3);
    n += 2;
    m[n++] = 0;
    put16(m + n, hlen);
    n += 2;
    memcpy(m + n, c->cfg.hostname, hlen);
    n += hlen;

    put16(m + n, TLS_EXT_SUPPORTED_GROUPS);
    n += 2;
    put16(m + n, 6);
    n += 2;
    put16(m + n, 4);
    n += 2;
    put16(m + n, TLS_GROUP_X25519);
    n += 2;
    put16(m + n, TLS_GROUP_SECP256R1);
    n += 2;

    put16(m + n, TLS_EXT_SIGNATURE_ALGORITHMS);
    n += 2;
    put16(m + n, 6);
    n += 2;
    put16(m + n, 4);
    n += 2;
    put16(m + n, TLS_SIG_RSA_PSS_RSAE_SHA256);
    n += 2;
    put16(m + n, TLS_SIG_ECDSA_SECP256R1_SHA256);
    n += 2;

    put16(m + n, TLS_EXT_SUPPORTED_VERSIONS);
    n += 2;
    put16(m + n, 3);
    n += 2;
    m[n++] = 2;
    put16(m + n, 0x0304);
    n += 2;

    put16(m + n, TLS_EXT_KEY_SHARE);
    n += 2;
    put16(m + n, c->cli_pub_len + 6);
    n += 2;
    put16(m + n, c->cli_pub_len + 4);
    n += 2;
    put16(m + n, (uint32_t)c->group);
    n += 2;
    put16(m + n, c->cli_pub_len);
    n += 2;
    memcpy(m + n, c->cli_pub, c->cli_pub_len);
    n += c->cli_pub_len;

    if (c->cookie_len) {
        put16(m + n, TLS_EXT_COOKIE);
        n += 2;
        put16(m + n, c->cookie_len + 2);
        n += 2;
        put16(m + n, c->cookie_len);
        n += 2;
        memcpy(m + n, c->cookie, c->cookie_len);
        n += c->cookie_len;
    }

    put16(m + ext_off, n - ext_off - 2);
    m[0] = TLS_HS_CLIENT_HELLO;
    put24(m + 1, n - 4);
    return n;
}

static int send_client_hello(struct tls_conn *c) {
    uint32_t len = build_client_hello(c);
    if (len > TLS_REC_OUT_SZ - TLS_HDR_LEN)
        return fail(c, TLS_ERR_PROTOCOL, "ClientHello too large");
    transcript_add(c, c->out + TLS_HDR_LEN, len);
    return rec_send(c, TLS_CT_HANDSHAKE, len);
}

static int parse_server_hello(struct tls_conn *c, const uint8_t *b, uint32_t n,
                              int *is_hrr) {
    uint32_t off;
    uint32_t sidl;
    uint32_t exts;
    uint32_t end;
    int hrr;
    int version_ok = 0;
    int ks_seen = 0;
    int cookie_seen = 0;

    if (n < 38)
        return fail(c, TLS_ERR_PROTOCOL, "short ServerHello");
    if (b[0] != 0x03 || b[1] != 0x03)
        return fail(c, TLS_ERR_PROTOCOL, "bad legacy version");
    memcpy(c->srv_random, b + 2, 32);
    hrr = (memcmp(c->srv_random, hrr_random, 32) == 0);
    sidl = b[34];
    if (35 + sidl + 3 > n)
        return fail(c, TLS_ERR_PROTOCOL, "truncated ServerHello");
    if (sidl != c->sid_len || memcmp(b + 35, c->sid, sidl) != 0)
        return fail(c, TLS_ERR_PROTOCOL, "session id echo mismatch");
    off = 35 + sidl;
    c->neg_cipher = (int)get16(b + off);
    off += 2;
    if (b[off] != 0)
        return fail(c, TLS_ERR_PROTOCOL, "non-null compression");
    off += 1;
    if (off + 2 > n)
        return fail(c, TLS_ERR_PROTOCOL, "missing extensions");
    exts = get16(b + off);
    off += 2;
    if (off + exts > n)
        return fail(c, TLS_ERR_PROTOCOL, "extensions overflow");
    end = off + exts;
    c->cookie_len = 0;
    c->srv_pub_len = 0;
    c->sel_group = 0;

    while (off + 4 <= end) {
        uint32_t et = get16(b + off);
        uint32_t el = get16(b + off + 2);
        const uint8_t *v = b + off + 4;
        off += 4;
        if (off + el > end)
            return fail(c, TLS_ERR_PROTOCOL, "extension overflow");
        if (et == TLS_EXT_SUPPORTED_VERSIONS) {
            if (el != 2 || get16(v) != 0x0304)
                return fail(c, TLS_ERR_PROTOCOL, "server did not pick TLS 1.3");
            version_ok = 1;
        } else if (et == TLS_EXT_KEY_SHARE) {
            if (hrr) {
                if (el != 2)
                    return fail(c, TLS_ERR_PROTOCOL, "bad HelloRetryRequest key_share");
                c->sel_group = (int)get16(v);
            } else {
                uint32_t kl;
                if (el < 4)
                    return fail(c, TLS_ERR_PROTOCOL, "bad key_share");
                c->sel_group = (int)get16(v);
                kl = get16(v + 2);
                if (kl == 0 || kl > 65 || 4 + kl > el)
                    return fail(c, TLS_ERR_PROTOCOL, "bad key share length");
                memcpy(c->srv_pub, v + 4, kl);
                c->srv_pub_len = kl;
            }
            ks_seen = 1;
        } else if (et == TLS_EXT_COOKIE) {
            uint32_t cl;
            if (!hrr)
                continue;
            if (el < 2)
                return fail(c, TLS_ERR_PROTOCOL, "bad cookie extension");
            cl = get16(v);
            if (2 + cl != el || cl > TLS_MAX_COOKIE)
                return fail(c, TLS_ERR_PROTOCOL, "cookie too large");
            memcpy(c->cookie, v + 2, cl);
            c->cookie_len = cl;
            cookie_seen = 1;
        } else if (et == TLS_EXT_PRE_SHARED_KEY) {
            return fail(c, TLS_ERR_PROTOCOL, "server selected an unoffered PSK");
        }
        off += el;
    }

    if (!version_ok)
        return fail(c, TLS_ERR_PROTOCOL, "missing supported_versions");
    if (!ks_seen)
        return fail(c, TLS_ERR_PROTOCOL, "missing key_share");
    if (hrr) {
        if (!cookie_seen && c->hrr_done)
            return fail(c, TLS_ERR_PROTOCOL, "repeated HelloRetryRequest");
        if (c->sel_group != TLS_GROUP_X25519 && c->sel_group != TLS_GROUP_SECP256R1)
            return fail(c, TLS_ERR_PROTOCOL, "HelloRetryRequest asked for an unsupported group");
        *is_hrr = 1;
        return 0;
    }
    if (c->neg_cipher != TLS_CIPHER_AES_128_GCM_SHA256 &&
        c->neg_cipher != TLS_CIPHER_CHACHA20_POLY1305_SHA256)
        return fail(c, TLS_ERR_PROTOCOL, "unsupported cipher suite");
    if (c->sel_group != c->group)
        return fail(c, TLS_ERR_PROTOCOL, "server key share group mismatch");
    *is_hrr = 0;
    return 0;
}

static void derive_traffic(struct tls_conn *c, struct tls_keys *k,
                           const uint8_t secret[32]) {
    uint32_t kl = (c->cipher == TLS_CIPHER_AES_128_GCM_SHA256) ? TLS_KEY_LEN_AES128
                                                               : TLS_KEY_LEN_CHACHA;
    hkdf_expand_label(secret, "key", 0, 0, k->key, kl);
    hkdf_expand_label(secret, "iv", 0, 0, k->iv, TLS_IV_LEN);
}

static int derive_handshake_secrets(struct tls_conn *c) {
    uint8_t empty_hash[32];
    uint8_t derived[32];
    uint8_t th[32];

    sha256(s_zero32, 0, empty_hash);
    hkdf_extract(0, 0, s_zero32, 32, c->early_secret);
    hkdf_expand_label(c->early_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, c->ecdhe, 32, c->hs_secret);
    transcript_hash(c, th);
    hkdf_expand_label(c->hs_secret, "c hs traffic", th, 32, c->c_hs, 32);
    hkdf_expand_label(c->hs_secret, "s hs traffic", th, 32, c->s_hs, 32);
    hkdf_expand_label(c->hs_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, s_zero32, 32, c->master_secret);
    derive_traffic(c, &c->wr, c->c_hs);
    derive_traffic(c, &c->rd, c->s_hs);
    c->wr_seq = 0;
    c->rd_seq = 0;
    return 0;
}

static int der_int(const uint8_t *p, uint32_t len, uint32_t *off,
                   const uint8_t **v, uint32_t *vlen) {
    uint32_t l;
    uint32_t h;
    if (*off + 2 > len)
        return -1;
    if (p[*off] != 0x02)
        return -1;
    l = p[*off + 1];
    h = *off + 2;
    if (l & 0x80) {
        uint32_t k = l & 0x7f;
        uint32_t i;
        if (k == 0 || k > 4 || h + k > len)
            return -1;
        l = 0;
        for (i = 0; i < k; i++)
            l = (l << 8) | p[h + i];
        h += k;
    }
    if (h + l > len)
        return -1;
    while (l > 0 && p[h] == 0) {
        h++;
        l--;
    }
    if (l == 0)
        return -1;
    *v = p + h;
    *vlen = l;
    *off = h + l;
    return 0;
}

static int rsa_pub_parts(const uint8_t *p, uint32_t len, const uint8_t **n,
                         uint32_t *nlen, const uint8_t **e, uint32_t *elen) {
    uint32_t l;
    uint32_t h = 2;
    if (len < 4 || p[0] != 0x30)
        return -1;
    l = p[1];
    if (l & 0x80) {
        uint32_t k = l & 0x7f;
        uint32_t i;
        if (k == 0 || k > 4 || 2 + k > len)
            return -1;
        l = 0;
        for (i = 0; i < k; i++)
            l = (l << 8) | p[2 + i];
        h = 2 + k;
    }
    if (h + l > len)
        return -1;
    if (der_int(p, len, &h, n, nlen) < 0)
        return -1;
    if (der_int(p, len, &h, e, elen) < 0)
        return -1;
    return 0;
}

static int parse_certificate(struct tls_conn *c, const uint8_t *b, uint32_t n) {
    uint32_t off;
    uint32_t listl;
    uint32_t end;

    if (n < 4)
        return fail(c, TLS_ERR_CERT, "short Certificate message");
    off = 1 + b[0];
    if (off + 3 > n)
        return fail(c, TLS_ERR_CERT, "truncated Certificate message");
    listl = get24(b + off);
    off += 3;
    if (off + listl > n)
        return fail(c, TLS_ERR_CERT, "certificate list overflow");
    end = off + listl;
    c->chain_len = 0;
    while (off + 3 <= end) {
        uint32_t cl = get24(b + off);
        uint32_t el;
        off += 3;
        if (cl == 0 || off + cl > end)
            return fail(c, TLS_ERR_CERT, "bad certificate entry");
        if (c->chain_len >= X509_MAX_CHAIN)
            return fail(c, TLS_ERR_CERT, "certificate chain too long");
        if (x509_parse(b + off, cl, &c->chain[c->chain_len]) != X509_OK)
            return fail(c, TLS_ERR_CERT, "certificate parse failed");
        c->chain_len++;
        off += cl;
        if (off + 2 > end)
            return fail(c, TLS_ERR_CERT, "missing certificate extensions");
        el = get16(b + off);
        off += 2;
        if (off + el > end)
            return fail(c, TLS_ERR_CERT, "certificate extensions overflow");
        off += el;
    }
    if (c->chain_len == 0)
        return fail(c, TLS_ERR_CERT, "server sent no certificate");
    return 0;
}

static int verify_certificate_verify(struct tls_conn *c, const uint8_t *b,
                                     uint32_t n) {
    uint8_t th[32];
    uint8_t to_sign[64 + 33 + 1 + 32];
    uint8_t sig_hash[32];
    uint32_t alg;
    uint32_t sl;
    int rc;

    if (n < 4)
        return fail(c, TLS_ERR_CERT, "short CertificateVerify");
    alg = get16(b);
    sl = get16(b + 2);
    if (sl == 0 || 4 + sl > n)
        return fail(c, TLS_ERR_CERT, "bad CertificateVerify signature length");

    memset(to_sign, 0x20, 64);
    memcpy(to_sign + 64, srv_cv_context, 33);
    to_sign[97] = 0;
    transcript_hash(c, th);
    memcpy(to_sign + 98, th, 32);
    sha256(to_sign, sizeof to_sign, sig_hash);

    if (alg == TLS_SIG_RSA_PSS_RSAE_SHA256) {
        const uint8_t *mod;
        const uint8_t *exp;
        uint32_t mlen;
        uint32_t elen;
        if (c->chain[0].pub_alg != X509_PUB_RSA)
            return fail(c, TLS_ERR_CERT, "certificate key is not RSA");
        if (rsa_pub_parts(c->chain[0].pub, c->chain[0].pub_len, &mod, &mlen, &exp,
                          &elen) < 0)
            return fail(c, TLS_ERR_CERT, "bad RSA public key");
        rc = rsa_pss_verify_sha256(mod, mlen, exp, elen, b + 4, sl, sig_hash, 32);
        if (rc != 1)
            return fail(c, TLS_ERR_CERT, "CertificateVerify signature is invalid");
    } else if (alg == TLS_SIG_ECDSA_SECP256R1_SHA256) {
        if (c->chain[0].pub_alg != X509_PUB_EC_P256 || c->chain[0].pub_len != 65)
            return fail(c, TLS_ERR_CERT, "certificate key is not P-256");
        rc = p256_ecdsa_verify_der(c->chain[0].pub + 1, sig_hash, b + 4, sl);
        if (rc != 0)
            return fail(c, TLS_ERR_CERT, "CertificateVerify signature is invalid");
    } else {
        return fail(c, TLS_ERR_CERT, "unsupported CertificateVerify algorithm");
    }
    return 0;
}

static int send_client_finished(struct tls_conn *c) {
    uint8_t fk[32];
    uint8_t th[32];
    uint8_t vd[32];
    uint8_t *p = c->out + TLS_HDR_LEN;

    if (c->want_client_cert) {
        put8(p, TLS_HS_CERTIFICATE);
        put24(p + 1, 4);
        put8(p + 4, 0);
        put24(p + 5, 0);
        transcript_add(c, p, 8);
        if (rec_send(c, TLS_CT_HANDSHAKE, 8) < 0)
            return -1;
    }

    transcript_hash(c, th);
    hkdf_expand_label(c->c_hs, "finished", 0, 0, fk, 32);
    hmac_sha256(fk, 32, th, 32, vd);
    put8(p, TLS_HS_FINISHED);
    put24(p + 1, 32);
    memcpy(p + 4, vd, 32);
    if (rec_send(c, TLS_CT_HANDSHAKE, 36) < 0)
        return -1;
    return 0;
}

static int read_server_finished(struct tls_conn *c) {
    uint8_t fk[32];
    uint8_t th[32];
    uint8_t expect[32];
    uint32_t type;
    const uint8_t *body;
    uint32_t blen;

    if (hs_read(c, &type, &body, &blen) < 0)
        return -1;
    if (type != TLS_HS_FINISHED)
        return fail(c, TLS_ERR_PROTOCOL, "expected Finished");
    if (blen != 32)
        return fail(c, TLS_ERR_PROTOCOL, "bad Finished length");
    transcript_hash(c, th);
    hkdf_expand_label(c->s_hs, "finished", 0, 0, fk, 32);
    hmac_sha256(fk, 32, th, 32, expect);
    if (memcmp(expect, body, 32) != 0)
        return fail(c, TLS_ERR_CRYPTO, "server Finished is invalid");
    transcript_add(c, c->hs_msg, c->hs_msg_len);
    return 0;
}

static int tls_handshake(struct tls_conn *c) {
    uint32_t type;
    const uint8_t *body;
    uint32_t blen;
    int is_hrr = 0;
    int64_t now;
    int rc;

    sha256_init(&c->transcript);
    c->sid_len = 32;
    c->cfg.rng(c->sid, 32);
    c->cookie_len = 0;
    if (gen_key_share(c, TLS_GROUP_X25519) < 0)
        return -1;
    if (send_client_hello(c) < 0)
        return -1;

    for (;;) {
        if (hs_read(c, &type, &body, &blen) < 0)
            return -1;
        if (type != TLS_HS_SERVER_HELLO)
            return fail(c, TLS_ERR_PROTOCOL, "expected ServerHello");
        if (parse_server_hello(c, body, blen, &is_hrr) < 0)
            return -1;
        if (!is_hrr) {
            transcript_add(c, c->hs_msg, c->hs_msg_len);
            break;
        }
        if (c->hrr_done)
            return fail(c, TLS_ERR_PROTOCOL, "second HelloRetryRequest");
        c->hrr_done = 1;
        {
            uint8_t synth[4];
            uint8_t th[32];
            synth[0] = 254;
            synth[1] = 0;
            synth[2] = 0;
            synth[3] = 32;
            transcript_hash(c, th);
            sha256_init(&c->transcript);
            transcript_add(c, synth, 4);
            transcript_add(c, th, 32);
            transcript_add(c, c->hs_msg, c->hs_msg_len);
        }
        if (gen_key_share(c, c->sel_group) < 0)
            return -1;
        if (send_client_hello(c) < 0)
            return -1;
    }

    c->cipher = c->neg_cipher;
    if (compute_ecdh(c) < 0)
        return -1;
    if (derive_handshake_secrets(c) < 0)
        return -1;

    if (hs_read(c, &type, &body, &blen) < 0)
        return -1;
    if (type != TLS_HS_ENCRYPTED_EXTENSIONS)
        return fail(c, TLS_ERR_PROTOCOL, "expected EncryptedExtensions");
    transcript_add(c, c->hs_msg, c->hs_msg_len);

    if (hs_read(c, &type, &body, &blen) < 0)
        return -1;
    if (type == TLS_HS_CERTIFICATE_REQUEST) {
        c->want_client_cert = 1;
        transcript_add(c, c->hs_msg, c->hs_msg_len);
        if (hs_read(c, &type, &body, &blen) < 0)
            return -1;
    }
    if (type != TLS_HS_CERTIFICATE)
        return fail(c, TLS_ERR_PROTOCOL, "expected Certificate");
    if (parse_certificate(c, body, blen) < 0)
        return -1;
    transcript_add(c, c->hs_msg, c->hs_msg_len);

    now = c->cfg.now_unix ? c->cfg.now_unix() : 0;
    rc = x509_verify_chain(c->chain, c->chain_len, c->cfg.hostname, now);
    if (rc != X509_OK)
        return fail(c, TLS_ERR_CERT, "certificate chain verification failed");

    if (hs_read(c, &type, &body, &blen) < 0)
        return -1;
    if (type != TLS_HS_CERTIFICATE_VERIFY)
        return fail(c, TLS_ERR_PROTOCOL, "expected CertificateVerify");
    if (verify_certificate_verify(c, body, blen) < 0)
        return -1;
    transcript_add(c, c->hs_msg, c->hs_msg_len);

    if (read_server_finished(c) < 0)
        return -1;

    {
        uint8_t th[32];
        transcript_hash(c, th);
        hkdf_expand_label(c->master_secret, "c ap traffic", th, 32, c->c_ap, 32);
        hkdf_expand_label(c->master_secret, "s ap traffic", th, 32, c->s_ap, 32);
    }
    derive_traffic(c, &c->rd, c->s_ap);
    c->rd_seq = 0;

    if (send_client_finished(c) < 0)
        return -1;
    derive_traffic(c, &c->wr, c->c_ap);
    c->wr_seq = 0;
    c->established = 1;
    return 0;
}

struct tls_conn *tls_connect(const struct tls_config *cfg) {
    int i;
    if (!cfg || !cfg->tr.send || !cfg->tr.recv || !cfg->rng || !cfg->hostname) {
        fail(0, TLS_ERR_PROTOCOL, "tls_config is incomplete");
        return 0;
    }
    if (strlen(cfg->hostname) == 0 || strlen(cfg->hostname) > TLS_MAX_HOST) {
        fail(0, TLS_ERR_PROTOCOL, "invalid hostname");
        return 0;
    }
    for (i = 0; i < TLS_MAX_CONNS; i++) {
        struct tls_conn *c = &g_conns[i];
        if (c->in_use)
            continue;
        memset(c, 0, sizeof *c);
        c->in_use = 1;
        c->cfg = *cfg;
        if (tls_handshake(c) < 0) {
            c->in_use = 0;
            return 0;
        }
        return c;
    }
    fail(0, TLS_ERR_BUSY, "no free TLS connection slot");
    return 0;
}

static int tls_conn_alive(struct tls_conn *c) {
    return c && c->in_use && c->established;
}

int tls_write(struct tls_conn *c, const void *buf, uint32_t len) {
    const uint8_t *p = buf;
    uint32_t done = 0;
    if (!tls_conn_alive(c))
        return fail(c, TLS_ERR_IO, "connection is not established");
    while (done < len) {
        uint32_t n = len - done;
        if (n > TLS_MAX_FRAG)
            n = TLS_MAX_FRAG;
        memcpy(c->out + TLS_HDR_LEN, p + done, n);
        if (rec_send(c, TLS_CT_APPLICATION_DATA, n) < 0)
            return done ? (int)done : (int)c->last_err;
        done += n;
    }
    return (int)done;
}

int tls_read(struct tls_conn *c, void *buf, uint32_t len) {
    uint8_t *dst = buf;
    uint32_t got = 0;
    if (!tls_conn_alive(c))
        return fail(c, TLS_ERR_IO, "connection is not established");
    if (c->closed)
        return 0;
    if (len == 0)
        return 0;
    while (got < len) {
        uint32_t avail;
        uint32_t take;
        if (c->in_off >= c->in_len) {
            if (rec_read(c) < 0) {
                if (c->closed)
                    return (int)got;
                return got ? (int)got : (int)c->last_err;
            }
            if (c->in_type == TLS_CT_HANDSHAKE) {
                c->in_off = c->in_len;
                continue;
            }
            if (c->in_type != TLS_CT_APPLICATION_DATA)
                return fail(c, TLS_ERR_PROTOCOL, "unexpected post-handshake record");
        }
        avail = c->in_len - c->in_off;
        take = len - got;
        if (take > avail)
            take = avail;
        memcpy(dst + got, c->in + c->in_off, take);
        c->in_off += take;
        got += take;
        break;
    }
    return (int)got;
}

int tls_close(struct tls_conn *c) {
    if (!c || !c->in_use)
        return fail(c, TLS_ERR_IO, "connection is not open");
    if (c->established && c->cipher) {
        uint8_t *p = c->out + TLS_HDR_LEN;
        put8(p, 1);
        put8(p + 1, 0);
        rec_send(c, TLS_CT_ALERT, 2);
    }
    c->in_use = 0;
    return 0;
}

const char *tls_error(struct tls_conn *c) {
    if (c && c->err_msg)
        return c->err_msg;
    if (g_last_error)
        return g_last_error;
    return "no error";
}

const struct x509_cert *tls_peer_cert(struct tls_conn *c) {
    if (!c || !c->in_use || c->chain_len == 0)
        return 0;
    return &c->chain[0];
}

int tls_negotiated_cipher(struct tls_conn *c) {
    if (!tls_conn_alive(c))
        return 0;
    return c->cipher;
}
