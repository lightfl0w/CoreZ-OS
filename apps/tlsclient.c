#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "lib/str/str.h"
#include "syscall.h"

#define TEST_HOST "www.rust-lang.org"

static void fmt_ip(uint32_t ip, char *out) {
    sprintf(out, "%d.%d.%d.%d", (int)((ip >> 24) & 0xFF),
            (int)((ip >> 16) & 0xFF), (int)((ip >> 8) & 0xFF), (int)(ip & 0xFF));
}

int main(void) {
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n";
    char ipstr[20];
    char err[128];
    uint32_t ip = 0;
    uint8_t buf[1024];
    int total = 0;
    int r;

    if (!dns_lookup(TEST_HOST, &ip)) {
        printf("tlsclient: dns failed for %s\n", TEST_HOST);
        return 1;
    }
    fmt_ip(ip, ipstr);
    printf("tlsclient: %s -> %s\n", TEST_HOST, ipstr);

    if (tls_sys_connect(ip, 443, TEST_HOST) != 0) {
        tls_sys_error(err, sizeof err);
        printf("tlsclient: handshake failed: %s\n", err);
        return 1;
    }
    printf("tlsclient: handshake ok\n");

    if (tls_sys_send(req, sizeof req - 1) != (int32_t)(sizeof req - 1)) {
        printf("tlsclient: send failed\n");
        tls_sys_close();
        return 1;
    }
    for (;;) {
        r = tls_sys_recv(buf, sizeof buf);
        if (r <= 0)
            break;
        total += r;
        if (total > 65536)
            break;
    }
    printf("tlsclient: read %d bytes\n", total);
    tls_sys_close();
    if (total > 0) {
        printf("TLSCLIENT_PASS\n");
        return 0;
    }
    printf("tlsclient: empty response\n");
    return 1;
}