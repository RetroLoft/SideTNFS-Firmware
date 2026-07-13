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
#define TNFS_PROTO_VER_MINOR 0x02
#define TNFS_PROTO_VER_MAJOR 0x01

#define SIDETNFS_DEBUG_RAW_SIZE 16
#define SIDETNFS_DEBUG_WRITE_MIN_INTERVAL_MS 250

static const char SIDETNFS_PROBE_PAYLOAD[] = "SIDETNFS_PROBE";

// Fase 5F: small, fixed-size debug state. Filled in by the UDP callback
// (RAM only, never touches FatFS) and consumed by
// sidetnfs_debug_file_service(), which is the only place that ever writes
// to SD. No malloc, no dynamic strings.
typedef struct
{
    bool mount_probe_sent;
    bool mount_response_received;
    bool debug_dirty;

    uint32_t mount_probe_send_count;
    uint32_t debug_write_count;

    uint16_t response_len;
    uint16_t sid;
    uint8_t seq;
    uint8_t cmd;
    uint8_t rc;

    uint8_t raw[SIDETNFS_DEBUG_RAW_SIZE];
} SidetnfsDebugState;

static SidetnfsDebugState s_debug_state = {0};

// The MOUNT PCB is intentionally never removed once created: the udp_recv()
// callback must still be able to fire whenever the server's reply happens
// to arrive, and this probe is a one-shot, once-per-boot action, so leaking
// a single PCB for the remaining lifetime of the firmware is the smallest
// safe choice (no risk of removing it out from under an in-flight callback).
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

// lwIP receive callback for the MOUNT probe. Only touches RAM state -- no
// FatFS, no printf, no heavy parsing, no blocking. Always frees the pbuf.
static void mount_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
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

    s_debug_state.response_len = p->tot_len;
    memcpy(s_debug_state.raw, buf, n);
    if (n < sizeof(s_debug_state.raw))
    {
        memset(&s_debug_state.raw[n], 0, sizeof(s_debug_state.raw) - n);
    }
    if (n >= 4)
    {
        s_debug_state.sid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        s_debug_state.seq = buf[2];
        s_debug_state.cmd = buf[3];
    }
    s_debug_state.rc = n > 4 ? buf[4] : 0;
    s_debug_state.mount_response_received = true;
    s_debug_state.debug_dirty = true;

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
    s_debug_state.mount_probe_sent = true;
    s_debug_state.mount_probe_send_count++;
    s_debug_state.debug_dirty = true;

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
    udp_recv(pcb, mount_recv_callback, NULL);

    // Header: session=0x0000 (new session), seq=0x00, cmd=TNFS_CMD_MOUNT.
    // Payload: proto version (minor, major) + null-terminated mount path
    // "/Atari.ST" + empty user + empty password.
    uint8_t buf[6 + 1 + sizeof(SIDETNFS_MOUNT_NAME) + 2];
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
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

static const char *result_text(void)
{
    if (!s_debug_state.mount_response_received)
    {
        return "NO_RESPONSE_YET";
    }
    if (s_debug_state.response_len < 5)
    {
        return "UNKNOWN";
    }
    return s_debug_state.rc == 0x00 ? "OK" : "ERROR";
}

// Fase 5F: (re)write DEBUG.TXT only when dirty, only when hd_folder is
// known, with simple throttling. Never blocks, never retries, silently
// does nothing on any failure -- the dirty flag is left set on failure or
// when throttled, so a later call will retry.
void sidetnfs_debug_file_service(const char *hd_folder)
{
    if (!s_debug_state.debug_dirty || hd_folder == NULL)
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

    char raw_hex[SIDETNFS_DEBUG_RAW_SIZE * 3 + 1] = {0};
    size_t raw_hex_len = 0;
    uint16_t raw_count = s_debug_state.response_len < SIDETNFS_DEBUG_RAW_SIZE
                             ? s_debug_state.response_len
                             : SIDETNFS_DEBUG_RAW_SIZE;
    for (uint16_t i = 0; i < raw_count && raw_hex_len < sizeof(raw_hex); i++)
    {
        int r = snprintf(&raw_hex[raw_hex_len], sizeof(raw_hex) - raw_hex_len, "%02X ", s_debug_state.raw[i]);
        if (r <= 0)
        {
            break;
        }
        raw_hex_len += (size_t)r;
    }

    char text[512];
    int len = snprintf(text, sizeof(text),
                        "SIDETNFS DEBUG\r\n"
                        "Server: " SIDETNFS_SERVER_IP ":16384\r\n"
                        "Mount/root: /" SIDETNFS_MOUNT_NAME "\r\n"
                        "\r\n"
                        "Mount probe sent: %s\r\n"
                        "Mount probe send count: %u\r\n"
                        "\r\n"
                        "Response received: %s\r\n"
                        "Response length: %u\r\n"
                        "SID: %u\r\n"
                        "SEQ: %u\r\n"
                        "CMD: %u\r\n"
                        "RC: %u\r\n"
                        "Result: %s\r\n"
                        "\r\n"
                        "Raw response:\r\n"
                        "%s\r\n"
                        "\r\n"
                        "Debug writes: %u\r\n",
                        s_debug_state.mount_probe_sent ? "yes" : "no",
                        (unsigned)s_debug_state.mount_probe_send_count,
                        s_debug_state.mount_response_received ? "yes" : "no",
                        (unsigned)s_debug_state.response_len,
                        (unsigned)s_debug_state.sid,
                        (unsigned)s_debug_state.seq,
                        (unsigned)s_debug_state.cmd,
                        (unsigned)s_debug_state.rc,
                        result_text(),
                        raw_hex,
                        (unsigned)s_debug_state.debug_write_count);
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

    s_debug_state.debug_dirty = false;
    s_debug_state.debug_write_count++;
}
