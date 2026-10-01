#ifndef LIB_CRYPTO_AES_GCM_H
#define LIB_CRYPTO_AES_GCM_H

#include <stdint.h>

void aes_gcm_encrypt(const uint8_t *key, uint32_t key_bits,
                     const uint8_t nonce[12], const uint8_t *aad,
                     uint32_t aad_len, const uint8_t *in, uint8_t *out,
                     uint32_t len, uint8_t tag[16]);
int aes_gcm_decrypt(const uint8_t *key, uint32_t key_bits,
                    const uint8_t nonce[12], const uint8_t *aad,
                    uint32_t aad_len, const uint8_t *in, uint8_t *out,
                    uint32_t len, const uint8_t tag[16]);

#endif
