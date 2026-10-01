#ifndef DRIVERS_NET_KTLS_H
#define DRIVERS_NET_KTLS_H

#include <stdint.h>

#include "lib/tls/tls.h"

void ktls_set_nameserver(uint32_t ns);
uint32_t ktls_nameserver(void);
uint32_t ktls_resolve(const char *hostname, uint32_t *out_ip);

struct tls_conn *ktls_connect(uint32_t ip, uint16_t port, const char *hostname);
int ktls_write(struct tls_conn *c, const void *buf, uint32_t len);
int ktls_read(struct tls_conn *c, void *buf, uint32_t len);
int ktls_close(struct tls_conn *c);

#endif
