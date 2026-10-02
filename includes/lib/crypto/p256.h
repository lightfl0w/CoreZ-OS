#ifndef LIB_CRYPTO_P256_H
#define LIB_CRYPTO_P256_H

#include <stdint.h>

int p256_pub_from_priv(uint8_t pub[64], const uint8_t priv[32]);
int p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[64]);
int p256_ecdsa_verify(const uint8_t pub[64], const uint8_t hash[32],
                      const uint8_t sig[64]);
int p256_ecdsa_verify_der(const uint8_t pub[64], const uint8_t hash[32],
                          const uint8_t *der, uint32_t der_len);

#endif
