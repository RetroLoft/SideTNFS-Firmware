#include "include/tnfs_test.h"

#ifdef SIDETNFS_DEBUG

#include <string.h>
#include <stdio.h>
#include "include/debug.h"
#include "include/tnfs_client.h"
#include "include/net_udp_test.h"
#include "wifi_config.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

// ─── Configuration ────────────────────────────────────────────────────────────

#define TNFS_TIMEOUT_MS   2000u
#define TNFS_MAX_RETRIES  3

// ─── State machine ────────────────────────────────────────────────────────────

typedef enum {
    TS_IDLE,
    TS_WAIT_UDP_OK,
    TS_SEND_MOUNT,    TS_WAIT_MOUNT,
    TS_SEND_OPENDIR,  TS_WAIT_OPENDIR,
    TS_SEND_READDIR,  TS_WAIT_READDIR,
    TS_SEND_CLOSEDIR, TS_WAIT_CLOSEDIR,
    TS_DONE,
    TS_ERROR,
} TnfsTestState;

// ─── Per-instance state ───────────────────────────────────────────────────────

static TnfsTestState s_state      = TS_IDLE;
static uint16_t      s_session    = 0;
static uint8_t       s_req_id     = 0;   // monotone per-request sequence number
static uint8_t       s_dir_handle = 0;
static int           s_dir_count  = 0;
static int           s_retries    = 0;
static uint32_t      s_send_ms    = 0;

// Current TX packet stored here so retries can re-send it unchanged (same seq).
static uint8_t  s_tx_buf[TNFS_MTU];
static uint16_t s_tx_len = 0;

static char s_result[128] = "[TNFS] Not run";

// ─── Packet helpers ───────────────────────────────────────────────────────────

// Prepare a new request header in s_tx_buf. Each call increments s_req_id so
// every new logical request has a unique sequence number. Retries re-send the
// unchanged s_tx_buf (same seq), as per the TNFS reference implementation.
static uint8_t *prep(uint16_t session, uint8_t cmd)
{
    s_tx_buf[0] = (uint8_t)(session & 0xFF);
    s_tx_buf[1] = (uint8_t)(session >> 8);
    s_tx_buf[2] = s_req_id++;
    s_tx_buf[3] = cmd;
    return s_tx_buf + 4;
}

static const char *cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case TNFS_CMD_MOUNT:    return "MOUNT";
    case TNFS_CMD_OPENDIR:  return "OPENDIR";
    case TNFS_CMD_READDIR:  return "READDIR";
    case TNFS_CMD_CLOSEDIR: return "CLOSEDIR";
    default:                return "?";
    }
}

static void log_pkt(const char *dir, const uint8_t *buf, uint16_t len)
{
    if (len < 4) return;
    uint16_t sid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    uint8_t  rc  = len >= 5 ? buf[4] : 0xFF;
    DPRINTF("[TNFS] %s %-8s sid=%04x seq=%02x rc=%02x plen=%u\n",
            dir, cmd_name(buf[3]), sid, buf[2], rc,
            (unsigned)(len > 4 ? len - 4 : 0));
}

// Send s_tx_buf[0..s_tx_len) and record timestamp.
static void do_send(void)
{
    log_pkt("TX", s_tx_buf, s_tx_len);
    tnfs_client_send(s_tx_buf, s_tx_len);
    s_send_ms = to_ms_since_boot(get_absolute_time());
}

// Retry: re-send the same packet (seq unchanged) and update timestamp.
static void do_retry(const char *op)
{
    s_retries++;
    LOG("[TNFS] %s timeout, retry %d/%d\n", op, s_retries, TNFS_MAX_RETRIES);
    tnfs_client_send(s_tx_buf, s_tx_len);
    s_send_ms = to_ms_since_boot(get_absolute_time());
}

// Validate a received response against the current in-flight request.
// Session ID is not checked for MOUNT responses (server assigns it there).
static bool validate(const uint8_t *rx, uint16_t len, uint8_t expect_cmd)
{
    if (len < 5) return false;
    uint16_t rx_sid = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8);
    if (rx[2] != s_tx_buf[2])                                   return false; // seq
    if (rx[3] != expect_cmd)                                     return false; // cmd
    if (expect_cmd != TNFS_CMD_MOUNT && rx_sid != s_session)    return false; // session
    return true;
}

static bool timed_out(uint32_t now)
{
    return (now - s_send_ms) >= TNFS_TIMEOUT_MS;
}

// Close PCB and enter error state. Must be called inside lwip_begin/end.
static void go_error(const char *msg)
{
    snprintf(s_result, sizeof(s_result), "[TNFS] Error: %s", msg);
    LOG("%s\n", s_result);
    tnfs_client_close();
    s_state = TS_ERROR;
}

// ─── Packet builders ──────────────────────────────────────────────────────────

static void build_mount(void)
{
    uint8_t *p = prep(0x0000, TNFS_CMD_MOUNT);
    *p++ = TNFS_PROTO_VER_MINOR;   // payload[0]: minor version
    *p++ = TNFS_PROTO_VER_MAJOR;   // payload[1]: major version
    *p++ = '/'; *p++ = '\0';       // mount path: "/"
    *p++ = '\0';                   // user: ""
    *p++ = '\0';                   // password: ""
    s_tx_len = (uint16_t)(p - s_tx_buf);
}

static void build_opendir(void)
{
    uint8_t *p = prep(s_session, TNFS_CMD_OPENDIR);
    *p++ = '/'; *p++ = '\0';
    s_tx_len = (uint16_t)(p - s_tx_buf);
}

static void build_readdir(void)
{
    uint8_t *p  = prep(s_session, TNFS_CMD_READDIR);
    *p++        = s_dir_handle;
    s_tx_len    = (uint16_t)(p - s_tx_buf);
}

static void build_closedir(void)
{
    uint8_t *p  = prep(s_session, TNFS_CMD_CLOSEDIR);
    *p++        = s_dir_handle;
    s_tx_len    = (uint16_t)(p - s_tx_buf);
}

// ─── Public API ───────────────────────────────────────────────────────────────

void tnfs_test_log_result(void)
{
    LOG("%s\n", s_result);
}

void tnfs_test_run_once(void)
{
    if (s_state != TS_IDLE) return;
    s_session   = 0;
    s_req_id    = 0;
    s_dir_count = 0;
    s_retries   = 0;
    s_state     = TS_WAIT_UDP_OK;
    snprintf(s_result, sizeof(s_result), "[TNFS] waiting for UDP test OK...");
    LOG("%s\n", s_result);
}

void tnfs_test_poll(void)
{
    if (s_state == TS_IDLE || s_state == TS_DONE || s_state == TS_ERROR) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Gate: wait until the raw UDP probe confirmed the server replies at all.
    if (s_state == TS_WAIT_UDP_OK) {
        if (!net_udp_test_ok()) return;
        LOG("[TNFS] UDP test OK — connecting to %s:%u\n", TNFS_SERVER, TNFS_PORT);
        ip_addr_t ip;
        ipaddr_aton(TNFS_SERVER, &ip);
        cyw43_arch_lwip_begin();
        bool ok = tnfs_client_open(&ip, TNFS_PORT);
        if (!ok) { go_error("socket open failed"); cyw43_arch_lwip_end(); return; }
        cyw43_arch_lwip_end();
        s_state = TS_SEND_MOUNT;
        // fall through into the main dispatch below
    }

    cyw43_arch_lwip_begin();

    uint8_t  rx[TNFS_MTU];
    uint16_t rlen;

    switch (s_state) {

    // ── MOUNT ─────────────────────────────────────────────────────────────────

    case TS_SEND_MOUNT:
        s_retries = 0;
        build_mount();
        do_send();
        s_state = TS_WAIT_MOUNT;
        break;

    case TS_WAIT_MOUNT:
        rlen = tnfs_client_recv(rx, sizeof(rx) - 1);
        if (rlen > 0) {
            rx[rlen] = '\0';
            log_pkt("RX", rx, rlen);
            if (!validate(rx, rlen, TNFS_CMD_MOUNT) || rx[4] != TNFS_OK) {
                go_error(rlen < 5 ? "short MOUNT response" : "mount rejected");
                break;
            }
            s_session = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8);
            uint8_t  srv_minor  = rlen >= 6 ? rx[5] : 0;
            uint8_t  srv_major  = rlen >= 7 ? rx[6] : 1;
            uint16_t min_retry  = rlen >= 9 ?
                (uint16_t)rx[7] | ((uint16_t)rx[8] << 8) : 0;
            LOG("[TNFS] mounted  sid=%04x  server=%u.%u  min_retry=%ums\n",
                s_session, srv_major, srv_minor, min_retry);
            snprintf(s_result, sizeof(s_result),
                     "[TNFS] mounted sid=%04x server=%u.%u",
                     s_session, srv_major, srv_minor);
            s_state = TS_SEND_OPENDIR;
        } else if (timed_out(now)) {
            if (s_retries >= TNFS_MAX_RETRIES) { go_error("MOUNT timeout"); break; }
            do_retry("MOUNT");
        }
        break;

    // ── OPENDIR ───────────────────────────────────────────────────────────────

    case TS_SEND_OPENDIR:
        s_retries = 0;
        build_opendir();
        do_send();
        s_state = TS_WAIT_OPENDIR;
        break;

    case TS_WAIT_OPENDIR:
        rlen = tnfs_client_recv(rx, sizeof(rx) - 1);
        if (rlen > 0) {
            rx[rlen] = '\0';
            log_pkt("RX", rx, rlen);
            if (!validate(rx, rlen, TNFS_CMD_OPENDIR) || rx[4] != TNFS_OK) {
                go_error("opendir rejected");
                break;
            }
            s_dir_handle = rx[5];
            s_dir_count  = 0;
            LOG("[TNFS] opendir OK  handle=%u\n", s_dir_handle);
            s_state = TS_SEND_READDIR;
        } else if (timed_out(now)) {
            if (s_retries >= TNFS_MAX_RETRIES) { go_error("OPENDIR timeout"); break; }
            do_retry("OPENDIR");
        }
        break;

    // ── READDIR loop ──────────────────────────────────────────────────────────

    case TS_SEND_READDIR:
        s_retries = 0;
        build_readdir();
        // No do_send() log here — one line per entry below is enough
        tnfs_client_send(s_tx_buf, s_tx_len);
        s_send_ms = now;
        s_state   = TS_WAIT_READDIR;
        break;

    case TS_WAIT_READDIR:
        rlen = tnfs_client_recv(rx, sizeof(rx) - 1);
        if (rlen > 0) {
            rx[rlen] = '\0';
            if (!validate(rx, rlen, TNFS_CMD_READDIR)) break; // stale — discard
            uint8_t rc = rx[4];
            if (rc == TNFS_OK && rlen >= 6) {
                LOG("[TNFS]  [%3d]  %s\n", s_dir_count, (char *)&rx[5]);
                s_dir_count++;
                s_state = TS_SEND_READDIR;
            } else {
                if (rc != TNFS_EOF)
                    LOG("[TNFS] readdir rc=%02x — stopping\n", rc);
                LOG("[TNFS] %d entries in /\n", s_dir_count);
                snprintf(s_result, sizeof(s_result),
                         "[TNFS] / listed %d entries", s_dir_count);
                s_state = TS_SEND_CLOSEDIR;
            }
        } else if (timed_out(now)) {
            if (s_retries >= TNFS_MAX_RETRIES) { go_error("READDIR timeout"); break; }
            do_retry("READDIR");
        }
        break;

    // ── CLOSEDIR ──────────────────────────────────────────────────────────────

    case TS_SEND_CLOSEDIR:
        s_retries = 0;
        build_closedir();
        do_send();
        s_state = TS_WAIT_CLOSEDIR;
        break;

    case TS_WAIT_CLOSEDIR:
        rlen = tnfs_client_recv(rx, sizeof(rx) - 1);
        if (rlen > 0) {
            rx[rlen] = '\0';
            if (validate(rx, rlen, TNFS_CMD_CLOSEDIR))
                log_pkt("RX", rx, rlen);
            else
                break; // stale — keep waiting
        } else if (timed_out(now)) {
            if (s_retries < TNFS_MAX_RETRIES) { do_retry("CLOSEDIR"); break; }
            LOG("[TNFS] closedir timeout (ignored)\n");
        } else {
            break; // still waiting
        }
        // Valid CLOSEDIR response or exhausted retries — either way, we're done.
        tnfs_client_close();
        LOG("[TNFS] test complete: %s\n", s_result);
        s_state = TS_DONE;
        break;

    default:
        break;
    }

    cyw43_arch_lwip_end();
}

#endif // SIDETNFS_DEBUG
