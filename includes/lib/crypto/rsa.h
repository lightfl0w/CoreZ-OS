#ifndef LIB_CRYPTO_RSA_H
#define LIB_CRYPTO_RSA_H

#include <stdint.h>

int rsa_pkcs1_v15_verify_sha256(const uint8_t *n, uint32_t n_len,
                                const uint8_t *e, uint32_t e_len,
                                const uint8_t *sig, uint32_t sig_len,
                                const uint8_t hash[32]);

int rsa_pss_verify_sha256(const uint8_t *n, uint32_t n_len, const uint8_t *e,
                          uint32_t e_len, const uint8_t *sig, uint32_t sig_len,
                          const uint8_t hash[32], uint32_t salt_len);

#endif
