#ifndef NT_PING_REPLY_H
#define NT_PING_REPLY_H

#include <stdint.h>

struct nt_ping_reply {
    uint32_t src;
    uint16_t id;
    uint16_t seq;
    uint32_t rtt_ms;
};

#endif
