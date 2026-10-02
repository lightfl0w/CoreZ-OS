#ifndef LIB_TLS_X509_H
#define LIB_TLS_X509_H

#include <stdint.h>

#define X509_MAX_SAN 8
#define X509_MAX_SAN_LEN 128
#define X509_MAX_CHAIN 6

enum {
    X509_PUB_RSA = 1,
    X509_PUB_EC_P256 = 2,
};

enum {
    X509_SIG_UNKNOWN = 0,
    X509_SIG_RSA_PKCS1_SHA256 = 1,
    X509_SIG_RSA_PSS_SHA256 = 2,
    X509_SIG_ECDSA_SHA256 = 3,
};

enum {
    X509_OK = 0,
    X509_ERR_FORMAT = -1,
    X509_ERR_SIG = -2,
    X509_ERR_TIME = -3,
    X509_ERR_HOSTNAME = -4,
    X509_ERR_NO_ROOT = -5,
    X509_ERR_CA = -6,
    X509_ERR_UNSUPPORTED = -7,
};

struct x509_cert {
    const uint8_t *der;
    uint32_t der_len;
    const uint8_t *tbs;
    uint32_t tbs_len;
    const uint8_t *issuer;
    uint32_t issuer_len;
    const uint8_t *subject;
    uint32_t subject_len;
    int64_t not_before;
    int64_t not_after;
    int pub_alg;
    const uint8_t *pub;
    uint32_t pub_len;
    int sig_alg;
    uint32_t pss_salt_len;
    int is_ca;
    int key_cert_sign;
    int has_server_auth;
    uint32_t san_count;
    char san[X509_MAX_SAN][X509_MAX_SAN_LEN];
};

struct tls_root_ca {
    const char *name;
    const uint8_t *der;
    uint32_t der_len;
};

extern const struct tls_root_ca tls_builtin_roots[];
extern const unsigned tls_builtin_roots_count;

int x509_parse(const uint8_t *der, uint32_t len, struct x509_cert *out);
int x509_verify_signature(const struct x509_cert *subject,
                          const struct x509_cert *issuer);
int x509_check_hostname(const struct x509_cert *cert, const char *hostname);
int x509_verify_chain(const struct x509_cert *chain, uint32_t chain_len,
                      const char *hostname, int64_t now);

#endif
