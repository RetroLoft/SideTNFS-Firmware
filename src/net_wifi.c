#include "include/net_wifi.h"
#include "include/debug.h"
#include "include/net_test.h"
#include "wifi_config.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

// ─── WiFi connection timeout ──────────────────────────────────────────────────
#define WIFI_TIMEOUT_MS 15000

// ── State ─────────────────────────────────────────────────────────────────────

static bool     wifi_done        = false;
static uint32_t wifi_start_ms    = 0;
static uint32_t wifi_last_poll   = 0;

// Latest one-line status — updated on every transition so it can be replayed
// by net_wifi_log_status() even if the original LOG was sent before USB was open.
static char wifi_status_msg[80] = "[WiFi] Not started";

// ── API ───────────────────────────────────────────────────────────────────────

void net_wifi_log_status(void)
{
    LOG("%s\n", wifi_status_msg);
}

void net_wifi_start(void)
{
    snprintf(wifi_status_msg, sizeof(wifi_status_msg),
             "[WiFi] Connecting to %s...", WIFI_SSID);
    LOG("%s\n", wifi_status_msg);

    cyw43_arch_enable_sta_mode();
    int err = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    if (err) {
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] Failed to start (err=%d)", err);
        LOG("%s\n", wifi_status_msg);
        wifi_done = true;
        return;
    }
    wifi_start_ms  = to_ms_since_boot(get_absolute_time());
    wifi_last_poll = wifi_start_ms;
}

void net_wifi_poll(void)
{
    if (wifi_done) return;

    // Rate-limit to once per second
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - wifi_last_poll < 1000) return;
    wifi_last_poll = now;

    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (status == CYW43_LINK_UP) {
        wifi_done = true;
        struct netif *n = netif_list;
        const char *ip = (n) ? ip4addr_ntoa(netif_ip4_addr(n)) : "?";
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] Connected, IP: %s", ip);
        LOG("%s\n", wifi_status_msg);
#ifdef SIDETNFS_DEBUG
        net_test_start();
#endif
        return;
    }

    if (status < 0) {
        wifi_done = true;
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] Failed (status=%d)", status);
        LOG("%s\n", wifi_status_msg);
        return;
    }

    // Still trying — check timeout
    if (now - wifi_start_ms >= WIFI_TIMEOUT_MS) {
        wifi_done = true;
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] Timeout after %d s", WIFI_TIMEOUT_MS / 1000);
        LOG("%s\n", wifi_status_msg);
    }
}
