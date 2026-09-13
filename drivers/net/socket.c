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
    struct TASK *waiter;
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
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_SOCKET; i++) {
        if (!s_sock[i].active) {
            s_sock[i].active = 1;
            lock_release(&net_lock);
            return &s_sock[i];
        }
    }
    lock_release(&net_lock);
    return 0;
}

static struct SOCKET *sock_get(int fd) {
    if (fd < 0 || fd >= MAX_SOCKET || !s_sock[fd].active) {
        return NULL;
    }
    return &s_sock[fd];
}

static int sock_readable(struct SOCKET *s) {
    lock_acquire(&net_lock);
    int r;
    if (s->type == SOCK_DGRAM) {
        r = udp_rx_ready(s->upcb);
    } else {
        struct TCP_PCB *p = s->pcb;
        r = (p->rx_tail - p->rx_head) > 0 || p->fin_rcvd || !p->active;
    }
    lock_release(&net_lock);
    return r;
}

static int sock_writable(struct SOCKET *s) {
    lock_acquire(&net_lock);
    int r;
    if (s->type == SOCK_DGRAM) {
        r = 1;
    } else {
        struct TCP_PCB *p = s->pcb;
        if (!p->active)
            r = 1;
        else if (p->state != TCP_ESTABLISHED && p->state != TCP_CLOSE_WAIT)
            r = 0;
        else
            r = (TCP_SND_BUF - p->tx_len) > 0;
    }
    lock_release(&net_lock);
    return r;
}

static int sock_ready(struct SOCKET *s) {
    lock_acquire(&net_lock);
    int r = 0;
    if (s->type == SOCK_DGRAM) {
        r = udp_rx_ready(s->upcb);
    } else {
        struct TCP_PCB *p = s->pcb;
        if (s->wtype == SWAIT_CONNECT)
            r = p->state == TCP_ESTABLISHED || !p->active;
        else if (s->wtype == SWAIT_RECV)
            r = (p->rx_tail - p->rx_head) > 0 || p->fin_rcvd || !p->active;
        else if (s->wtype == SWAIT_SEND)
            r = TCP_SND_BUF - p->tx_len > 0 || !p->active;
        else if (s->wtype == SWAIT_ACCEPT)
            r = tcp_accept_ready(p);
    }
    lock_release(&net_lock);
    return r;
}

static int sock_block(struct SOCKET *s, uint8_t wtype, uint32_t timeout_ms) {
    lock_acquire(&net_lock);
    s->wtype = wtype;
    int nonblock = s->nonblock;
    lock_release(&net_lock);
    if (nonblock)
        return sock_ready(s) ? 0 : -EAGAIN;
    uint32_t deadline = timeout_ms ? net_now_ms() + timeout_ms : 0;
    for (;;) {
        if (sock_ready(s))
            return 0;
        if (timeout_ms && (int32_t)(net_now_ms() - deadline) >= 0)
            return -1;

        lock_acquire(&net_lock);
        s->waiter = current;
        lock_release(&net_lock);
        thread_block_with_status(TASK_BLOCKED);
        lock_acquire(&net_lock);
        s->waiter = 0;
        lock_release(&net_lock);
    }
}

void sock_poll(void) {
    lock_acquire(&net_lock);
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
        struct TASK *w = s->waiter;
        s->waiter = 0;
        thread_unblock(w);
    }
    lock_release(&net_lock);
}

int net_socket(int domain, int type, int proto) {
    if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM))
        return -1;
    (void)proto;
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_alloc();
    if (!s) {
        lock_release(&net_lock);
        return -1;
    }
    s->type = (uint8_t)type;
    if (type == SOCK_DGRAM) {
        s->upcb = udp_pcb_alloc();
        if (!s->upcb) {
            s->active = 0;
            lock_release(&net_lock);
            return -1;
        }
        lock_release(&net_lock);
        return (int)(s - s_sock);
    }
    s->pcb = tcp_pcb_alloc();
    if (!s->pcb) {
        s->active = 0;
        lock_release(&net_lock);
        return -1;
    }
    s->pcb->local_port = 0;
    lock_release(&net_lock);
    return (int)(s - s_sock);
}

int net_bind(int fd, uint32_t ip, uint16_t port) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL) {
        lock_release(&net_lock);
        return -1;
    }
    int rc = (s->type == SOCK_DGRAM) ? udp_bind(s->upcb, ip, port)
                                     : tcp_bind(s->pcb, ip, port);
    lock_release(&net_lock);
    return rc;
}

int net_listen(int fd, int backlog) {
    (void)backlog;
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL) {
        lock_release(&net_lock);
        return -1;
    }
    int rc = tcp_listen(s->pcb);
    lock_release(&net_lock);
    return rc;
}

int net_connect(int fd, uint32_t ip, uint16_t port) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL) {
        lock_release(&net_lock);
        return -1;
    }
    if (s->type == SOCK_DGRAM) {
        udp_connect(s->upcb, ip, port);
        lock_release(&net_lock);
        return 0;
    }
    int rc = tcp_connect(s->pcb, ip, port);
    int nonblock = s->nonblock;
    lock_release(&net_lock);
    if (rc < 0)
        return -1;
    if (nonblock)
        return -EINPROGRESS;

    if (sock_block(s, SWAIT_CONNECT, SOCK_CONNECT_TMO))
        return -1;
    lock_acquire(&net_lock);
    int ok = s->pcb->active && s->pcb->state == TCP_ESTABLISHED;
    lock_release(&net_lock);
    return ok ? 0 : -1;
}

int net_send(int fd, const void *buf, uint32_t len) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL) {
        lock_release(&net_lock);
        return -1;
    }
    if (s->type == SOCK_DGRAM) {
        extern NETIF g_netif;
        if (!s->upcb->remote_ip) {
            lock_release(&net_lock);
            return -1;
        }
        int rc = udp_sendto(&g_netif, s->upcb, buf, len, s->upcb->remote_ip,
                            s->upcb->remote_port);
        lock_release(&net_lock);
        return rc;
    }
    lock_release(&net_lock);

    if (sock_block(s, SWAIT_SEND, SOCK_CONNECT_TMO))
        return -1;
    return tcp_send(s->pcb, buf, len);
}

int net_recv(int fd, void *buf, uint32_t len) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL) {
        lock_release(&net_lock);
        return -1;
    }
    int is_dgram = (s->type == SOCK_DGRAM);
    lock_release(&net_lock);

    if (is_dgram) {
        if (sock_block(s, SWAIT_RECV, 0))
            return -1;
        return udp_recv(s->upcb, buf, len, 0, 0);
    }
    if (sock_block(s, SWAIT_RECV, 0))
        return -1;
    int n = tcp_recv(s->pcb, buf, len);
    if (n < 0) {
        lock_acquire(&net_lock);
        int closed = s->pcb->fin_rcvd || !s->pcb->active;
        lock_release(&net_lock);
        if (closed)
            return 0;
    }
    return n;
}

int net_sendto(int fd, const void *buf, uint32_t len, uint32_t daddr,
               uint16_t dport) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL || s->type != SOCK_DGRAM) {
        lock_release(&net_lock);
        return -1;
    }
    extern NETIF g_netif;
    int rc = udp_sendto(&g_netif, s->upcb, buf, len, daddr, dport);
    lock_release(&net_lock);
    return rc;
}

int net_recvfrom(int fd, void *buf, uint32_t len, uint32_t *saddr,
                 uint16_t *sport) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL || s->type != SOCK_DGRAM) {
        lock_release(&net_lock);
        return -1;
    }
    lock_release(&net_lock);
    if (sock_block(s, SWAIT_RECV, 0))
        return -1;
    return udp_recv(s->upcb, buf, len, saddr, sport);
}

int net_accept(int fd) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL || s->type != SOCK_STREAM || s->pcb->state != TCP_LISTEN) {
        lock_release(&net_lock);
        return -1;
    }
    lock_release(&net_lock);
    if (sock_block(s, SWAIT_ACCEPT, 0))
        return -1;
    struct TCP_PCB *np = tcp_accept(s->pcb);
    if (!np)
        return -1;
    lock_acquire(&net_lock);
    struct SOCKET *n = sock_alloc();
    if (!n) {
        lock_release(&net_lock);
        return -1;
    }
    n->type = SOCK_STREAM;
    n->pcb = np;
    lock_release(&net_lock);
    return (int)(n - s_sock);
}

int net_close(int fd) {
    lock_acquire(&net_lock);
    struct SOCKET *s = sock_get(fd);
    if (s == NULL) {
        lock_release(&net_lock);
        return -1;
    }
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
    lock_release(&net_lock);
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
    lock_acquire(&net_lock);
    struct SOCKET *s = &s_sock[fd];
    int rc;
    switch (cmd) {
    case F_GETFL:
        rc = s->nonblock ? O_NONBLOCK : 0;
        break;
    case F_SETFL:
        s->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
        rc = 0;
        break;
    case F_GETFD:
    case F_SETFD:
        rc = 0;
        break;
    default:
        rc = -EINVAL;
        break;
    }
    lock_release(&net_lock);
    return rc;
}

int net_getsockname(int fd, uint32_t *ip, uint16_t *port) {
    if (!net_is_socket(fd))
        return -EBADF;
    lock_acquire(&net_lock);
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
    lock_release(&net_lock);
    return 0;
}

int net_getpeername(int fd, uint32_t *ip, uint16_t *port) {
    if (!net_is_socket(fd))
        return -EBADF;
    lock_acquire(&net_lock);
    struct SOCKET *s = &s_sock[fd];
    int rc = 0;
    if (s->type == SOCK_DGRAM) {
        if (!s->upcb->remote_ip)
            rc = -ENOTCONN;
        else {
            if (ip)
                *ip = s->upcb->remote_ip;
            if (port)
                *port = s->upcb->remote_port;
        }
    } else {
        if (!s->pcb->remote_ip)
            rc = -ENOTCONN;
        else {
            if (ip)
                *ip = s->pcb->remote_ip;
            if (port)
                *port = s->pcb->remote_port;
        }
    }
    lock_release(&net_lock);
    return rc;
}

int net_getsockopt(int fd, int level, int optname, void *val, uint32_t *len) {
    if (!net_is_socket(fd))
        return -EBADF;
    if (level != SOL_SOCKET)
        return -EINVAL;
    struct SOCKET *s = &s_sock[fd];
    switch (optname) {
    case SO_ERROR: {
        lock_acquire(&net_lock);
        uint32_t e = s->err;
        s->err = 0;
        lock_release(&net_lock);
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
    lock_acquire(&net_lock);
    struct SOCKET *s = &s_sock[fd];
    int rc;
    if (s->type != SOCK_STREAM)
        rc = -EOPNOTSUPP;
    else
        rc = tcp_shutdown(s->pcb, how);
    lock_release(&net_lock);
    return rc;
}

int net_select(int nfds, uint32_t *rfds, uint32_t *wfds, uint32_t *efds,
               int timeout_ms) {
    int limit = nfds < MAX_SOCKET ? nfds : MAX_SOCKET;
    uint32_t deadline = timeout_ms > 0 ? net_now_ms() + (uint32_t)timeout_ms : 0;
    for (;;) {
        lock_acquire(&net_lock);
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
        lock_release(&net_lock);
        if (total)
            return total;

        if (timeout_ms >= 0 && (int32_t)(net_now_ms() - deadline) >= 0)
            return 0;
        mtime_sleep(1);
    }
}
