#ifndef DRIVERS_NET_TLS_H
#define DRIVERS_NET_TLS_H

#include <stdint.h>

#include "lib/tls/tls.h"

struct tls_conn *tls_connect_tcp(uint32_t ip, uint16_t port,
                                 const char *hostname);
int tls_write_tcp(struct tls_conn *c, const void *buf, uint32_t len);
int tls_read_tcp(struct tls_conn *c, void *buf, uint32_t len);
int tls_close_tcp(struct tls_conn *c);

#endif