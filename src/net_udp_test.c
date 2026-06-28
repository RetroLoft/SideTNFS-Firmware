#include "include/net_udp_test.h"

#ifdef SIDETNFS_DEBUG

#include <string.h>
#include <stdio.h>
#include "include/debug.h"
#include "wifi_config.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/dns.h"

// ─── Configuration ────────────────────────────────────────────────────────────

#define TNFS_PORT         16384u
#define TNFS_TIMEOUT_MS   2000u
#define TNFS_MAX_RETRIES  3

// Minimal TNFS MOUNT request: session=0 (new), retry=0, cmd=MOUNT(0x00),
// version 1.0, mount point "/", empty user, empty password.
// Offset 2 (retry field) is patched per attempt in send_probe().
static const uint8_t s_mount_pkt[] = {
    0x00, 0x00,  // session ID — 0 requests a new session
    0x00,        // retry count (patched at send time)
    0x00,        // command: MOUNT
    0x01, 0x00,  // TNFS version 1.0 (major, minor)
    '/', 0x00,   // mount point: "/"
    0x00,        // user: "" (anonymous)
    0x00,        // password: ""
};

// ─── State ────────────────────────────────────────────────────────────────────

typedef enum {
    UDP_TEST_IDLE,
    UDP_TEST_DNS,
    UDP_TEST_RUNNING,
    UDP_TEST_DONE,
} UdpTestState;

static UdpTestState   s_state     = UDP_TEST_IDLE;
static ip_addr_t      s_server_ip;
static struct udp_pcb *s_pcb      = NULL;
static int            s_attempt   = 0;
static uint32_t       s_send_ms   = 0;
static bool           s_got_reply = false;

// Summary updated at each stage so net_udp_test_log_result() can replay it.
static char s_result[128] = "[UdpTest] Not run";

// ─── lwIP callbacks (IRQ / lwIP timer context) ────────────────────────────────

static void udp_recv_cb(void *arg, struct udp_pcb *pcb,
                        struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb;
    if (!p) return;

    snprintf(s_result, sizeof(s_result),
             "[UdpTest] reply from %s:%u len=%u",
             ipaddr_ntoa(addr), (unsigned)port, (unsigned)p->tot_len);
    LOG("%s\n", s_result);

    uint16_t dumplen = p->len < 32 ? p->len : 32;
    for (uint16_t i = 0; i < dumplen; i++)
        LOG(" %02x", ((uint8_t *)p->payload)[i]);
    LOG("\n");

    s_got_reply = true;
    pbuf_free(p);
}

static void dns_found_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)arg;
    if (!addr) {
        snprintf(s_result, sizeof(s_result),
                 "[UdpTest] DNS failed for %s", name);
        LOG("%s\n", s_result);
        s_state = UDP_TEST_DONE;
        return;
    }
    snprintf(s_result, sizeof(s_result),
             "[UdpTest] DNS OK: %s = %s", name, ipaddr_ntoa(addr));
    LOG("%s\n", s_result);
    s_server_ip = *addr;
    s_state     = UDP_TEST_RUNNING;
}

// ─── Internal ─────────────────────────────────────────────────────────────────

// Must be called inside cyw43_arch_lwip_begin()/end().
static void send_probe(void)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(s_mount_pkt), PBUF_RAM);
    if (!p) {
        snprintf(s_result, sizeof(s_result), "[UdpTest] pbuf_alloc failed");
        LOG("%s\n", s_result);
        return;
    }
    memcpy(p->payload, s_mount_pkt, sizeof(s_mount_pkt));
    ((uint8_t *)p->payload)[2] = (uint8_t)s_attempt;  // TNFS retry count

    err_t err = udp_sendto(s_pcb, p, &s_server_ip, TNFS_PORT);
    pbuf_free(p);

    s_send_ms = to_ms_since_boot(get_absolute_time());
    s_attempt++;

    if (err == ERR_OK) {
        snprintf(s_result, sizeof(s_result),
                 "[UdpTest] attempt %d: sent %u bytes to %s:%u",
                 s_attempt, (unsigned)sizeof(s_mount_pkt),
                 ipaddr_ntoa(&s_server_ip), TNFS_PORT);
    } else {
        snprintf(s_result, sizeof(s_result),
                 "[UdpTest] attempt %d: udp_sendto error %d",
                 s_attempt, (int)err);
    }
    LOG("%s\n", s_result);
}

// ─── Public API ───────────────────────────────────────────────────────────────

void net_udp_test_log_result(void)
{
    LOG("%s\n", s_result);
}

bool net_udp_test_ok(void)
{
    return s_state == UDP_TEST_DONE && s_got_reply;
}

void net_udp_test_start(void)
{
    if (s_state != UDP_TEST_IDLE) return;
    s_attempt   = 0;
    s_got_reply = false;
    s_pcb       = NULL;
    s_state     = UDP_TEST_DNS;

    snprintf(s_result, sizeof(s_result),
             "[UdpTest] resolving %s", TNFS_SERVER);
    LOG("%s\n", s_result);

    cyw43_arch_lwip_begin();
    ip_addr_t addr;
    err_t err = dns_gethostbyname(TNFS_SERVER, &addr, dns_found_cb, NULL);
    if (err == ERR_OK) {
        dns_found_cb(TNFS_SERVER, &addr, NULL);  // IP literal or cached
    } else if (err != ERR_INPROGRESS) {
        snprintf(s_result, sizeof(s_result),
                 "[UdpTest] dns error %d for %s", (int)err, TNFS_SERVER);
        LOG("%s\n", s_result);
        s_state = UDP_TEST_DONE;
    }
    cyw43_arch_lwip_end();
}

void net_udp_test_poll(void)
{
    if (s_state != UDP_TEST_RUNNING) return;

    cyw43_arch_lwip_begin();

    if (!s_pcb) {
        s_pcb = udp_new();
        if (!s_pcb) {
            snprintf(s_result, sizeof(s_result), "[UdpTest] udp_new() failed");
            LOG("%s\n", s_result);
            s_state = UDP_TEST_DONE;
            cyw43_arch_lwip_end();
            return;
        }
        udp_recv(s_pcb, udp_recv_cb, NULL);
        send_probe();
        cyw43_arch_lwip_end();
        return;
    }

    if (s_got_reply) {
        udp_remove(s_pcb);
        s_pcb   = NULL;
        s_state = UDP_TEST_DONE;
        cyw43_arch_lwip_end();
        return;
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_send_ms < TNFS_TIMEOUT_MS) {
        cyw43_arch_lwip_end();
        return;
    }

    if (s_attempt >= TNFS_MAX_RETRIES) {
        snprintf(s_result, sizeof(s_result),
                 "[UdpTest] no reply after %d attempts", TNFS_MAX_RETRIES);
        LOG("%s\n", s_result);
        udp_remove(s_pcb);
        s_pcb   = NULL;
        s_state = UDP_TEST_DONE;
        cyw43_arch_lwip_end();
        return;
    }

    LOG("[UdpTest] timeout on attempt %d, retrying\n", s_attempt);
    send_probe();
    cyw43_arch_lwip_end();
}

#endif // SIDETNFS_DEBUG
