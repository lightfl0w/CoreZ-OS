#include <stdint.h>

#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "lib/str/str.h"
#include "lib/tls/tls.h"
#include "lib/tls/x509.h"
#include "syscall.h"

#define LINUX_SYS_read 0
#define LINUX_SYS_write 1
#define LINUX_SYS_close 3
#define LINUX_SYS_socket 41
#define LINUX_SYS_connect 42
#define LINUX_SYS_sendto 44
#define LINUX_SYS_recvfrom 45
#define LINUX_SYS_clock_gettime 228
#define LINUX_SYS_getrandom 318
#define LINUX_SYS_poll 7

#define POLLIN 0x001
#define DNS_TRIES 20
#define DNS_WAIT_MS 500

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define CLOCK_REALTIME 0

#define TEST_HOST "www.rust-lang.org"
#define DNS_PORT 53
#define DNS_LOCAL_PORT 53211
#define NS_IP 0x0A000203u

static long lsys(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static long lsys3(long n, long a, long b, long c) {
    return lsys(n, a, b, c, 0, 0, 0);
}

static void fill_sin(uint8_t *sa, uint32_t ip, uint16_t port) {
    memset(sa, 0, 16);
    sa[0] = AF_INET & 0xFF;
    sa[1] = 0;
    sa[2] = (uint8_t)((port >> 8) & 0xFF);
    sa[3] = (uint8_t)(port & 0xFF);
    sa[4] = (uint8_t)(ip & 0xFF);
    sa[5] = (uint8_t)((ip >> 8) & 0xFF);
    sa[6] = (uint8_t)((ip >> 16) & 0xFF);
    sa[7] = (uint8_t)((ip >> 24) & 0xFF);
}

static int dns_query(const char *host, uint32_t *out_ip) {
    static uint8_t q[512];
    static uint8_t r[1024];
    uint8_t sa[16];
    uint32_t n = 0;
    const char *p = host;
    uint16_t id;
    int fd;
    int tries;

    id = (uint16_t)(getpid() ^ 0x4B1Du);
    q[n++] = (uint8_t)(id >> 8);
    q[n++] = (uint8_t)id;
    q[n++] = 0x01;
    q[n++] = 0x00;
    q[n++] = 0;
    q[n++] = 1;
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 0;
    while (*p) {
        const char *dot = p;
        uint32_t l;
        while (*dot && *dot != '.')
            dot++;
        l = (uint32_t)(dot - p);
        if (l == 0 || l > 63 || n + l + 6 > sizeof q)
            return 0;
        q[n++] = (uint8_t)l;
        memcpy(q + n, p, l);
        n += l;
        if (!*dot)
            break;
        p = dot + 1;
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1;
    q[n++] = 0;
    q[n++] = 1;

    fd = (int)lsys3(LINUX_SYS_socket, AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    fill_sin(sa, 0, 0);
    lsys3(49, fd, (long)sa, 16);
    fill_sin(sa, NS_IP, DNS_PORT);
    if (lsys(44, fd, (long)q, n, 0, (long)sa, 16) < 0) {
        lsys3(LINUX_SYS_close, fd, 0, 0);
        return 0;
    }
    for (tries = 0; tries < DNS_TRIES; tries++) {
        struct pollfd pfd;
        uint8_t from[16];
        uint32_t alen = 16;
        long len;
        uint32_t qd;
        uint32_t an;
        uint32_t off;
        uint32_t i;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (lsys3(LINUX_SYS_poll, (long)&pfd, 1, DNS_WAIT_MS) <= 0)
            continue;
        if (!(pfd.revents & POLLIN))
            continue;
        len = lsys(45, fd, (long)r, sizeof r, 0, (long)from, (long)&alen);
        if (len <= 0)
            continue;
        if ((((uint32_t)r[0] << 8) | r[1]) != (uint32_t)id)
            continue;
        if ((r[3] & 0x0F) != 0)
            break;
        qd = ((uint32_t)r[4] << 8) | r[5];
        an = ((uint32_t)r[6] << 8) | r[7];
        off = 12;
        for (i = 0; i < qd; i++) {
            while (off < (uint32_t)len) {
                uint32_t l = r[off];
                if (l == 0) {
                    off++;
                    break;
                }
                if ((l & 0xC0) == 0xC0) {
                    off += 2;
                    break;
                }
                off += 1 + l;
            }
            off += 4;
        }
        for (i = 0; i < an; i++) {
            uint32_t type;
            uint32_t rdlen;
            while (off < (uint32_t)len) {
                uint32_t l = r[off];
                if (l == 0) {
                    off++;
                    break;
                }
                if ((l & 0xC0) == 0xC0) {
                    off += 2;
                    break;
                }
                off += 1 + l;
            }
            if (off + 10 > (uint32_t)len)
                break;
            type = ((uint32_t)r[off] << 8) | r[off + 1];
            rdlen = ((uint32_t)r[off + 8] << 8) | r[off + 9];
            off += 10;
            if (off + rdlen > (uint32_t)len)
                break;
            if (type == 1 && rdlen == 4) {
                *out_ip = ((uint32_t)r[off] << 24) | ((uint32_t)r[off + 1] << 16) |
                          ((uint32_t)r[off + 2] << 8) | r[off + 3];
                lsys3(LINUX_SYS_close, fd, 0, 0);
                return 1;
            }
            off += rdlen;
        }
    }
    lsys3(LINUX_SYS_close, fd, 0, 0);
    return 0;
}

static int g_fd = -1;

static int tr_send(void *ctx, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    uint32_t done = 0;
    while (done < len) {
        long n = lsys3(LINUX_SYS_write, g_fd, (long)(buf + done), len - done);
        if (n <= 0)
            return done ? (int)done : -1;
        done += (uint32_t)n;
    }
    return (int)done;
}

static int tr_recv(void *ctx, uint8_t *buf, uint32_t len) {
    (void)ctx;
    long n = lsys3(LINUX_SYS_read, g_fd, (long)buf, len);
    if (n <= 0)
        return -1;
    return (int)n;
}

static void rng(void *buf, uint32_t len) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < len) {
        long n = lsys3(LINUX_SYS_getrandom, (long)(p + got), len - got, 0);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
}

struct ltimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static int64_t now_unix(void) {
    struct ltimespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    lsys3(LINUX_SYS_clock_gettime, CLOCK_REALTIME, (long)&ts, 0);
    return ts.tv_sec;
}

static void fmt_ip(uint32_t ip, char *out) {
    sprintf(out, "%d.%d.%d.%d", (int)((ip >> 24) & 0xFF),
            (int)((ip >> 16) & 0xFF), (int)((ip >> 8) & 0xFF), (int)(ip & 0xFF));
}

int main(void) {
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n";
    struct tls_config cfg;
    struct tls_conn *c;
    const struct x509_cert *peer;
    uint8_t sa[16];
    char ipstr[20];
    uint8_t buf[1024];
    uint32_t ip = 0;
    int total = 0;
    int r;

    if (!dns_query(TEST_HOST, &ip)) {
        printf("apktls: dns failed\n");
        return 1;
    }
    fmt_ip(ip, ipstr);
    printf("apktls: %s -> %s (linux syscalls)\n", TEST_HOST, ipstr);

    g_fd = (int)lsys3(LINUX_SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) {
        printf("apktls: socket failed\n");
        return 1;
    }
    fill_sin(sa, ip, 443);
    if (lsys3(LINUX_SYS_connect, g_fd, (long)sa, 16) < 0) {
        printf("apktls: connect failed\n");
        return 1;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.tr.send = tr_send;
    cfg.tr.recv = tr_recv;
    cfg.tr.ctx = 0;
    cfg.rng = rng;
    cfg.now_unix = now_unix;
    cfg.hostname = TEST_HOST;

    c = tls_connect(&cfg);
    if (!c) {
        printf("apktls: handshake failed: %s\n", tls_error(0));
        return 1;
    }
    printf("apktls: handshake ok cipher=0x%x\n", tls_negotiated_cipher(c));
    peer = tls_peer_cert(c);
    if (peer)
        printf("apktls: peer pub_alg=%d san0=%s\n", peer->pub_alg,
               peer->san_count ? peer->san[0] : "(none)");

    if (tls_write(c, req, sizeof req - 1) != (int)(sizeof req - 1)) {
        printf("apktls: write failed\n");
        return 1;
    }
    for (;;) {
        r = tls_read(c, buf, sizeof buf);
        if (r <= 0)
            break;
        total += r;
        if (total > 65536)
            break;
    }
    printf("apktls: read %d bytes\n", total);
    tls_close(c);
    if (total > 0) {
        printf("APKTLS_PASS\n");
        return 0;
    }
    return 1;
}