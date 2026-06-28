#include "include/net_wifi.h"
#include "include/debug.h"
#include "include/net_test.h"
#include "include/net_udp_test.h"
#include "include/tnfs_test.h"
#include "wifi_config.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

// ─── Timing ───────────────────────────────────────────────────────────────────

#define CONNECT_TIMEOUT_MS  15000u
#define RETRY_DELAY_1_MS     5000u
#define RETRY_DELAY_2_MS    10000u
#define RETRY_DELAY_MAX_MS  30000u

// ─── State machine ────────────────────────────────────────────────────────────

typedef enum {
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_FAILED_WAIT,
    WIFI_DISABLED,
} WifiState;

static WifiState s_state       = WIFI_IDLE;
static uint32_t  s_attempt_ms  = 0;                  // when current connect started
static uint32_t  s_next_ms     = 0;                  // absolute time of next retry
static uint32_t  s_retry_delay = RETRY_DELAY_1_MS;   // current backoff interval
static uint32_t  s_last_poll   = 0;
static bool      s_tests_done  = false;

// Latest one-line status replayed by net_wifi_log_status() for late-opening terminals.
static char wifi_status_msg[80] = "[WiFi] Not started";

// ─── Internal ─────────────────────────────────────────────────────────────────

static void advance_retry_delay(void)
{
    if      (s_retry_delay < RETRY_DELAY_2_MS)   s_retry_delay = RETRY_DELAY_2_MS;
    else if (s_retry_delay < RETRY_DELAY_MAX_MS) s_retry_delay = RETRY_DELAY_MAX_MS;
}

static void enter_failed_wait(uint32_t now, int status_code)
{
    uint32_t delay = s_retry_delay;
    advance_retry_delay();
    if (status_code == 0) {
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] Timeout, retry in %us", (unsigned)(delay / 1000));
    } else {
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] Failed (status=%d), retry in %us",
                 status_code, (unsigned)(delay / 1000));
    }
    LOG("%s\n", wifi_status_msg);
    s_next_ms = now + delay;
    s_state   = WIFI_FAILED_WAIT;
}

static void start_connect(void)
{
    snprintf(wifi_status_msg, sizeof(wifi_status_msg),
             "[WiFi] Connecting to %s...", WIFI_SSID);
    LOG("%s\n", wifi_status_msg);

    int err = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD,
                                             CYW43_AUTH_WPA2_AES_PSK);
    if (err) {
        uint32_t now   = to_ms_since_boot(get_absolute_time());
        uint32_t delay = s_retry_delay;
        advance_retry_delay();
        snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                 "[WiFi] connect_async error %d, retry in %us",
                 err, (unsigned)(delay / 1000));
        LOG("%s\n", wifi_status_msg);
        s_next_ms = now + delay;
        s_state   = WIFI_FAILED_WAIT;
        return;
    }
    s_attempt_ms = to_ms_since_boot(get_absolute_time());
    s_state      = WIFI_CONNECTING;
}

// ─── API ──────────────────────────────────────────────────────────────────────

void net_wifi_log_status(void)
{
    LOG("%s\n", wifi_status_msg);
}

void net_wifi_start(void)
{
    cyw43_arch_enable_sta_mode();
    start_connect();
}

void net_wifi_poll(void)
{
    if (s_state == WIFI_IDLE || s_state == WIFI_DISABLED) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_last_poll < 1000) return;
    s_last_poll = now;

    switch (s_state) {

    case WIFI_CONNECTING: {
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status == CYW43_LINK_UP) {
            struct netif *n = netif_list;
            const char  *ip = n ? ip4addr_ntoa(netif_ip4_addr(n)) : "?";
            snprintf(wifi_status_msg, sizeof(wifi_status_msg),
                     "[WiFi] Connected, IP: %s", ip);
            LOG("%s\n", wifi_status_msg);
            s_state       = WIFI_CONNECTED;
            s_retry_delay = RETRY_DELAY_1_MS;  // reset backoff for future drops
            if (!s_tests_done) {
#ifdef SIDETNFS_DEBUG
                net_test_start();
                net_udp_test_start();
                tnfs_test_run_once();
#endif
                s_tests_done = true;
            }
        } else if (status < 0) {
            enter_failed_wait(now, status);
        } else if (now - s_attempt_ms >= CONNECT_TIMEOUT_MS) {
            enter_failed_wait(now, 0);
        }
        break;
    }

    case WIFI_FAILED_WAIT:
        // now - s_next_ms wraps correctly: small when past due, large when still waiting
        if (now - s_next_ms < 0x80000000u) {
            start_connect();
        }
        break;

    case WIFI_CONNECTED: {
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != CYW43_LINK_UP) {
            enter_failed_wait(now, status);
        }
        break;
    }

    default:
        break;
    }
}
