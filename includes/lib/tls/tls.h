#ifndef LIB_TLS_TLS_H
#define LIB_TLS_TLS_H

#include <stdint.h>

#include "lib/tls/x509.h"

enum {
    TLS_ERR_IO = -1,
    TLS_ERR_PROTOCOL = -2,
    TLS_ERR_ALERT = -3,
    TLS_ERR_CRYPTO = -4,
    TLS_ERR_CERT = -5,
    TLS_ERR_BUSY = -6,
};

enum {
    TLS_CIPHER_AES_128_GCM_SHA256 = 0x1301,
    TLS_CIPHER_CHACHA20_POLY1305_SHA256 = 0x1303,
};

struct tls_transport {
    int (*send)(void *ctx, const uint8_t *buf, uint32_t len);
    int (*recv)(void *ctx, uint8_t *buf, uint32_t len);
    void *ctx;
};

struct tls_config {
    struct tls_transport tr;
    void (*rng)(void *buf, uint32_t len);
    int64_t (*now_unix)(void);
    const char *hostname;
};

struct tls_conn;

struct tls_conn *tls_connect(const struct tls_config *cfg);
int tls_write(struct tls_conn *c, const void *buf, uint32_t len);
int tls_read(struct tls_conn *c, void *buf, uint32_t len);
int tls_close(struct tls_conn *c);
const char *tls_error(struct tls_conn *c);
const struct x509_cert *tls_peer_cert(struct tls_conn *c);
int tls_negotiated_cipher(struct tls_conn *c);

#endif
