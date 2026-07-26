#ifndef LWIP_UDP_H
#define LWIP_UDP_H
#include "pbuf.h"
#include "ip_addr.h"

typedef int8_t err_t;
#define ERR_OK 0

struct udp_pcb
{
    int dummy;
};

typedef void (*udp_recv_fn)(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

struct udp_pcb *udp_new(void);
void udp_remove(struct udp_pcb *pcb);
void udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg);
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, uint16_t dst_port);
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port);
err_t udp_connect(struct udp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port);

#endif
