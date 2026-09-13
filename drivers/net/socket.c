#include "drivers/net/socket.h"

#include "kernel/asm_func.h"
#include "lib/str/str.h"
#include "kernel/sched/thread.h"
#include "drivers/net/net.h"
#include "drivers/net/tcp.h"
#include "drivers/net/udp.h"
#include "kernel/init/pit/pit.h"

enum SOCK_WAIT {
    SWAIT_NONE,
    SWAIT_CONNECT,
    SWAIT_RECV,
    SWAIT_SEND,
    SWAIT_ACCEPT,
};

struct SOCKET {
    uint8_t active;
    uint8_t type;
    uint8_t wtype;
    uint8_t nonblock;
    uint8_t sel_rd;
    uint8_t sel_wr;
    struct task_struct *waiter;
    struct TCP_PCB *pcb;
    struct UDP_PCB *upcb;
    uint32_t err;
};

static struct SOCKET s_sock[MAX_SOCKET];

#define SOCK_CONNECT_TMO 10000

void sock_init(void) {
    memset(s_sock, 0, sizeof s_sock);
}

static struct SOCKET *sock_alloc(void) {
    for (int i = 0; i < MAX_SOCKET; i++) {
        if (!s_sock[i].active) {
            s_sock[i].active = 1;
            return &s_sock[i];
        }
    }
    return 0;
}

static struct SOCKET *sock_get(int fd) {
    if (fd < 0 || fd >= MAX_SOCKET || !s_sock[fd].active) {
        return NULL;
    }
    return &s_sock[fd];
}

static int sock_readable(struct SOCKET *s) {
    if (s->type == SOCK_DGRAM)
        return udp_rx_ready(s->upcb);
    struct TCP_PCB *p = s->pcb;
    return (p->rx_tail - p->rx_head) > 0 || p->fin_rcvd || !p->active;
}

static int sock_writable(struct SOCKET *s) {
    if (s->type == SOCK_DGRAM)
        return 1;
    struct TCP_PCB *p = s->pcb;
    if (!p->active)
        return 1;
    if (p->state != TCP_ESTABLISHED && p->state != TCP_CLOSE_WAIT)
        return 0;
    return (TCP_SND_BUF - p->tx_len) > 0;
}

static int sock_ready(struct SOCKET *s) {
    if (s->type == SOCK_DGRAM)
        return udp_rx_ready(s->upcb);
    struct TCP_PCB *p = s->pcb;
    if (s->wtype == SWAIT_CONNECT)
        return p->state == TCP_ESTABLISHED || !p->active;
    if (s->wtype == SWAIT_RECV)
        return (p->rx_tail - p->rx_head) > 0 || p->fin_rcvd || !p->active;
    if (s->wtype == SWAIT_SEND)
        return TCP_SND_BUF - p->tx_len > 0 || !p->active;
    if (s->wtype == SWAIT_ACCEPT)
        return tcp_accept_ready(p);
    return 0;
}

static int sock_block(struct SOCKET *s, uint8_t wtype, uint32_t timeout_ms) {
    s->wtype = wtype;
    if (s->nonblock)
        return sock_ready(s) ? 0 : -EAGAIN;
    uint32_t deadline = timeout_ms ? net_now_ms() + timeout_ms : 0;
    for (;;) {
        if (sock_ready(s))
            return 0;
        if (timeout_ms && (int32_t)(net_now_ms() - deadline) >= 0)
            return -1;
        s->waiter = current;
        thread_block_with_status(TASK_BLOCKED);
        s->waiter = 0;
    }
}

void sock_poll(void) {
    asm_cli();
    for (int i = 0; i < MAX_SOCKET; i++) {
        struct SOCKET *s = &s_sock[i];
        if (!s->active || !s->waiter)
            continue;
        if (s->sel_rd || s->sel_wr) {
            if (!((s->sel_rd && sock_readable(s)) ||
                  (s->sel_wr && sock_writable(s))))
                continue;
        } else if (!sock_ready(s)) {
            continue;
        }
        struct task_struct *w = s->waiter;
        s->waiter = 0;
        thread_unblock(w);
    }
    asm_sti();
}

int net_socket(int domain, int type, int proto) {
    if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM))
        return -1;
    (void)proto;
    struct SOCKET *s = sock_alloc();
    if (!s)
        return -1;
    s->type = (uint8_t)type;
    if (type == SOCK_DGRAM) {
        s->upcb = udp_pcb_alloc();
        if (!s->upcb) {
            s->active = 0;
            return -1;
        }
        return (int)(s - s_sock);
    }
    s->pcb = tcp_pcb_alloc();
    if (!s->pcb) {
        s->active = 0;
        return -1;
    }
    s->pcb->local_port = 0;
    return (int)(s - s_sock);
}

int net_bind(int fd, uint32_t ip, uint16_t port) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM)
        return udp_bind(s->upcb, ip, port);
    return tcp_bind(s->pcb, ip, port);
}

int net_listen(int fd, int backlog) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL)
        return -1;
    (void)backlog;
    return tcp_listen(s->pcb);
}

int net_connect(int fd, uint32_t ip, uint16_t port) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM) {
        udp_connect(s->upcb, ip, port);
        return 0;
    }
    if (tcp_connect(s->pcb, ip, port) < 0)
        return -1;
    if (s->nonblock)
        return -EINPROGRESS;
    if (sock_block(s, SWAIT_CONNECT, SOCK_CONNECT_TMO))
        return -1;
    return (s->pcb->active && s->pcb->state == TCP_ESTABLISHED) ? 0 : -1;
}

int net_send(int fd, const void *buf, uint32_t len) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM) {
        extern NETIF g_netif;
        if (!s->upcb->remote_ip)
            return -1;
        return udp_sendto(&g_netif, s->upcb, buf, len, s->upcb->remote_ip,
                          s->upcb->remote_port);
    }
    if (sock_block(s, SWAIT_SEND, SOCK_CONNECT_TMO))
        return -1;
    return tcp_send(s->pcb, buf, len);
}

int net_recv(int fd, void *buf, uint32_t len) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM) {
        if (sock_block(s, SWAIT_RECV, 0))
            return -1;
        return udp_recv(s->upcb, buf, len, 0, 0);
    }
    if (sock_block(s, SWAIT_RECV, 0))
        return -1;
    int n = tcp_recv(s->pcb, buf, len);
    if (n < 0 && (s->pcb->fin_rcvd || !s->pcb->active))
        return 0;
    return n;
}

int net_sendto(int fd, const void *buf, uint32_t len, uint32_t daddr,
               uint16_t dport) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL || s->type != SOCK_DGRAM)
        return -1;
    extern NETIF g_netif;
    return udp_sendto(&g_netif, s->upcb, buf, len, daddr, dport);
}

int net_recvfrom(int fd, void *buf, uint32_t len, uint32_t *saddr,
                 uint16_t *sport) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL || s->type != SOCK_DGRAM)
        return -1;
    if (sock_block(s, SWAIT_RECV, 0))
        return -1;
    return udp_recv(s->upcb, buf, len, saddr, sport);
}

int net_accept(int fd) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL || s->type != SOCK_STREAM || s->pcb->state != TCP_LISTEN)
        return -1;
    if (sock_block(s, SWAIT_ACCEPT, 0))
        return -1;
    struct TCP_PCB *np = tcp_accept(s->pcb);
    if (!np)
        return -1;
    struct SOCKET *n = sock_alloc();
    if (!n)
        return -1;
    n->type = SOCK_STREAM;
    n->pcb = np;
    return (int)(n - s_sock);
}

int net_close(int fd) {
    struct SOCKET *s = sock_get(fd);
    if (s == NULL)
        return -1;
    if (s->type == SOCK_STREAM) {
        tcp_close(s->pcb);
        tcp_pcb_free(s->pcb);
    } else {
        udp_pcb_free(s->upcb);
    }
    s->active = 0;
    s->waiter = 0;
    s->pcb = 0;
    s->upcb = 0;
    return 0;
}

#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

int net_is_socket(int fd) {
    return fd >= 0 && fd < MAX_SOCKET && s_sock[fd].active;
}

int net_fcntl(int fd, int cmd, uint32_t arg) {
    if (!net_is_socket(fd))
        return -EBADF;
    struct SOCKET *s = &s_sock[fd];
    switch (cmd) {
    case F_GETFL:
        return s->nonblock ? O_NONBLOCK : 0;
    case F_SETFL:
        s->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
        return 0;
    case F_GETFD:
    case F_SETFD:
        return 0;
    default:
        return -EINVAL;
    }
}

int net_getsockname(int fd, uint32_t *ip, uint16_t *port) {
    if (!net_is_socket(fd))
        return -EBADF;
    struct SOCKET *s = &s_sock[fd];
    if (s->type == SOCK_DGRAM) {
        if (ip)
            *ip = s->upcb->local_ip;
        if (port)
            *port = s->upcb->local_port;
    } else {
        if (ip)
            *ip = s->pcb->local_ip;
        if (port)
            *port = s->pcb->local_port;
    }
    return 0;
}

int net_getpeername(int fd, uint32_t *ip, uint16_t *port) {
    if (!net_is_socket(fd))
        return -EBADF;
    struct SOCKET *s = &s_sock[fd];
    if (s->type == SOCK_DGRAM) {
        if (!s->upcb->remote_ip)
            return -ENOTCONN;
        if (ip)
            *ip = s->upcb->remote_ip;
        if (port)
            *port = s->upcb->remote_port;
    } else {
        if (!s->pcb->remote_ip)
            return -ENOTCONN;
        if (ip)
            *ip = s->pcb->remote_ip;
        if (port)
            *port = s->pcb->remote_port;
    }
    return 0;
}

int net_getsockopt(int fd, int level, int optname, void *val, uint32_t *len) {
    if (!net_is_socket(fd))
        return -EBADF;
    if (level != SOL_SOCKET)
        return -EINVAL;
    struct SOCKET *s = &s_sock[fd];
    switch (optname) {
    case SO_ERROR: {
        uint32_t e = s->err;
        s->err = 0;
        if (val && len && *len >= sizeof(int32_t))
            *(int32_t *)val = (int32_t)e;
        return 0;
    }
    case SO_RCVBUF:
        if (val && len && *len >= sizeof(int32_t))
            *(int32_t *)val = (int32_t)TCP_RCV_BUF;
        return 0;
    case SO_SNDBUF:
        if (val && len && *len >= sizeof(int32_t))
            *(int32_t *)val = (int32_t)TCP_SND_BUF;
        return 0;
    default:
        return 0;
    }
}

int net_setsockopt(int fd, int level, int optname, const void *val,
                   uint32_t len) {
    (void)val;
    (void)len;
    if (!net_is_socket(fd))
        return -EBADF;
    if (level != SOL_SOCKET)
        return -EINVAL;
    switch (optname) {
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
        return 0;
    case SO_RCVBUF:
    case SO_SNDBUF:
        return 0;
    default:
        return -EINVAL;
    }
}

int net_shutdown(int fd, int how) {
    if (!net_is_socket(fd))
        return -EBADF;
    struct SOCKET *s = &s_sock[fd];
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    return tcp_shutdown(s->pcb, how);
}

int net_select(int nfds, uint32_t *rfds, uint32_t *wfds, uint32_t *efds,
               int timeout_ms) {
    int limit = nfds < MAX_SOCKET ? nfds : MAX_SOCKET;
    uint32_t deadline = timeout_ms > 0 ? net_now_ms() + (uint32_t)timeout_ms : 0;
    for (;;) {
        for (int fd = 0; fd < limit; fd++) {
            struct SOCKET *s = &s_sock[fd];
            if (!s->active)
                continue;
            if (rfds && ((rfds[fd / 32] >> (fd % 32)) & 1u)) {
                if (sock_readable(s))
                    rfds[fd / 32] |= (1u << (fd % 32));
                else
                    rfds[fd / 32] &= ~(1u << (fd % 32));
            }
            if (wfds && ((wfds[fd / 32] >> (fd % 32)) & 1u)) {
                if (sock_writable(s))
                    wfds[fd / 32] |= (1u << (fd % 32));
                else
                    wfds[fd / 32] &= ~(1u << (fd % 32));
            }
        }
        int total = 0;
        for (int fd = 0; fd < limit; fd++) {
            struct SOCKET *s = &s_sock[fd];
            if (!s->active)
                continue;
            if (rfds && ((rfds[fd / 32] >> (fd % 32)) & 1u))
                total++;
            if (wfds && ((wfds[fd / 32] >> (fd % 32)) & 1u))
                total++;
            if (efds && ((efds[fd / 32] >> (fd % 32)) & 1u))
                total++;
        }
        if (total)
            return total;
        if (timeout_ms >= 0 && (int32_t)(net_now_ms() - deadline) >= 0)
            return 0;
        mtime_sleep(1);
    }
}
