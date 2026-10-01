#include "drivers/net/dns.h"

#include "drivers/net/net.h"
#include "drivers/net/socket.h"
#include "lib/str/str.h"

#define DNS_PORT 53
#define DNS_TRIES 50
#define DNS_WAIT_MS 200
#define DNS_FDSET_WORDS (SEL_FD_SET_BYTES / 4)
#define DNS_NS_DEFAULT 0x0A000203u

static uint32_t s_ns = DNS_NS_DEFAULT;

void dns_set_nameserver(uint32_t ns) {
    if (ns)
        s_ns = ns;
}

uint32_t dns_nameserver(void) { return s_ns; }

static void dns_fd_set(uint32_t *set, int fd) {
    if (fd < 0 || (uint32_t)fd >= SEL_FD_SET_FDS)
        return;
    set[fd / 32] |= 1u << (fd % 32);
}

static int dns_build_query(const char *host, uint8_t *out, uint16_t id) {
    uint32_t n = 0;
    const char *p = host;
    if (strlen(host) > 253)
        return -1;
    out[n++] = (uint8_t)(id >> 8);
    out[n++] = (uint8_t)id;
    out[n++] = 0x01;
    out[n++] = 0x00;
    out[n++] = 0;
    out[n++] = 1;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    while (*p) {
        const char *dot = p;
        uint32_t l;
        while (*dot && *dot != '.')
            dot++;
        l = (uint32_t)(dot - p);
        if (l == 0 || l > 63 || n + l + 6 > 512)
            return -1;
        out[n++] = (uint8_t)l;
        memcpy(out + n, p, l);
        n += l;
        if (!*dot)
            break;
        p = dot + 1;
    }
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 1;
    out[n++] = 0;
    out[n++] = 1;
    return (int)n;
}

static int dns_skip_name(const uint8_t *b, uint32_t len, uint32_t *off) {
    for (;;) {
        uint32_t l;
        if (*off >= len)
            return -1;
        l = b[*off];
        if (l == 0) {
            *off += 1;
            return 0;
        }
        if ((l & 0xC0) == 0xC0) {
            *off += 2;
            return 0;
        }
        if (l > 63)
            return -1;
        *off += 1 + l;
    }
}

static int dns_parse_answer(const uint8_t *b, uint32_t len, uint16_t id,
                            uint32_t *ip) {
    uint32_t qd;
    uint32_t an;
    uint32_t off = 12;
    uint32_t i;
    if (len < 12)
        return -1;
    if ((((uint32_t)b[0] << 8) | b[1]) != (uint32_t)id)
        return -1;
    if ((b[3] & 0x0F) != 0)
        return -1;
    qd = ((uint32_t)b[4] << 8) | b[5];
    an = ((uint32_t)b[6] << 8) | b[7];
    for (i = 0; i < qd; i++) {
        if (dns_skip_name(b, len, &off) < 0)
            return -1;
        off += 4;
    }
    for (i = 0; i < an; i++) {
        uint32_t type;
        uint32_t rdlen;
        if (dns_skip_name(b, len, &off) < 0)
            return -1;
        if (off + 10 > len)
            return -1;
        type = ((uint32_t)b[off] << 8) | b[off + 1];
        rdlen = ((uint32_t)b[off + 8] << 8) | b[off + 9];
        off += 10;
        if (off + rdlen > len)
            return -1;
        if (type == 1 && rdlen == 4) {
            *ip = ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
                  ((uint32_t)b[off + 2] << 8) | b[off + 3];
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

uint32_t dns_resolve(const char *hostname, uint32_t *out_ip) {
    static uint8_t q[512];
    static uint8_t r[1024];
    uint16_t id;
    int fd;
    int qn;
    int tries;

    if (!hostname || !out_ip)
        return 0;
    id = (uint16_t)(net_now_ms() ^ (uint32_t)(uintptr_t)hostname ^ 0x5A5Au);
    if (id == 0)
        id = 1;

    fd = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    if (net_bind(fd, 0, 0) < 0) {
        net_close(fd);
        return 0;
    }
    qn = dns_build_query(hostname, q, id);
    if (qn < 0) {
        net_close(fd);
        return 0;
    }
    if (net_sendto(fd, q, (uint32_t)qn, s_ns, DNS_PORT) < 0) {
        net_close(fd);
        return 0;
    }
    for (tries = 0; tries < DNS_TRIES; tries++) {
        uint32_t rf[DNS_FDSET_WORDS];
        uint32_t saddr = 0;
        uint16_t sport = 0;
        int n;
        memset(rf, 0, sizeof rf);
        dns_fd_set(rf, fd);
        if (net_select(fd + 1, rf, 0, 0, DNS_WAIT_MS) <= 0)
            continue;
        n = net_recvfrom(fd, r, sizeof r, &saddr, &sport);
        if (n <= 0)
            continue;
        if (dns_parse_answer(r, (uint32_t)n, id, out_ip) == 0) {
            net_close(fd);
            return *out_ip;
        }
    }
    net_close(fd);
    return 0;
}