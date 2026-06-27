#include "include/net_test.h"

#ifdef SIDETNFS_DEBUG

#include <string.h>
#include "include/debug.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"

// ─── Configuration ────────────────────────────────────────────────────────────

#define HTTP_HOST "example.com"
#define HTTP_PORT  80
#define HTTP_GET   "GET / HTTP/1.0\r\nHost: " HTTP_HOST "\r\nConnection: close\r\n\r\n"

// ─── State ────────────────────────────────────────────────────────────────────

static struct tcp_pcb *s_pcb    = NULL;
static bool           s_logged  = false;  // true once first response line is logged

// ─── Internal helpers ─────────────────────────────────────────────────────────

// Graceful close: unregister callbacks so lwIP doesn't call them after close.
static void net_test_close(struct tcp_pcb *pcb)
{
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_close(pcb);
    if (s_pcb == pcb) s_pcb = NULL;
}

// ─── lwIP callbacks (called from CYW43 background IRQ / lwIP timer) ──────────

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (!p) {
        // Remote closed the connection (FIN received).
        net_test_close(pcb);
        return ERR_OK;
    }

    if (!s_logged) {
        // Log the first line of the HTTP response (everything up to \r\n).
        char buf[128];
        uint16_t n = p->len < sizeof(buf) - 1 ? p->len : (uint16_t)(sizeof(buf) - 1);
        memcpy(buf, p->payload, n);
        buf[n] = '\0';
        char *crlf = strstr(buf, "\r\n");
        if (crlf) *crlf = '\0';
        LOG("[NetTest] HTTP response: %s\n", buf);
        s_logged = true;
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    // PCB is already freed by lwIP at this point — do NOT call tcp_close.
    LOG("[NetTest] TCP error %d\n", (int)err);
    s_pcb = NULL;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK) {
        LOG("[NetTest] TCP connect failed (err=%d)\n", (int)err);
        tcp_abort(pcb);
        s_pcb = NULL;
        return ERR_ABRT;
    }
    LOG("[NetTest] TCP connected to " HTTP_HOST ":%d\n", HTTP_PORT);
    tcp_write(pcb, HTTP_GET, sizeof(HTTP_GET) - 1, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void dns_found_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    if (!addr) {
        LOG("[NetTest] DNS failed for %s\n", name);
        return;
    }
    LOG("[NetTest] DNS OK: %s = %s\n", name, ipaddr_ntoa(addr));

    s_pcb = tcp_new();
    if (!s_pcb) {
        LOG("[NetTest] tcp_new() failed\n");
        return;
    }
    tcp_err(s_pcb, tcp_err_cb);
    tcp_recv(s_pcb, tcp_recv_cb);

    err_t err = tcp_connect(s_pcb, addr, HTTP_PORT, tcp_connected_cb);
    if (err != ERR_OK) {
        LOG("[NetTest] tcp_connect() error %d\n", (int)err);
        tcp_abort(s_pcb);
        s_pcb = NULL;
    } else {
        LOG("[NetTest] TCP connecting...\n");
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void net_test_start(void)
{
    LOG("[NetTest] DNS lookup: " HTTP_HOST "\n");
    s_logged = false;

    cyw43_arch_lwip_begin();
    ip_addr_t addr;
    err_t err = dns_gethostbyname(HTTP_HOST, &addr, dns_found_cb, NULL);
    if (err == ERR_OK) {
        // Result already cached — callback won't be called automatically.
        dns_found_cb(HTTP_HOST, &addr, NULL);
    } else if (err != ERR_INPROGRESS) {
        LOG("[NetTest] dns_gethostbyname() error %d\n", (int)err);
    }
    cyw43_arch_lwip_end();
}

#endif // SIDETNFS_DEBUG
