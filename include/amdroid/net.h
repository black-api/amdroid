#ifndef AMDROID_NET_H
#define AMDROID_NET_H
#include "amdroid/types.h"

/* IPv4 addresses are u32 in network order as bytes: 10.0.2.2 == 0x0A000202 (big-endian value). */
#define IP(a,b,c,d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

int  net_init(void);                              /* finds a NIC; DHCP for the address (0 = DHCP, 1 = static fallback) */
int  net_up(void);
void net_poll(void);                              /* handle frames that have arrived */
int  net_ping(u32 ip, u32 timeout_ms, u32 *rtt_ms);
void net_info(u32 *ip, u32 *gw, u8 mac[6]);

/* TCP. Sockets are small integers; -1 means failure. */
int  tcp_connect(u32 ip, u16 port);
int  tcp_listen(u16 port);                        /* a listening socket */
int  tcp_accept(int lsock, u32 timeout_ms);       /* the next connection, or -1 */
int  tcp_send(int sock, const void *data, u32 len);
int  tcp_recv(int sock, void *buf, u32 max, u32 timeout_ms);   /* bytes read, 0 at end of stream */
void tcp_close(int sock);

/* DNS and UDP */
int  net_resolve(const char *name, u32 *ip, u32 server, u16 port);   /* server 0 = the DHCP DNS */
u32  net_dns(void);
int  udp_bind(u16 port, void (*fn)(u32 src, u16 sport, const u8 *d, u32 n));
void udp_unbind(u16 port);
int  udp_send(u32 ip, u16 dport, u16 sport, const void *data, u32 len);
#endif
