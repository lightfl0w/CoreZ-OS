#ifndef DRIVERS_NET_DNS_H
#define DRIVERS_NET_DNS_H

#include <stdint.h>

void dns_set_nameserver(uint32_t ns);
uint32_t dns_nameserver(void);
uint32_t dns_resolve(const char *hostname, uint32_t *out_ip);

#endif