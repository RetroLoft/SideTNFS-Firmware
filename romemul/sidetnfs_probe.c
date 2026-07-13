/**
 * File: sidetnfs_probe.c
 * Description: One-shot, fire-and-forget UDP reachability probe toward the
 * TNFS server, plus a small dirty-flag-driven DEBUG.TXT status file. See
 * include/sidetnfs_probe.h.
 */
#include "include/sidetnfs_probe.h"

#include <string.h>
#include <stdio.h>
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "f_util.h"

#define SIDETNFS_SERVER_IP "192.168.178.10"
#define SIDETNFS_SERVER_PORT 16384
#define SIDETNFS_MOUNT_NAME "Atari.ST"

#define TNFS_CMD_MOUNT 0x00u
#define TNFS_CMD_OPENDIR 0x10u
#define TNFS_PROTO_VER_MINOR 0x02
#define TNFS_PROTO_VER_MAJOR 0x01

// Set to 1 to append a raw hex dump of the last response to DEBUG.TXT.
// Off by default -- Fase 5G wants short, human-readable status lines only.
#ifndef SIDETNFS_DEBUG_SHOW_RAW
#define SIDETNFS_DEBUG_SHOW_RAW 0
#endif

#define SIDETNFS_DEBUG_RAW_SIZE 16
#define SIDETNFS_DEBUG_WRITE_MIN_INTERVAL_MS 250

static const char SIDETNFS_PROBE_PAYLOAD[] = "SIDETNFS_PROBE";

// Fase 5F/5G: small, fixed-size debug state. Filled in by the UDP callback
// (RAM only, never touches FatFS) and consumed by
// sidetnfs_debug_file_service(), which is the only place that ever writes
// to SD. No malloc, no dynamic strings.
typedef struct
{
    bool debug_dirty;
    uint32_t debug_write_count;

    bool network_skipped;

    bool mount_probe_sent;
    bool mount_response_received;
    uint16_t sid;
    uint8_t mount_rc;

    bool opendir_sent;
    bool opendir_response_received;
    uint8_t opendir_handle;
    uint8_t opendir_rc;

#if SIDETNFS_DEBUG_SHOW_RAW
    uint16_t last_response_len;
    uint8_t last_raw[SIDETNFS_DEBUG_RAW_SIZE];
#endif
} SidetnfsDebugState;

static SidetnfsDebugState s_state = {0};

// The MOUNT PCB is intentionally never removed once created: the udp_recv()
// callback must still be able to fire whenever the server's reply happens
// to arrive (for both MOUNT and the later OPENDIR sent over the same PCB),
// and this probe is a one-shot, once-per-boot action, so leaking a single
// PCB for the remaining lifetime of the firmware is the smallest safe
// choice (no risk of removing it out from under an in-flight callback).
static struct udp_pcb *s_mount_pcb = NULL;

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

// lwIP receive callback, shared by MOUNT and OPENDIR responses (both use the
// same PCB). Only touches RAM state -- no FatFS, no printf, no heavy
// parsing, no blocking. Always frees the pbuf. Routes on the echoed command
// byte (offset 3) since both request types share this one callback.
static void tnfs_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;
    if (!p)
    {
        return;
    }

    uint8_t buf[SIDETNFS_DEBUG_RAW_SIZE];
    uint16_t n = p->tot_len < sizeof(buf) ? (uint16_t)p->tot_len : (uint16_t)sizeof(buf);
    pbuf_copy_partial(p, buf, n, 0);

#if SIDETNFS_DEBUG_SHOW_RAW
    s_state.last_response_len = p->tot_len;
    memcpy(s_state.last_raw, buf, n);
    if (n < sizeof(s_state.last_raw))
    {
        memset(&s_state.last_raw[n], 0, sizeof(s_state.last_raw) - n);
    }
#endif

    if (n < 4)
    {
        pbuf_free(p);
        return;
    }

    uint8_t cmd = buf[3];
    if (cmd == TNFS_CMD_MOUNT)
    {
        s_state.sid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        s_state.mount_rc = n > 4 ? buf[4] : 0;
        s_state.mount_response_received = true;
        s_state.debug_dirty = true;
    }
    else if (cmd == TNFS_CMD_OPENDIR)
    {
        s_state.opendir_rc = n > 4 ? buf[4] : 0;
        s_state.opendir_handle = n > 5 ? buf[5] : 0;
        s_state.opendir_response_received = true;
        s_state.debug_dirty = true;
    }
    // Unknown/unexpected command: ignore silently.

    pbuf_free(p);
}

// Fase 5C/5F: send a single TNFS MOUNT request and register a receive
// callback for the (optional, asynchronous) reply. Fire-and-forget from the
// caller's point of view -- this function never waits, never retries, never
// logs, and always returns immediately regardless of whether a reply ever
// arrives. Must only be called after WiFi is confirmed connected.
void sidetnfs_send_mount_probe(void)
{
    // Mark as attempted regardless of what happens below -- this is a
    // fire-and-forget probe, "sent" means "we tried this boot".
    s_state.mount_probe_sent = true;
    s_state.debug_dirty = true;

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

    // Register the reply callback before sending, so a fast reply can never
    // race ahead of us registering it.
    udp_recv(pcb, tnfs_recv_callback, NULL);

    // Header: session=0x0000 (new session), seq=0x00, cmd=TNFS_CMD_MOUNT.
    // Payload: proto version (minor, major) + null-terminated mount path
    // "/Atari.ST" + empty user + empty password.
    uint8_t buf[6 + 1 + sizeof(SIDETNFS_MOUNT_NAME) + 2];
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00; // seq 0 for MOUNT
    buf[3] = TNFS_CMD_MOUNT;
    buf[4] = TNFS_PROTO_VER_MINOR;
    buf[5] = TNFS_PROTO_VER_MAJOR;
    buf[6] = '/';
    memcpy(&buf[7], SIDETNFS_MOUNT_NAME, sizeof(SIDETNFS_MOUNT_NAME)); // includes '\0'
    size_t offset = 7 + sizeof(SIDETNFS_MOUNT_NAME);
    buf[offset++] = '\0'; // user: anonymous
    buf[offset++] = '\0'; // password: none

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!p)
    {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, buf, offset);
    udp_sendto(pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);

    // Keep the PCB alive (see s_mount_pcb comment above) so the callback
    // registered above can still fire for a reply that arrives later.
    s_mount_pcb = pcb;

    cyw43_arch_lwip_end();
}

// Fase 5G: send a single OPENDIR "/" request over the existing MOUNT PCB,
// using the session id learned from the MOUNT response. Fire-and-forget,
// same non-blocking guarantees as sidetnfs_send_mount_probe().
static void send_opendir_probe(void)
{
    s_state.opendir_sent = true;
    s_state.debug_dirty = true;

    if (!s_mount_pcb)
    {
        return;
    }

    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    // Header: session=<from MOUNT response>, seq=0x01, cmd=TNFS_CMD_OPENDIR.
    // Payload: null-terminated path "/" -- MOUNT already scoped the session
    // to "/Atari.ST", so subsequent paths are relative to that root.
    uint8_t buf[6];
    buf[0] = (uint8_t)(s_state.sid & 0xFF);
    buf[1] = (uint8_t)(s_state.sid >> 8);
    buf[2] = 0x01; // seq 1 for OPENDIR
    buf[3] = TNFS_CMD_OPENDIR;
    buf[4] = '/';
    buf[5] = '\0';

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_mount_pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);

    cyw43_arch_lwip_end();
}

void sidetnfs_probe_service(void)
{
    if (s_state.mount_response_received && s_state.mount_rc == 0x00 && !s_state.opendir_sent)
    {
        send_opendir_probe();
    }
}

// Fase 5H: record that networking/TNFS was skipped this boot (no WiFi
// configured, connect/NTP timeout, or ESC/CANCEL during either wait). Only
// touches RAM state -- safe to call regardless of WiFi/cyw43 state.
void sidetnfs_mark_network_skipped(void)
{
    if (s_state.network_skipped)
    {
        return; // already recorded, avoid needless dirty-thrashing
    }
    s_state.network_skipped = true;
    s_state.debug_dirty = true;
}

// Fase 5G/5H: build the short status text. No raw dumps by default (see
// SIDETNFS_DEBUG_SHOW_RAW). Each line is independent so partial progress
// (e.g. mount done, opendir still pending) is always representable.
static int build_status_text(char *text, size_t text_size)
{
    char line1[64];
    char line2[64] = "";

    if (s_state.network_skipped)
    {
        return snprintf(text, text_size, "[SKIP] tnfs disabled\r\n");
    }
    if (!s_state.mount_probe_sent)
    {
        return 0; // nothing to report yet
    }
    else if (!s_state.mount_response_received)
    {
        snprintf(line1, sizeof(line1), "[WAIT] mount response pending\r\n");
    }
    else if (s_state.mount_rc != 0x00)
    {
        snprintf(line1, sizeof(line1), "[ERR] mount failed - rc: %02X\r\n", s_state.mount_rc);
    }
    else
    {
        snprintf(line1, sizeof(line1), "[OK] mounted successful - session id: %04X\r\n", s_state.sid);

        if (s_state.opendir_sent)
        {
            if (!s_state.opendir_response_received)
            {
                snprintf(line2, sizeof(line2), "[WAIT] opendir / response pending\r\n");
            }
            else if (s_state.opendir_rc != 0x00)
            {
                snprintf(line2, sizeof(line2), "[ERR] opendir / failed - rc: %02X\r\n", s_state.opendir_rc);
            }
            else
            {
                snprintf(line2, sizeof(line2), "[OK] opendir / successful - handle: %u\r\n", s_state.opendir_handle);
            }
        }
    }

    int len = snprintf(text, text_size, "%s%s", line1, line2);

#if SIDETNFS_DEBUG_SHOW_RAW
    if (len > 0 && (size_t)len < text_size)
    {
        char raw_hex[SIDETNFS_DEBUG_RAW_SIZE * 3 + 1] = {0};
        size_t raw_hex_len = 0;
        uint16_t raw_count = s_state.last_response_len < SIDETNFS_DEBUG_RAW_SIZE
                                 ? s_state.last_response_len
                                 : SIDETNFS_DEBUG_RAW_SIZE;
        for (uint16_t i = 0; i < raw_count && raw_hex_len < sizeof(raw_hex); i++)
        {
            int r = snprintf(&raw_hex[raw_hex_len], sizeof(raw_hex) - raw_hex_len, "%02X ", s_state.last_raw[i]);
            if (r <= 0)
            {
                break;
            }
            raw_hex_len += (size_t)r;
        }
        int extra = snprintf(text + len, text_size - (size_t)len, "raw: %s\r\n", raw_hex);
        if (extra > 0)
        {
            len += extra;
        }
    }
#endif

    return len;
}

// Fase 5F/5G: (re)write DEBUG.TXT only when dirty, only when hd_folder is
// known, with simple throttling. Never blocks, never retries, silently
// does nothing on any failure -- the dirty flag is left set on failure or
// when throttled, so a later call will retry.
void sidetnfs_debug_file_service(const char *hd_folder)
{
    if (!s_state.debug_dirty || hd_folder == NULL)
    {
        return;
    }

    static absolute_time_t s_last_write_time;
    static bool s_have_last_write_time = false;
    if (s_have_last_write_time &&
        absolute_time_diff_us(s_last_write_time, get_absolute_time()) < (SIDETNFS_DEBUG_WRITE_MIN_INTERVAL_MS * 1000))
    {
        return; // throttled -- stays dirty, retried on a later call
    }

    char path[160];
    int n = snprintf(path, sizeof(path), "%s/DEBUG.TXT", hd_folder);
    if (n <= 0 || (size_t)n >= sizeof(path))
    {
        return;
    }

    char text[192];
    int len = build_status_text(text, sizeof(text));
    if (len <= 0)
    {
        return;
    }
    if ((size_t)len >= sizeof(text))
    {
        len = sizeof(text) - 1; // defensive: still a safe, valid write
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);

    s_last_write_time = get_absolute_time();
    s_have_last_write_time = true;

    if (fr != FR_OK)
    {
        return; // stays dirty; a later call will retry
    }

    UINT written;
    f_write(&file, text, (UINT)len, &written);
    f_close(&file);

    s_state.debug_dirty = false;
    s_state.debug_write_count++;
}
