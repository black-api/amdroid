/* net.c — Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP, DNS, and TCP (client and
 * server). Polled: callers drive the stack by calling net_poll() while they
 * wait. The address comes from DHCP; 10.0.2.15 is the fallback (QEMU's user net). */
#include "amdroid/net.h"
#include "amdroid/nic.h"
#include "amdroid/timer.h"
#include "amdroid/console.h"
#include "amdroid/sched.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include <string.h>

#define ETH_ARP  0x0806
#define ETH_IP   0x0800
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

static nic_t *nic;
static int in_rx;        /* set while a received frame is being handled */
static u8 my_mac[6];
static u32 my_ip = 0, gw_ip = 0, netmask = IP(255,255,255,0), dns_ip = 0;

/* ---------- byte order ---------- */
static u16 be16(const u8 *p){ return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p){ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put16(u8 *p, u16 v){ p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v){ p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static u16 checksum(const void *data, u32 len, u32 seed){
    const u8 *p = data;
    u32 sum = seed;
    for(u32 i = 0; i + 1 < len; i += 2) sum += (u32)((p[i] << 8) | p[i + 1]);
    if(len & 1) sum += (u32)(p[len - 1] << 8);
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}
static u32 pseudo_sum(u32 src, u32 dst, u8 proto, u16 len){
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

/* ---------- ARP cache ---------- */
typedef struct { u32 ip; u8 mac[6]; int valid; } arp_t;
static arp_t arp[8];
static int arp_next;

static int arp_lookup(u32 ip, u8 mac[6]){
    for(int i = 0; i < 8; i++) if(arp[i].valid && arp[i].ip == ip){ memcpy(mac, arp[i].mac, 6); return 1; }
    return 0;
}
static void arp_store(u32 ip, const u8 mac[6]){
    for(int i = 0; i < 8; i++) if(arp[i].valid && arp[i].ip == ip){ memcpy(arp[i].mac, mac, 6); return; }
    arp[arp_next].ip = ip; memcpy(arp[arp_next].mac, mac, 6); arp[arp_next].valid = 1;
    arp_next = (arp_next + 1) % 8;
}

/* ---------- sending ---------- */
static u8 txframe[1600];
static u16 ip_id = 1;
static const u8 bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static int send_frame(const u8 mac[6], u16 type, const u8 *payload, u32 len){
    if(len + 14 > sizeof txframe) return -1;
    memcpy(txframe, mac, 6);
    memcpy(txframe + 6, my_mac, 6);
    put16(txframe + 12, type);
    memcpy(txframe + 14, payload, len);
    return nic->send(txframe, len + 14);
}

static void send_arp(u16 op, const u8 tha[6], u32 tpa){
    u8 p[28];
    put16(p, 1); put16(p + 2, ETH_IP); p[4] = 6; p[5] = 4; put16(p + 6, op);
    memcpy(p + 8, my_mac, 6); put32(p + 14, my_ip);
    memcpy(p + 18, tha, 6); put32(p + 24, tpa);
    send_frame(op == 1 ? bcast_mac : tha, ETH_ARP, p, 28);
}

/* resolves the next hop for ip (the gateway if it is off-subnet) */
static int resolve(u32 ip, u8 mac[6]){
    u32 hop = ((ip & netmask) == (my_ip & netmask)) ? ip : gw_ip;
    if(arp_lookup(hop, mac)) return 0;
    for(int tries = 0; tries < 3; tries++){
        send_arp(1, bcast_mac, hop);
        u64 until = timer_ticks() + 100;
        while(timer_ticks() < until){
            net_poll();
            if(arp_lookup(hop, mac)) return 0;
            proc_yield();
        }
    }
    return -1;
}

static u16 send_ip(u32 dst, u8 proto, const u8 *payload, u32 len){
    u8 pkt[1500];
    if(len + 20 > sizeof pkt) return 0;
    u8 mac[6];
    if(in_rx){
        /* inside the receive path we must not wait: use the cache, or ask for
           the address and let the peer's retransmission try again */
        u32 hop = ((dst & netmask) == (my_ip & netmask)) ? dst : gw_ip;
        if(!arp_lookup(hop, mac)){ send_arp(1, bcast_mac, hop); return 0; }
    } else if(resolve(dst, mac) != 0) return 0;
    u16 id = ip_id++;
    pkt[0] = 0x45; pkt[1] = 0;
    put16(pkt + 2, (u16)(20 + len));
    put16(pkt + 4, id);
    put16(pkt + 6, 0x4000);             /* don't fragment */
    pkt[8] = 64; pkt[9] = proto;
    put16(pkt + 10, 0);
    put32(pkt + 12, my_ip);
    put32(pkt + 16, dst);
    put16(pkt + 10, checksum(pkt, 20, 0));
    memcpy(pkt + 20, payload, len);
    send_frame(mac, ETH_IP, pkt, 20 + len);
    return id;
}

/* a broadcast from 0.0.0.0 (used by DHCP before we have an address) */
static void send_bcast_udp(u16 sport, u16 dport, const u8 *data, u32 len){
    u8 seg[600];
    if(len + 8 > sizeof seg) return;
    put16(seg, sport); put16(seg + 2, dport); put16(seg + 4, (u16)(8 + len)); put16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    u8 pkt[700];
    pkt[0] = 0x45; pkt[1] = 0;
    put16(pkt + 2, (u16)(28 + len));
    put16(pkt + 4, ip_id++);
    put16(pkt + 6, 0);
    pkt[8] = 64; pkt[9] = IPPROTO_UDP;
    put16(pkt + 10, 0);
    put32(pkt + 12, 0);
    put32(pkt + 16, 0xFFFFFFFF);
    put16(pkt + 10, checksum(pkt, 20, 0));
    memcpy(pkt + 20, seg, 8 + len);
    send_frame(bcast_mac, ETH_IP, pkt, 20 + 8 + len);
}

/* ---------- ICMP echo ---------- */
static volatile int ping_got;
static volatile u32 ping_from;
static volatile u16 ping_seq_want;

static void handle_icmp(u32 src, const u8 *p, u32 len){
    if(len < 8) return;
    if(p[0] == 8){                                /* echo request: answer it */
        u8 r[1500];
        if(len > sizeof r) return;
        memcpy(r, p, len);
        r[0] = 0; r[2] = 0; r[3] = 0;
        put16(r + 2, checksum(r, len, 0));
        send_ip(src, IPPROTO_ICMP, r, len);
    } else if(p[0] == 0){                         /* echo reply */
        ping_got = 1;
        ping_from = src;
        ping_seq_want = be16(p + 6);
    }
}

/* ---------- UDP ---------- */
#define MAX_UDP 8
typedef struct { u16 port; void (*fn)(u32 src, u16 sport, const u8 *d, u32 n); } udp_h;
static udp_h udp_handlers[MAX_UDP];

int udp_bind(u16 port, void (*fn)(u32 src, u16 sport, const u8 *d, u32 n)){
    for(int i = 0; i < MAX_UDP; i++)
        if(!udp_handlers[i].fn){ udp_handlers[i].port = port; udp_handlers[i].fn = fn; return 0; }
    return -1;
}
void udp_unbind(u16 port){
    for(int i = 0; i < MAX_UDP; i++) if(udp_handlers[i].fn && udp_handlers[i].port == port) udp_handlers[i].fn = 0;
}

/* ---------- DHCP ---------- */
static u32 dhcp_xid;
static int dhcp_stage;                  /* 0 idle, 1 offer received, 2 address acknowledged */
static u32 offer_ip, offer_server, offer_mask, offer_router, offer_dns;

static void dhcp_input(const u8 *p, u32 n){
    if(n < 240 || p[0] != 2) return;                       /* BOOTREPLY only */
    if(be32(p + 4) != dhcp_xid) return;
    u32 yiaddr = be32(p + 16);
    if(be32(p + 236) != 0x63825363) return;                 /* magic cookie */
    const u8 *o = p + 240;
    u32 on = n - 240, mtype = 0, mask = 0, router = 0, dns = 0, server = 0;
    for(u32 i = 0; i < on;){
        u8 code = o[i];
        if(code == 255) break;
        if(code == 0){ i++; continue; }
        if(i + 1 >= on) break;
        u8 l = o[i + 1];
        if(i + 2 + l > on) break;
        const u8 *v = o + i + 2;
        switch(code){
            case 53: mtype = v[0]; break;
            case 1:  mask = be32(v); break;
            case 3:  router = be32(v); break;
            case 6:  dns = be32(v); break;
            case 54: server = be32(v); break;
        }
        i += 2 + l;
    }
    if(mtype == 2 && dhcp_stage == 0){
        offer_ip = yiaddr; offer_server = server; offer_mask = mask;
        offer_router = router; offer_dns = dns;
        dhcp_stage = 1;
    } else if(mtype == 5 && dhcp_stage == 1 && yiaddr == offer_ip){
        dhcp_stage = 2;
    }
}

static void dhcp_send(u8 type, u32 req_ip, u32 server){
    u8 pkt[300];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 1; pkt[1] = 1; pkt[2] = 6;
    put32(pkt + 4, dhcp_xid);
    put16(pkt + 10, 0x8000);                 /* ask for a broadcast reply */
    memcpy(pkt + 28, my_mac, 6);
    u8 *o = pkt + 236;
    put32(o, 0x63825363); o += 4;
    *o++ = 53; *o++ = 1; *o++ = type;
    if(req_ip){ *o++ = 50; *o++ = 4; put32(o, req_ip); o += 4; }
    if(server){ *o++ = 54; *o++ = 4; put32(o, server); o += 4; }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 255;
    send_bcast_udp(68, 67, pkt, (u32)(o - pkt));
}

static void dhcp_udp(u32 src, u16 sport, const u8 *d, u32 n){ (void)src; (void)sport; dhcp_input(d, n); }

static int dhcp_acquire(void){
    udp_bind(68, dhcp_udp);
    for(int attempt = 0; attempt < 3; attempt++){
        dhcp_xid = 0x5A5A0000u + (u32)timer_ticks();
        dhcp_stage = 0;
        dhcp_send(1, 0, 0);                              /* discover */
        u64 until = timer_ticks() + 200;
        while(timer_ticks() < until && dhcp_stage < 1){ net_poll(); proc_yield(); }
        if(dhcp_stage < 1) continue;
        dhcp_send(3, offer_ip, offer_server);            /* request the offered address */
        until = timer_ticks() + 200;
        while(timer_ticks() < until && dhcp_stage < 2){ net_poll(); proc_yield(); }
        if(dhcp_stage == 2){
            my_ip = offer_ip;
            netmask = offer_mask ? offer_mask : IP(255,255,255,0);
            gw_ip = offer_router;
            dns_ip = offer_dns;
            udp_unbind(68);
            return 0;
        }
    }
    udp_unbind(68);
    return -1;
}

/* ---------- DNS (A records) ---------- */
static u16 dns_id;
static volatile int dns_got;
static u32 dns_result;

static int dns_skip_name(const u8 *d, u32 n, u32 pos){
    while(pos < n){
        u8 l = d[pos];
        if(l == 0) return (int)pos + 1;
        if((l & 0xC0) == 0xC0) return (int)pos + 2;
        pos += (u32)l + 1;
    }
    return -1;
}

static void dns_udp(u32 src, u16 sport, const u8 *d, u32 n){
    (void)src; (void)sport;
    if(n < 12 || be16(d) != dns_id) return;
    u16 qd = be16(d + 4), an = be16(d + 6);
    int pos = 12;
    for(u16 i = 0; i < qd; i++){
        pos = dns_skip_name(d, n, (u32)pos);
        if(pos < 0 || (u32)pos + 4 > n) return;
        pos += 4;
    }
    for(u16 i = 0; i < an; i++){
        pos = dns_skip_name(d, n, (u32)pos);
        if(pos < 0 || (u32)pos + 10 > n) return;
        u16 type = be16(d + pos), rdlen = be16(d + pos + 8);
        pos += 10;
        if((u32)pos + rdlen > n) return;
        if(type == 1 && rdlen == 4){ dns_result = be32(d + pos); dns_got = 1; return; }
        pos += rdlen;
    }
}

static int dns_query(u32 server, u16 port, const char *name, u32 *ip){
    static u16 local = 40000;
    local++;
    u8 q[300];
    memset(q, 0, sizeof q);
    dns_id = (u16)(0x2000 + timer_ticks());
    put16(q, dns_id); put16(q + 2, 0x0100);      /* recursion desired */
    put16(q + 4, 1);
    int n = 12;
    const char *p = name;
    while(*p){
        const char *dot = p;
        while(*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if(len == 0 || len > 63 || n + len + 1 > (int)sizeof q - 6) return -1;
        q[n++] = (u8)len;
        for(int i = 0; i < len; i++) q[n++] = (u8)p[i];
        p = *dot ? dot + 1 : dot;
    }
    q[n++] = 0;
    put16(q + n, 1); put16(q + n + 2, 1);        /* A, IN */
    n += 4;
    if(udp_bind(local, dns_udp) != 0) return -1;
    int ok = -1;
    for(int tries = 0; tries < 2 && ok < 0; tries++){
        dns_got = 0;
        udp_send(server, port, local, q, (u32)n);
        u64 until = timer_ticks() + 200;
        while(timer_ticks() < until && !dns_got){ net_poll(); proc_yield(); }
        if(dns_got){ *ip = dns_result; ok = 0; }
    }
    udp_unbind(local);
    return ok;
}

/* name -> IPv4. Dotted addresses are returned as they are. */
int net_resolve(const char *name, u32 *ip, u32 server, u16 port){
    u32 parts[4]; int k = 0; u32 v = 0; int digits = 0; const char *p = name;
    for(;;){
        if(*p >= '0' && *p <= '9'){ v = v * 10 + (u32)(*p - '0'); digits++; }
        else if(*p == '.' || *p == 0){
            if(!digits || v > 255 || k > 3) break;
            parts[k++] = v; v = 0; digits = 0;
            if(!*p){ if(k == 4){ *ip = IP(parts[0], parts[1], parts[2], parts[3]); return 0; } break; }
        } else break;
        p++;
    }
    if(!server) server = dns_ip;
    if(!port) port = 53;
    if(!server) return -1;
    return dns_query(server, port, name, ip);
}

u32 net_dns(void){ return dns_ip; }

/* ---------- UDP receive ---------- */
static void handle_udp(u32 src, const u8 *p, u32 len){
    if(len < 8) return;
    u16 sport = be16(p), dport = be16(p + 2);
    u16 ulen = be16(p + 4);
    if(ulen < 8 || ulen > len) return;
    for(int i = 0; i < MAX_UDP; i++)
        if(udp_handlers[i].fn && udp_handlers[i].port == dport)
            udp_handlers[i].fn(src, sport, p + 8, ulen - 8);
}

int udp_send(u32 ip, u16 dport, u16 sport, const void *data, u32 len){
    u8 seg[1400];
    if(len + 8 > sizeof seg) return -1;
    put16(seg, sport); put16(seg + 2, dport); put16(seg + 4, (u16)(8 + len)); put16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    u16 c = checksum(seg, 8 + len, pseudo_sum(my_ip, ip, IPPROTO_UDP, (u16)(8 + len)));
    put16(seg + 6, c ? c : 0xFFFF);
    return send_ip(ip, IPPROTO_UDP, seg, 8 + len) ? 0 : -1;
}

/* ---------- TCP ---------- */
#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10
#define RXBUF 61440              /* receive buffer, and so the largest window we offer */
#define MAX_SOCK 8

enum { TS_FREE = 0, TS_SYN_SENT, TS_ESTABLISHED, TS_CLOSED_BY_PEER, TS_LISTEN, TS_SYN_RCVD };

#define MSS 1000
#define OOO_SLOTS 4

typedef struct {
    int used;
    u32 seq, len;
    u8 d[MSS];
} ooo_t;

typedef struct {
    int used;
    int state;
    int parent;                /* listening socket a connection came from, or -1 */
    int accepted;
    u32 ip; u16 lport, rport;
    u32 snd_nxt, rcv_nxt, last_ack_seen;
    u8 *rx; u32 rx_len;
    int peer_closed;
    /* flow and congestion control */
    u32 peer_wnd;             /* the window the peer advertised */
    u32 cwnd, ssthresh;       /* congestion window and slow-start threshold, in bytes */
    int dupacks;
    u32 adv_wnd;              /* the window we last told the peer about */
    ooo_t ooo[OOO_SLOTS];     /* segments that arrived out of order */
} tcb_t;

static tcb_t socks[MAX_SOCK];
static u16 next_lport = 49152;

static int tcp_send_seg(tcb_t *s, u8 flags, const u8 *data, u32 len);
static int tcp_seg(tcb_t *s, u32 seq, u8 flags, const u8 *data, u32 len);

/* Answers a segment that matches no connection with a reset (RFC 793), so the
 * peer gives up at once instead of waiting. */
static void tcp_reset(u32 dst, u16 lport, u16 rport, u32 seq_in, u32 ack_in, u8 flags_in){
    u8 seg[20];
    memset(seg, 0, sizeof seg);
    put16(seg, lport); put16(seg + 2, rport);
    if(flags_in & TF_ACK){ put32(seg + 4, ack_in); put32(seg + 8, 0); seg[13] = TF_RST; }
    else { put32(seg + 4, 0); put32(seg + 8, seq_in + ((flags_in & TF_SYN) ? 1 : 0)); seg[13] = TF_RST | TF_ACK; }
    seg[12] = 5 << 4;
    u16 c = checksum(seg, 20, pseudo_sum(my_ip, dst, IPPROTO_TCP, 20));
    put16(seg + 16, c);
    send_ip(dst, IPPROTO_TCP, seg, 20);
}

/* the segment handler: TCP for every socket we have */
static void tcp_handle(u32 src, const u8 *p, u32 len){
    if(len < 20) return;
    u16 sport = be16(p), dport = be16(p + 2);
    u32 seq = be32(p + 4), ack = be32(p + 8);
    u8 hl = (u8)((p[12] >> 4) * 4);
    u8 flags = p[13];
    if(hl < 20 || hl > len) return;
    const u8 *data = p + hl;
    u32 dlen = len - hl;

    int idx = -1;
    for(int i = 0; i < MAX_SOCK; i++)
        if(socks[i].used && socks[i].state != TS_LISTEN && socks[i].lport == dport &&
           socks[i].rport == sport && socks[i].ip == src){ idx = i; break; }
    if(idx < 0){
        /* a new connection to a listening port */
        if((flags & (TF_SYN | TF_ACK)) != TF_SYN){
            if(!(flags & TF_RST)) tcp_reset(src, dport, sport, seq, ack, flags);
            return;
        }
        int lis = -1, slot = -1;
        for(int i = 0; i < MAX_SOCK; i++){
            if(socks[i].used && socks[i].state == TS_LISTEN && socks[i].lport == dport) lis = i;
            if(!socks[i].used && slot < 0) slot = i;
        }
        if(lis < 0 || slot < 0){
            if(!(flags & TF_RST)) tcp_reset(src, dport, sport, seq, ack, flags);
            return;
        }
        tcb_t *c = &socks[slot];
        memset(c, 0, sizeof *c);
        c->used = 1; c->state = TS_SYN_RCVD; c->parent = lis;
        c->ip = src; c->lport = dport; c->rport = sport;
        c->snd_nxt = 0x3000u + (u32)timer_ticks();
        c->rcv_nxt = seq + 1;
        c->rx = kmalloc(RXBUF);
        if(!c->rx){ c->used = 0; return; }
        tcp_send_seg(c, TF_SYN | TF_ACK, 0, 0);
        c->snd_nxt++;                            /* the SYN consumes one number */
        return;
    }
    tcb_t *s = &socks[idx];
    if(flags & TF_RST){ s->state = TS_CLOSED_BY_PEER; s->peer_closed = 1; return; }
    if(s->state == TS_SYN_SENT){
        /* the SYN-ACK acknowledges snd_nxt + 1 (the SYN) */
        if((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK) && ack == s->snd_nxt + 1){
            s->rcv_nxt = seq + 1;
            s->state = TS_ESTABLISHED;
            s->last_ack_seen = ack;
        }
        return;
    }
    if(s->state == TS_SYN_RCVD){
        if((flags & TF_ACK) && ack == s->snd_nxt) s->state = TS_ESTABLISHED;
        else return;
    }
    s->peer_wnd = be16(p + 14);
    if(flags & TF_ACK){
        if(ack == s->last_ack_seen && dlen == 0 && !(flags & (TF_SYN | TF_FIN))) s->dupacks++;
        else if((i32)(ack - s->last_ack_seen) > 0){ s->last_ack_seen = ack; s->dupacks = 0; }
    }
    if(dlen){
        if(seq == s->rcv_nxt){
            if(s->rx_len + dlen <= RXBUF){
                memcpy(s->rx + s->rx_len, data, dlen);
                s->rx_len += dlen;
                s->rcv_nxt += dlen;
            }
            /* segments that were waiting behind this one may now be in order */
            int moved = 1;
            while(moved){
                moved = 0;
                for(int k = 0; k < OOO_SLOTS; k++){
                    ooo_t *o = &s->ooo[k];
                    if(o->used && o->seq == s->rcv_nxt && s->rx_len + o->len <= RXBUF){
                        memcpy(s->rx + s->rx_len, o->d, o->len);
                        s->rx_len += o->len;
                        s->rcv_nxt += o->len;
                        o->used = 0;
                        moved = 1;
                    }
                }
            }
        } else if((i32)(seq - s->rcv_nxt) > 0 && seq - s->rcv_nxt < RXBUF && dlen <= MSS){
            /* arrived early: keep it until the gap is filled */
            for(int k = 0; k < OOO_SLOTS; k++){
                if(s->ooo[k].used && s->ooo[k].seq == seq) break;
                if(!s->ooo[k].used){
                    s->ooo[k].used = 1; s->ooo[k].seq = seq; s->ooo[k].len = dlen;
                    memcpy(s->ooo[k].d, data, dlen);
                    break;
                }
            }
        }
    }
    if(flags & TF_FIN && seq + dlen == s->rcv_nxt){
        s->rcv_nxt++;
        s->peer_closed = 1;
    }
    if(dlen || (flags & TF_FIN)) tcp_send_seg(s, TF_ACK, 0, 0);
}

static void handle_ip(const u8 *p, u32 len){
    if(len < 20 || (p[0] >> 4) != 4) return;
    u8 hl = (u8)((p[0] & 0xF) * 4);
    u16 total = be16(p + 2);
    if(hl < 20 || total > len || total < hl) return;
    u32 src = be32(p + 12), dst = be32(p + 16);
    if(!(dst == my_ip || dst == 0xFFFFFFFF || my_ip == 0)) return;
    const u8 *payload = p + hl;
    u32 plen = total - hl;
    switch(p[9]){
        case IPPROTO_ICMP: handle_icmp(src, payload, plen); break;
        case IPPROTO_UDP:  handle_udp(src, payload, plen); break;
        case IPPROTO_TCP:  tcp_handle(src, payload, plen); break;
    }
}

static void handle_arp(const u8 *p, u32 len){
    if(len < 28) return;
    u16 op = be16(p + 6);
    u32 spa = be32(p + 14), tpa = be32(p + 24);
    arp_store(spa, p + 8);
    if(op == 1 && my_ip && tpa == my_ip) send_arp(2, p + 8, spa);
}

void net_poll(void){
    if(!nic || in_rx) return;                  /* never re-enter the receive path */
    static u8 rf[1600];
    u32 n = 0;
    in_rx = 1;
    for(int k = 0; k < 64; k++){
        if(!nic->recv(rf, sizeof rf, &n)) break;
        if(n < 14) continue;
        u16 type = be16(rf + 12);
        if(type == ETH_ARP) handle_arp(rf + 14, n - 14);
        else if(type == ETH_IP) handle_ip(rf + 14, n - 14);
    }
    in_rx = 0;
}

/* ---------- public API ---------- */
int net_init(void){
    nic = nic_get();
    if(!nic) return -1;
    memcpy(my_mac, nic->mac, 6);
    if(dhcp_acquire() == 0){
        u8 gwm[6];
        resolve(gw_ip, gwm);                  /* warm the cache for the gateway */
        return 0;
    }
    /* no DHCP server answered: use QEMU's user-network defaults */
    my_ip = IP(10,0,2,15); gw_ip = IP(10,0,2,2); dns_ip = IP(10,0,2,3);
    netmask = IP(255,255,255,0);
    u8 gwm[6];
    resolve(gw_ip, gwm);
    return 1;                                 /* up, with the static address */
}

int net_up(void){ return nic != 0 && my_ip != 0; }
void net_info(u32 *ip, u32 *gw, u8 mac[6]){
    if(ip) *ip = my_ip;
    if(gw) *gw = gw_ip;
    if(mac) memcpy(mac, my_mac, 6);
}

int net_ping(u32 ip, u32 timeout_ms, u32 *rtt_ms){
    static u16 seq = 1;
    u8 req[64];
    memset(req, 0, sizeof req);
    req[0] = 8;
    put16(req + 4, 0x1234);
    put16(req + 6, seq);
    for(int i = 8; i < 64; i++) req[i] = (u8)i;
    put16(req + 2, checksum(req, sizeof req, 0));
    ping_got = 0;
    u64 t0 = timer_ticks();
    if(!send_ip(ip, IPPROTO_ICMP, req, sizeof req)) return -1;
    u16 want = seq++;
    while((timer_ticks() - t0) * 10 < timeout_ms){
        net_poll();
        if(ping_got && ping_from == ip && ping_seq_want == want){
            if(rtt_ms) *rtt_ms = (u32)((timer_ticks() - t0) * 10);
            return 0;
        }
        proc_yield();
    }
    return -1;
}

static int tcp_seg(tcb_t *s, u32 seq, u8 flags, const u8 *data, u32 len){
    u8 seg[20 + 1400];
    if(len > 1400) return -1;
    memset(seg, 0, 20);
    put16(seg, s->lport); put16(seg + 2, s->rport);
    put32(seg + 4, seq); put32(seg + 8, s->rcv_nxt);
    seg[12] = 5 << 4; seg[13] = flags;
    u32 win = s->rx ? RXBUF - s->rx_len : 4096;
    if(win > 65535) win = 65535;
    put16(seg + 14, (u16)win);
    s->adv_wnd = win;
    memcpy(seg + 20, data, len);
    u32 total = 20 + len;
    u16 c = checksum(seg, total, pseudo_sum(my_ip, s->ip, IPPROTO_TCP, (u16)total));
    put16(seg + 16, c);
    return send_ip(s->ip, IPPROTO_TCP, seg, total) ? 0 : -1;
}

static int tcp_send_seg(tcb_t *s, u8 flags, const u8 *data, u32 len){
    return tcp_seg(s, s->snd_nxt, flags, data, len);
}

int tcp_connect(u32 ip, u16 port){
    int id = -1;
    for(int i = 0; i < MAX_SOCK; i++) if(!socks[i].used){ id = i; break; }
    if(id < 0) return -1;
    tcb_t *s = &socks[id];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->parent = -1;
    s->ip = ip; s->rport = port; s->lport = next_lport++;
    s->snd_nxt = 0x1000 + (u32)timer_ticks();
    s->rx = kmalloc(RXBUF);
    if(!s->rx){ s->used = 0; return -1; }
    s->state = TS_SYN_SENT;
    for(int tries = 0; tries < 3; tries++){
        tcp_send_seg(s, TF_SYN, 0, 0);
        u64 until = timer_ticks() + 150;
        while(timer_ticks() < until){
            net_poll();
            if(s->state == TS_ESTABLISHED){
                s->snd_nxt++;                 /* our SYN consumes one sequence number */
                tcp_send_seg(s, TF_ACK, 0, 0);
                return id;
            }
            proc_yield();
        }
    }
    kfree(s->rx);
    s->used = 0;
    return -1;
}

int tcp_listen(u16 port){
    int id = -1;
    for(int i = 0; i < MAX_SOCK; i++) if(!socks[i].used){ id = i; break; }
    if(id < 0) return -1;
    tcb_t *s = &socks[id];
    memset(s, 0, sizeof *s);
    s->used = 1; s->state = TS_LISTEN; s->parent = -1;
    s->lport = port;
    return id;
}

int tcp_accept(int lsock, u32 timeout_ms){
    if(lsock < 0 || lsock >= MAX_SOCK || !socks[lsock].used || socks[lsock].state != TS_LISTEN) return -1;
    u64 until = timer_ticks() + timeout_ms / 10;
    for(;;){
        for(int i = 0; i < MAX_SOCK; i++){
            tcb_t *c = &socks[i];
            if(c->used && c->parent == lsock && c->state == TS_ESTABLISHED && !c->accepted){
                c->accepted = 1;
                return i;
            }
        }
        if(timer_ticks() >= until) return -1;
        net_poll();
        proc_yield();
    }
}

/* Sends len bytes reliably: a sliding window limited by the peer's window and
 * a congestion window (slow start, then congestion avoidance). Loss is found by
 * a timeout or by three duplicate ACKs, and the data is sent again from the
 * first unacknowledged byte. Returns when everything is acknowledged. */
int tcp_send(int sock, const void *data, u32 len){
    if(sock < 0 || sock >= MAX_SOCK || !socks[sock].used || socks[sock].state != TS_ESTABLISHED) return -1;
    tcb_t *s = &socks[sock];
    const u8 *p = data;
    if(!s->cwnd){ s->cwnd = 2 * MSS; s->ssthresh = 16 * MSS; }
    u32 base = s->snd_nxt;
    u32 sent = 0, acked = 0;
    u64 last_progress = timer_ticks();
    int timeouts = 0;
    while(acked < len){
        /* fill the window */
        for(;;){
            u32 inflight = sent - acked;
            u32 wnd = s->cwnd < (s->peer_wnd ? s->peer_wnd : MSS) ? s->cwnd : (s->peer_wnd ? s->peer_wnd : MSS);
            if(sent >= len || inflight >= wnd) break;
            u32 chunk = len - sent;
            if(chunk > MSS) chunk = MSS;
            if(chunk > wnd - inflight) chunk = wnd - inflight;
            if(tcp_seg(s, base + sent, TF_PSH | TF_ACK, p + sent, chunk) != 0) return -1;
            sent += chunk;
        }
        net_poll();
        proc_yield();
        if(s->state != TS_ESTABLISHED) return -1;
        u32 newly = s->last_ack_seen - base;
        if(newly > acked && newly <= len){
            acked = newly;
            last_progress = timer_ticks();
            timeouts = 0;
            /* grow the window: exponentially below the threshold, then linearly */
            if(s->cwnd < s->ssthresh) s->cwnd += MSS;
            else s->cwnd += (MSS * MSS) / s->cwnd;
            if(s->cwnd > 65535) s->cwnd = 65535;
        }
        if(s->dupacks >= 3 && acked < sent){
            /* three duplicate ACKs: the next segment was lost; resend from there */
            s->ssthresh = s->cwnd / 2 > 2 * MSS ? s->cwnd / 2 : 2 * MSS;
            s->cwnd = s->ssthresh;
            sent = acked;
            s->dupacks = 0;
        }
        if(acked < len && timer_ticks() - last_progress > 50){
            /* a timeout (500 ms): assume the window was lost */
            if(++timeouts > 8) return -1;
            s->ssthresh = s->cwnd / 2 > 2 * MSS ? s->cwnd / 2 : 2 * MSS;
            s->cwnd = MSS;
            sent = acked;
            last_progress = timer_ticks();
        }
    }
    s->snd_nxt = base + len;
    return 0;
}

int tcp_recv(int sock, void *buf, u32 max, u32 timeout_ms){
    if(sock < 0 || sock >= MAX_SOCK || !socks[sock].used) return -1;
    tcb_t *s = &socks[sock];
    u64 until = timer_ticks() + timeout_ms / 10;
    while(s->rx_len == 0 && !s->peer_closed && timer_ticks() < until){
        net_poll();
        proc_yield();
    }
    u32 n = s->rx_len < max ? s->rx_len : max;
    memcpy(buf, s->rx, n);
    memmove(s->rx, s->rx + n, s->rx_len - n);
    s->rx_len -= n;
    /* reading opened a window the peer saw as (nearly) shut: tell it, or it waits for a timeout */
    if(n && s->state == TS_ESTABLISHED && !s->peer_closed && s->adv_wnd < RXBUF / 4 && RXBUF - s->rx_len >= RXBUF / 2)
        tcp_send_seg(s, TF_ACK, 0, 0);
    return (int)n;
}

/* The other side has closed and every byte it sent has been read. */
int tcp_closed(int sock){
    if(sock < 0 || sock >= MAX_SOCK || !socks[sock].used) return 1;
    return socks[sock].peer_closed && socks[sock].rx_len == 0;
}

void tcp_close(int sock){
    if(sock < 0 || sock >= MAX_SOCK || !socks[sock].used) return;
    tcb_t *s = &socks[sock];
    if(s->state == TS_ESTABLISHED) tcp_send_seg(s, TF_FIN | TF_ACK, 0, 0);
    if(s->rx) kfree(s->rx);
    s->used = 0;
    s->state = TS_FREE;
}
