#ifndef LIB_CRYPTO_SHA256_H
#define LIB_CRYPTO_SHA256_H

#include <stdint.h>

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    uint32_t buf_len;
};

struct hmac_sha256_ctx {
    struct sha256_ctx inner;
    struct sha256_ctx outer;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, uint32_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, uint32_t len, uint8_t out[32]);

void hmac_sha256_init(struct hmac_sha256_ctx *c, const uint8_t *key,
                      uint32_t key_len);
void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data,
                        uint32_t len);
void hmac_sha256_final(struct hmac_sha256_ctx *c, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, uint32_t key_len, const uint8_t *msg,
                 uint32_t msg_len, uint8_t out[32]);

void hkdf_extract(const uint8_t *salt, uint32_t salt_len, const uint8_t *ikm,
                  uint32_t ikm_len, uint8_t out[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t info_len,
                 uint8_t *out, uint32_t out_len);
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, uint32_t context_len,
                       uint8_t *out, uint32_t out_len);

#endif
