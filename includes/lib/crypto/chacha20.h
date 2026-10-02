#ifndef LIB_CRYPTO_CHACHA20_H
#define LIB_CRYPTO_CHACHA20_H

#include <stdint.h>

void chacha20_block(const uint32_t key[8], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint32_t counter, const uint8_t *in, uint8_t *out,
                  uint32_t len);

void poly1305(const uint8_t key[32], const uint8_t *msg, uint32_t len,
              uint8_t tag[16]);

void chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, uint32_t aad_len,
                               const uint8_t *in, uint8_t *out, uint32_t len,
                               uint8_t tag[16]);
int chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, uint32_t aad_len,
                              const uint8_t *in, uint8_t *out, uint32_t len,
                              const uint8_t tag[16]);

#endif
