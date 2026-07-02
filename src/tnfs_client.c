#include "include/tnfs_client.h"

#include <string.h>
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"

static struct udp_pcb *s_pcb    = NULL;
static uint8_t         s_rx_buf[TNFS_MTU];
static uint16_t        s_rx_len = 0;
static volatile bool   s_rx_rdy = false;

static void recv_cb(void *arg, struct udp_pcb *pcb,
                    struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (!p) return;
    // Use tot_len (full chain) and pbuf_copy_partial so fragmented pbufs are
    // handled correctly; p->len is only the first segment.
    uint16_t n = p->tot_len < TNFS_MTU ? (uint16_t)p->tot_len : (uint16_t)TNFS_MTU;
    pbuf_copy_partial(p, s_rx_buf, n, 0);
    s_rx_len = n;
    s_rx_rdy = true;
    pbuf_free(p);
}

bool tnfs_client_open(const ip_addr_t *ip, uint16_t port)
{
    s_rx_rdy = false;
    s_pcb    = udp_new();
    if (!s_pcb) return false;
    udp_recv(s_pcb, recv_cb, NULL);
    if (udp_connect(s_pcb, ip, port) != ERR_OK) {
        udp_remove(s_pcb);
        s_pcb = NULL;
        return false;
    }
    return true;
}

void tnfs_client_close(void)
{
    if (s_pcb) { udp_remove(s_pcb); s_pcb = NULL; }
}

bool tnfs_client_send(const uint8_t *buf, uint16_t len)
{
    if (!s_pcb) return false;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return false;
    memcpy(p->payload, buf, len);
    err_t err = udp_send(s_pcb, p);
    pbuf_free(p);
    return err == ERR_OK;
}

uint16_t tnfs_client_recv(uint8_t *dst, uint16_t max_len)
{
    if (!s_rx_rdy) return 0;
    uint16_t n = s_rx_len < max_len ? s_rx_len : max_len;
    memcpy(dst, s_rx_buf, n);
    s_rx_rdy = false;
    return n;
}

