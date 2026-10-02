#ifndef LIB_CRYPTO_X25519_H
#define LIB_CRYPTO_X25519_H

#include <stdint.h>

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#endif
