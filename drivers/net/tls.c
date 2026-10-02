#include "drivers/net/tls.h"

#include "drivers/char/rtc.h"
#include "drivers/net/net.h"
#include "drivers/net/socket.h"
#include "lib/rand/rand.h"
#include "lib/str/str.h"

static int s_fd = -1;

static int tls_tr_send(void *ctx, const uint8_t *buf, uint32_t len) {
    int fd = *(int *)ctx;
    int n = net_send(fd, buf, len);
    if (n <= 0)
        return -1;
    return n;
}

static int tls_tr_recv(void *ctx, uint8_t *buf, uint32_t len) {
    int fd = *(int *)ctx;
    int n = net_recv(fd, buf, len);
    if (n <= 0)
        return -1;
    return n;
}

static void tls_rng(void *buf, uint32_t len) { rand_bytes(buf, len); }

static int64_t tls_now(void) { return (int64_t)rtc_unix_time(); }

struct tls_conn *tls_connect_tcp(uint32_t ip, uint16_t port,
                                 const char *hostname) {
    struct tls_config cfg;
    struct tls_conn *c;

    if (s_fd >= 0)
        return 0;
    if (!hostname || !ip)
        return 0;
    s_fd = net_socket(AF_INET, SOCK_STREAM, 0);
    if (s_fd < 0)
        return 0;
    if (net_connect(s_fd, ip, port) < 0) {
        net_close(s_fd);
        s_fd = -1;
        return 0;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.tr.send = tls_tr_send;
    cfg.tr.recv = tls_tr_recv;
    cfg.tr.ctx = &s_fd;
    cfg.rng = tls_rng;
    cfg.now_unix = tls_now;
    cfg.hostname = hostname;
    c = tls_connect(&cfg);
    if (!c) {
        net_close(s_fd);
        s_fd = -1;
        return 0;
    }
    return c;
}

int tls_write_tcp(struct tls_conn *c, const void *buf, uint32_t len) {
    return tls_write(c, buf, len);
}

int tls_read_tcp(struct tls_conn *c, void *buf, uint32_t len) {
    return tls_read(c, buf, len);
}

int tls_close_tcp(struct tls_conn *c) {
    int r = tls_close(c);
    if (s_fd >= 0) {
        net_close(s_fd);
        s_fd = -1;
    }
    return r;
}