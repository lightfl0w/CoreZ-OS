#include "drivers/net/ip.h"

#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "drivers/net/arp.h"
#include "drivers/net/eth.h"
#include "drivers/net/icmp.h"
#include "drivers/net/net.h"
#include "drivers/net/tcp.h"
#include "drivers/net/udp.h"

#define IP_PEND_MAX 4

struct IP_PEND {
    uint32_t dst;
    uint32_t len;
    int active;
    uint8_t frame[ETH_FRAME_MAX];
};

static struct IP_PEND s_pend[IP_PEND_MAX];

uint16_t ip_csum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    uint32_t n = len;
    while (n > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (uint16_t)p[0] << 8;
    while (sum >> 16)
        sum = (uint16_t)sum + (sum >> 16);
    return (uint16_t)~sum;
}

void ip_input(NETIF *ifp, const uint8_t *pkt, uint32_t len) {
    if (len < IP_HDR_LEN)
        return;
    uint8_t ver = (uint8_t)(pkt[0] >> 4);
    uint8_t ihlen = (uint8_t)((pkt[0] & 0x0F) * 4);
    if (ver != 4 || ihlen < IP_HDR_LEN || len < ihlen) {
        nt_raw("ipbad", net_be32(pkt + 12), 0);
        return;
    }
    if (ip_csum(pkt, ihlen) != 0) {
        nt_raw("ipcsum", net_be32(pkt + 12), 0);
        return;
    }

    uint32_t total = net_be16(pkt + 2);
    if (total >= ihlen && total <= len)
        len = total;
    uint32_t daddr = net_be32(pkt + 16);
    if (daddr != ifp->ip && daddr != 0xFFFFFFFFu) {
        nt_raw("ipdst", net_be32(pkt + 12), daddr);
        return;
    }
    if (net_be16(pkt + 6) & 0x3FFF)
        nt_raw("ipfrag", net_be32(pkt + 12), net_be16(pkt + 6));
    uint8_t proto = pkt[9];
    uint32_t plen = len - ihlen;
    if (proto == IPPROTO_ICMP)
        icmp_input(ifp, net_be32(pkt + 12), pkt + ihlen, plen);
    else if (proto == IPPROTO_TCP)
        tcp_input(ifp, net_be32(pkt + 12), pkt + ihlen, plen);
    else if (proto == IPPROTO_UDP)
        udp_input(ifp, net_be32(pkt + 12), pkt + ihlen, plen);
}

static struct IP_PEND *ip_pend_slot(uint32_t nh) {
    for (int i = 0; i < IP_PEND_MAX; i++)
        if (!s_pend[i].active)
            return &s_pend[i];
    for (int i = 0; i < IP_PEND_MAX; i++)
        if (s_pend[i].dst == nh)
            return &s_pend[i];
    return 0;
}

int ip_output(NETIF *ifp, uint32_t daddr, uint8_t proto, const void *data,
              uint32_t len) {
    uint8_t pkt[IP_HDR_LEN + TCP_MSS];
    uint32_t tot = IP_HDR_LEN + len;
    if (tot > sizeof pkt)
        return -1;
    pkt[0] = 0x45;
    pkt[1] = 0;
    net_put16(pkt + 2, (uint16_t)tot);
    net_put16(pkt + 4, 0);
    net_put16(pkt + 6, 0);
    pkt[8] = 64;
    pkt[9] = proto;
    net_put16(pkt + 10, 0);
    net_put32(pkt + 12, ifp->ip);
    net_put32(pkt + 16, daddr);
    net_put16(pkt + 10, ip_csum(pkt, IP_HDR_LEN));
    memcpy(pkt + IP_HDR_LEN, data, len);

    uint32_t nh =
        ((daddr & ifp->mask) == (ifp->ip & ifp->mask)) ? daddr : ifp->gw;
    uint8_t mac[6];
    if (arp_resolve(ifp, nh, mac) == 0)
        return eth_output(ifp, mac, ETH_IP, pkt, tot);

    lock_acquire(&net_lock);
    struct IP_PEND *pend = ip_pend_slot(nh);
    if (pend) {
        pend->dst = nh;
        pend->len = tot;
        memcpy(pend->frame, pkt, tot);
        pend->active = 1;
    }
    lock_release(&net_lock);
    return 0;
}

void ip_arp_resolved(NETIF *ifp, uint32_t ip, const uint8_t *mac) {
    lock_acquire(&net_lock);
    for (int i = 0; i < IP_PEND_MAX; i++) {
        if (!s_pend[i].active || s_pend[i].dst != ip)
            continue;
        s_pend[i].active = 0;
        eth_output(ifp, mac, ETH_IP, s_pend[i].frame, s_pend[i].len);
    }
    lock_release(&net_lock);
}
