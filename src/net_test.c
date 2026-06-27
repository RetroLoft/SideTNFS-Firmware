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
static bool           s_logged  = false;   // true once first response line is logged

// Latest one-line result — updated at each stage so net_test_log_result() can
// replay it even if the original LOG was sent before USB was open.
static char test_result[128] = "[NetTest] Not run";

// ─── Internal helpers ─────────────────────────────────────────────────────────

static void close_connection(struct tcp_pcb *pcb)
{
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_close(pcb);
    if (s_pcb == pcb) s_pcb = NULL;
}

// ─── lwIP callbacks (CYW43 background IRQ / lwIP timer context) ──────────────

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (!p) {
        close_connection(pcb);
        return ERR_OK;
    }

    if (!s_logged) {
        // Extract first response line (up to \r\n).
        char buf[128];
        uint16_t n = p->len < sizeof(buf) - 1 ? p->len : (uint16_t)(sizeof(buf) - 1);
        memcpy(buf, p->payload, n);
        buf[n] = '\0';
        char *crlf = strstr(buf, "\r\n");
        if (crlf) *crlf = '\0';
        snprintf(test_result, sizeof(test_result), "[NetTest] HTTP: %s", buf);
        LOG("%s\n", test_result);
        s_logged = true;
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    snprintf(test_result, sizeof(test_result), "[NetTest] TCP error %d", (int)err);
    LOG("%s\n", test_result);
    s_pcb = NULL;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK) {
        snprintf(test_result, sizeof(test_result),
                 "[NetTest] TCP connect failed (err=%d)", (int)err);
        LOG("%s\n", test_result);
        tcp_abort(pcb);
        s_pcb = NULL;
        return ERR_ABRT;
    }
    snprintf(test_result, sizeof(test_result),
             "[NetTest] TCP connected to " HTTP_HOST ":%d — awaiting HTTP response", HTTP_PORT);
    LOG("%s\n", test_result);
    tcp_write(pcb, HTTP_GET, sizeof(HTTP_GET) - 1, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void dns_found_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    if (!addr) {
        snprintf(test_result, sizeof(test_result), "[NetTest] DNS failed for %s", name);
        LOG("%s\n", test_result);
        return;
    }
    snprintf(test_result, sizeof(test_result),
             "[NetTest] DNS OK: %s = %s — TCP connecting...", name, ipaddr_ntoa(addr));
    LOG("%s\n", test_result);

    s_pcb = tcp_new();
    if (!s_pcb) {
        snprintf(test_result, sizeof(test_result), "[NetTest] tcp_new() failed");
        LOG("%s\n", test_result);
        return;
    }
    tcp_err(s_pcb, tcp_err_cb);
    tcp_recv(s_pcb, tcp_recv_cb);

    err_t err = tcp_connect(s_pcb, addr, HTTP_PORT, tcp_connected_cb);
    if (err != ERR_OK) {
        snprintf(test_result, sizeof(test_result),
                 "[NetTest] tcp_connect() error %d", (int)err);
        LOG("%s\n", test_result);
        tcp_abort(s_pcb);
        s_pcb = NULL;
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void net_test_log_result(void)
{
    LOG("%s\n", test_result);
}

void net_test_start(void)
{
    snprintf(test_result, sizeof(test_result), "[NetTest] DNS lookup: " HTTP_HOST);
    LOG("%s\n", test_result);
    s_logged = false;

    cyw43_arch_lwip_begin();
    ip_addr_t addr;
    err_t err = dns_gethostbyname(HTTP_HOST, &addr, dns_found_cb, NULL);
    if (err == ERR_OK) {
        dns_found_cb(HTTP_HOST, &addr, NULL);  // cached — invoke manually
    } else if (err != ERR_INPROGRESS) {
        snprintf(test_result, sizeof(test_result),
                 "[NetTest] dns_gethostbyname() error %d", (int)err);
        LOG("%s\n", test_result);
    }
    cyw43_arch_lwip_end();
}

#endif // SIDETNFS_DEBUG
