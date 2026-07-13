/**
 * File: sidetnfs_probe.c
 * Description: One-shot, fire-and-forget UDP reachability probe toward the
 * TNFS server. See include/sidetnfs_probe.h.
 */
#include "include/sidetnfs_probe.h"

#include <string.h>
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define SIDETNFS_SERVER_IP "192.168.178.10"
#define SIDETNFS_SERVER_PORT 16384

static const char SIDETNFS_PROBE_PAYLOAD[] = "SIDETNFS_PROBE";

bool sidetnfs_udp_connect_test(void)
{
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return false;
    }

    cyw43_arch_lwip_begin();

    struct udp_pcb *pcb = udp_new();
    if (!pcb)
    {
        cyw43_arch_lwip_end();
        return false;
    }

    bool connected = (udp_connect(pcb, &server_ip, SIDETNFS_SERVER_PORT) == ERR_OK);

    udp_remove(pcb);
    cyw43_arch_lwip_end();

    return connected;
}

void sidetnfs_send_udp_probe(void)
{
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    struct udp_pcb *pcb = udp_new();
    if (!pcb)
    {
        cyw43_arch_lwip_end();
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(SIDETNFS_PROBE_PAYLOAD) - 1, PBUF_RAM);
    if (!p)
    {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, SIDETNFS_PROBE_PAYLOAD, sizeof(SIDETNFS_PROBE_PAYLOAD) - 1);
    udp_sendto(pcb, p, &server_ip, SIDETNFS_SERVER_PORT);

    pbuf_free(p);
    udp_remove(pcb);

    cyw43_arch_lwip_end();
}
