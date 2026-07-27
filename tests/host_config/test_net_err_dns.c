/**
 * File: test_net_err_dns.c
 * NET_ERR.TXT for TNFS slots whose host is a DNS name.
 *
 * Compiles the REAL sidetnfs_probe.c + sidetnfs_resolve.c against a stub
 * lwIP/cyw43/FatFS layer, so classification, the generated body text and
 * the resolver state machine under test are the production ones. The DNS
 * stub is scriptable and reproduces every outcome lwIP can hand back:
 * an immediate cache hit (ERR_OK, no callback), a deferred answer
 * (ERR_INPROGRESS + callback with an address), NXDOMAIN or an unreachable
 * DNS server (ERR_INPROGRESS + callback with NULL), a synchronous
 * resolver error, and an answer that never arrives at all (the bounded
 * SIDETNFS_RESOLVE_TIMEOUT_MS wait expires).
 *
 * Fopen/Fread/Fseek/Fwrite are mirrored from gemdrvemul.c (which is not
 * host-compilable as a whole -- see test_fase5_net_err_root.c's header for
 * why), but run over the REAL bytes sidetnfs_build_net_err_text() produces
 * for the slot, not a hand-written sample. That is what makes the
 * directory size vs. readable content check meaningful.
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_net_err_dns.c sandbox/sidetnfs_probe.c \
 *       sandbox/sidetnfs_resolve.c sandbox/sidetnfs_config.c \
 *       -o /tmp/test_net_err_dns && /tmp/test_net_err_dns
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_resolve.h"
#include "include/sidetnfs_config.h"
#include "include/filesys.h" // FS_ST_ARCH
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/dns.h"
#include "hardware/flash.h"
#include "f_util.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                            \
    do                                                               \
    {                                                                \
        g_checks++;                                                  \
        if (!(cond))                                                 \
        {                                                            \
            g_failures++;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                             \
    } while (0)

/* ============================================================
 * Stub layer (same shape as test_fase5_net_err_root.c, duplicated per this
 * repo's own convention that each test file builds independently).
 * ============================================================ */

uint8_t g_fake_flash[0x102000];
void flash_range_erase(uint32_t flash_offs, size_t count) { memset(g_fake_flash + flash_offs, 0xFF, count); }
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) { memcpy(g_fake_flash + flash_offs, data, count); }

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) { (void)fp; (void)path; (void)mode; return FR_OK; }
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) { (void)fp; (void)buff; if (bw) *bw = btw; return FR_OK; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_opendir(DIR *dp, const char *path) { (void)dp; (void)path; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; if (fno) fno->fname[0] = '\0'; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

static long g_utc_offset = 0;
long get_utc_offset_seconds(void) { return g_utc_offset; }
void set_utc_offset_seconds(long offset) { g_utc_offset = offset; }

bool ipaddr_aton(const char *cp, ip_addr_t *addr)
{
    unsigned a, b, c, d;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
    {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255)
    {
        return false;
    }
    addr->addr = a | (b << 8) | (c << 16) | (d << 24);
    return true;
}

char *ipaddr_ntoa(const ip_addr_t *addr)
{
    static char buf[16];
    uint32_t a = addr->addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
    return buf;
}

struct pbuf *pbuf_alloc(int layer, uint16_t length, int type)
{
    (void)layer;
    (void)type;
    struct pbuf *p = malloc(sizeof(struct pbuf));
    p->payload = malloc(length);
    p->tot_len = length;
    p->len = length;
    p->next = NULL;
    return p;
}

void pbuf_free(struct pbuf *p)
{
    if (p == NULL)
    {
        return;
    }
    free(p->payload);
    free(p);
}

uint16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr, uint16_t len, uint16_t offset)
{
    uint16_t avail = p->tot_len > offset ? (uint16_t)(p->tot_len - offset) : 0;
    uint16_t n = len < avail ? len : avail;
    memcpy(dataptr, (const uint8_t *)p->payload + offset, n);
    return n;
}

typedef void (*RecvFn)(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static RecvFn g_recv_fn = NULL;
static void *g_recv_arg = NULL;
static struct udp_pcb g_fake_pcb;

struct udp_pcb *udp_new(void) { return &g_fake_pcb; }
void udp_remove(struct udp_pcb *pcb) { (void)pcb; }
void udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg)
{
    (void)pcb;
    g_recv_fn = (RecvFn)recv;
    g_recv_arg = recv_arg;
}
err_t udp_connect(struct udp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port) { (void)pcb; (void)ipaddr; (void)port; return ERR_OK; }
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port) { (void)pcb; (void)ipaddr; (void)port; return ERR_OK; }

typedef struct
{
    bool pending;
    ip_addr_t dst;
    uint16_t port;
    char mount_path[SIDETNFS_MOUNTPATH_LEN + 1];
} LastSend;
static LastSend g_last_send;

static void extract_mount_path(const uint8_t *payload, uint16_t len, char *out, size_t out_size)
{
    if (len <= 6)
    {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%s", (const char *)&payload[6]);
}

err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, uint16_t dst_port)
{
    (void)pcb;
    g_last_send.pending = true;
    g_last_send.dst = *dst_ip;
    g_last_send.port = dst_port;
    extract_mount_path((const uint8_t *)p->payload, p->tot_len, g_last_send.mount_path, sizeof(g_last_send.mount_path));
    return ERR_OK;
}

typedef struct
{
    const char *mount_path;
    bool should_respond;
    uint16_t sid;
    uint8_t rc;
} ScriptEntry;
static ScriptEntry g_script[16];
static int g_script_count = 0;

static void script_add(const char *mount_path, bool respond, uint16_t sid, uint8_t rc)
{
    g_script[g_script_count].mount_path = mount_path;
    g_script[g_script_count].should_respond = respond;
    g_script[g_script_count].sid = sid;
    g_script[g_script_count].rc = rc;
    g_script_count++;
}

/* ---------- scriptable DNS stub ---------- */

typedef enum
{
    DNS_NEVER_ANSWERS = 0, // query accepted, no answer ever -- the bounded wait expires
    DNS_ASYNC_OK,          // ERR_INPROGRESS, then a callback carrying an address
    DNS_ASYNC_FAIL,        // ERR_INPROGRESS, then a callback carrying NULL (NXDOMAIN / no DNS server)
    DNS_CACHE_HIT,         // ERR_OK with the address filled in; the callback is never invoked
    DNS_IMMEDIATE_ERROR,   // synchronous resolver error (ERR_ARG-shaped)
} DnsOutcome;

static DnsOutcome g_dns_outcome = DNS_NEVER_ANSWERS;
static uint32_t g_dns_addr = 0;
static int g_dns_queries = 0;
static dns_found_callback g_dns_cb = NULL;
static void *g_dns_cb_arg = NULL;
static bool g_dns_answer_due = false;

#ifndef ERR_ARG
#define ERR_ARG (-16)
#endif

err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr, dns_found_callback found, void *callback_arg)
{
    (void)hostname;
    g_dns_queries++;
    switch (g_dns_outcome)
    {
    case DNS_CACHE_HIT:
        addr->addr = g_dns_addr;
        return ERR_OK;
    case DNS_IMMEDIATE_ERROR:
        return ERR_ARG;
    case DNS_ASYNC_OK:
    case DNS_ASYNC_FAIL:
        g_dns_cb = found;
        g_dns_cb_arg = callback_arg;
        g_dns_answer_due = true;
        return ERR_INPROGRESS;
    case DNS_NEVER_ANSWERS:
    default:
        return ERR_INPROGRESS;
    }
}

void sleep_ms(uint32_t ms) { (void)ms; }

// Single injection point for both asynchronous deliveries the production
// code waits on: a deferred DNS answer and a MOUNT response.
int cyw43_arch_poll(void)
{
    if (g_dns_answer_due && g_dns_cb != NULL)
    {
        g_dns_answer_due = false;
        if (g_dns_outcome == DNS_ASYNC_OK)
        {
            ip_addr_t a;
            a.addr = g_dns_addr;
            g_dns_cb("host", &a, g_dns_cb_arg);
        }
        else
        {
            g_dns_cb("host", NULL, g_dns_cb_arg);
        }
        return 0;
    }

    if (!g_last_send.pending || g_recv_fn == NULL)
    {
        return 0;
    }
    g_last_send.pending = false;
    for (int i = 0; i < g_script_count; i++)
    {
        if (strcmp(g_script[i].mount_path, g_last_send.mount_path) == 0)
        {
            if (!g_script[i].should_respond)
            {
                return 0;
            }
            uint8_t resp[5];
            resp[0] = (uint8_t)(g_script[i].sid & 0xFF);
            resp[1] = (uint8_t)(g_script[i].sid >> 8);
            resp[2] = 0x00;
            resp[3] = 0x00;
            resp[4] = g_script[i].rc;
            struct pbuf *resp_pbuf = pbuf_alloc(0, sizeof(resp), 1);
            memcpy(resp_pbuf->payload, resp, sizeof(resp));
            g_recv_fn(g_recv_arg, &g_fake_pcb, resp_pbuf, &g_last_send.dst, g_last_send.port);
            return 0;
        }
    }
    return 0; // no script entry -- mount times out
}

/* ---------- slot/config helpers ---------- */

static int g_next_config_slot = 0;

static void build_cfg(sidetnfs_drive_config_t *cfg, int slot, const char *host, uint16_t port, const char *mount_path)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->state = SIDETNFS_DRIVE_SLOT_ENABLED;
    cfg->drive_letter = (uint8_t)('N' + slot);
    cfg->type = SIDETNFS_DRIVE_TNFS;
    cfg->transport = SIDETNFS_TRANSPORT_UDP;
    if (host != NULL)
    {
        strncpy(cfg->host, host, sizeof(cfg->host) - 1);
    }
    cfg->port = port;
    if (mount_path != NULL)
    {
        strncpy(cfg->mount_path, mount_path, sizeof(cfg->mount_path) - 1);
    }
}

// Slot 0's MOUNT packet is built from the active-server globals, which come
// from the persistent config, so slot 0 needs a backing config record too.
static void set_slot(int slot, const char *host, uint16_t port, const char *mount_path)
{
    sidetnfs_drive_config_t cfg;
    build_cfg(&cfg, slot, host, port, mount_path);

    if (slot == 0)
    {
        CHECK(sidetnfs_config_set_drive((uint8_t)g_next_config_slot++, &cfg) == SIDETNFS_CONFIG_STATUS_OK,
              "slot 0's backing config record accepted");
        sidetnfs_probe_load_active_server();
    }
    sidetnfs_probe_set_slot_context(slot, &cfg);
}

// Repopulates only the runtime slot context, the way a reboot does after
// the config already holds the corrected values. Used where writing a
// second config record would collide with the first on drive letter, and
// for a host the config layer deliberately refuses to persist at all.
static void repopulate_slot(int slot, const char *host, uint16_t port, const char *mount_path)
{
    sidetnfs_drive_config_t cfg;
    build_cfg(&cfg, slot, host, port, mount_path);
    sidetnfs_probe_set_slot_context(slot, &cfg);
}

static void scenario_reset(DnsOutcome outcome, uint32_t addr)
{
    sidetnfs_config_init();
    g_next_config_slot = 0;
    g_script_count = 0;
    g_last_send.pending = false;
    g_dns_outcome = outcome;
    g_dns_addr = addr;
    g_dns_queries = 0;
    g_dns_cb = NULL;
    g_dns_cb_arg = NULL;
    g_dns_answer_due = false;
}

// 10.9.0.1 in lwIP ip_addr_t.addr byte order.
static const uint32_t ADDR_10_9_0_1 = 10u | (9u << 8) | (0u << 16) | (1u << 24);

/* ---------- gemdrvemul.c mirrors (verbatim arithmetic) ---------- */

static const int32_t MIRROR_GEMDOS_EACCDN = -36;
static const int32_t MIRROR_GEMDOS_EFILNF = -33;

// A NET_ERR.TXT handle as gemdrive_backend_fopen() builds it: the body
// text is captured once, and config_flash_size is exactly its length.
typedef struct
{
    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    uint32_t config_flash_size;
    uint32_t offset;
} MirrorFd;

// Mirrors gemdrive_backend_fopen()'s NET_ERR branch: read-only (mode 0
// only), and only the one synthetic name exists on this virtual root.
static int32_t mirror_fopen(MirrorFd *fd, int slot, char driveletter, bool wifi_ok, uint16_t mode, const char *name83)
{
    if (mode != 0)
    {
        return MIRROR_GEMDOS_EACCDN;
    }
    if (strcmp(name83, SIDETNFS_NET_ERR_NAME) != 0)
    {
        return MIRROR_GEMDOS_EFILNF;
    }
    memset(fd, 0, sizeof(*fd));
    size_t len = sidetnfs_build_net_err_text(slot, driveletter, wifi_ok, fd->text, sizeof(fd->text));
    fd->config_flash_size = (uint32_t)len;
    fd->offset = 0;
    return 0;
}

// Mirrors GEMDRVEMUL_READ_BUFF_CALL's CONFIG_FLASH/NET_ERR branch: bound
// to the real length, advance the offset by what was actually delivered.
#define MIRROR_READ_BUFFER_SIZE 4096u

static uint32_t mirror_fread(MirrorFd *fd, uint32_t wanted, uint8_t *out)
{
    uint32_t remaining = (fd->offset < fd->config_flash_size) ? (fd->config_flash_size - fd->offset) : 0;
    uint32_t n = wanted > MIRROR_READ_BUFFER_SIZE ? MIRROR_READ_BUFFER_SIZE : wanted;
    if (n > remaining)
    {
        n = remaining;
    }
    if (n > 0 && out != NULL)
    {
        memcpy(out, (const uint8_t *)fd->text + fd->offset, n);
    }
    fd->offset += n;
    return n;
}

// Mirrors the FSEEK_CALL CONFIG_FLASH/NET_ERR branch's offset clamp.
static uint32_t mirror_fseek(MirrorFd *fd, int mode, int32_t offset)
{
    int64_t new_offset;
    switch (mode)
    {
    case 0:
        new_offset = (int64_t)offset;
        break;
    case 1:
        new_offset = (int64_t)fd->offset + (int32_t)offset;
        break;
    case 2:
        new_offset = (int64_t)fd->config_flash_size + (int32_t)offset;
        break;
    default:
        new_offset = (int64_t)fd->offset;
        break;
    }
    if (new_offset < 0)
    {
        new_offset = 0;
    }
    if (new_offset > (int64_t)fd->config_flash_size)
    {
        new_offset = (int64_t)fd->config_flash_size;
    }
    fd->offset = (uint32_t)new_offset;
    return fd->offset;
}

// Mirrors the WRITE_BUFF_CALL NET_ERR branch: always refused.
static int32_t mirror_fwrite(void) { return MIRROR_GEMDOS_EACCDN; }

// Mirrors gemdrvemul.c's Fattrib answer for this virtual file.
static uint32_t mirror_fattrib_inquire(void) { return (uint32_t)(FS_ST_READONLY | FS_ST_ARCH); }

/* ============================================================
 * Tests
 * ============================================================ */

// Item 1: no WiFi -- the wifi text wins over everything else, and never
// mentions DNS even when the host is a name that could not resolve.
static void test_1_wifi_text(void)
{
    printf("Test 1: no WiFi -> NO WIFI text, never a DNS reason\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "tnfs.example.invalid", 16384, "/Atari.ST");

    CHECK(sidetnfs_probe_classify_slot_error(0, false) == SIDETNFS_DRIVE_ERR_NO_WIFI,
          "wifi_ok=false classifies as NO_WIFI regardless of the host being a name");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    size_t len = sidetnfs_build_net_err_text(0, 'N', false, text, sizeof(text));
    CHECK(len == strlen(text), "reported length equals strlen of the generated body");
    CHECK(strstr(text, "NO WIFI") != NULL, "body states NO WIFI");
    CHECK(strstr(text, "drive N") != NULL, "body names drive N");
    CHECK(strstr(text, "DNS") == NULL, "body does not mention DNS when the real problem is WiFi");
    CHECK(strstr(text, "TNFS rc") == NULL, "no TNFS rc is shown for a WiFi failure");
}

// Item 2: the hostname does not exist -- lwIP accepts the query and later
// answers with NULL. Reaches DNS_FAILED, and no MOUNT is ever sent.
static void test_2_hostname_unresolved(void)
{
    printf("Test 2: NXDOMAIN -> DNS_FAILED, no MOUNT sent\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "nosuchhost.example.invalid", 16384, "/Atari.ST");

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_dns_queries == 1, "exactly one DNS query was issued for the name");
    CHECK(!g_last_send.pending, "no MOUNT packet is sent when the name never resolved");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "an NXDOMAIN answer classifies as DNS_FAILED, not SERVER_UNREACHABLE");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    size_t len = sidetnfs_build_net_err_text(0, 'N', true, text, sizeof(text));
    CHECK(len == strlen(text), "reported length equals strlen of the generated body");
    CHECK(strstr(text, "DNS FAILED") != NULL, "body states the DNS FAILED category");
    CHECK(strstr(text, "The TNFS server hostname could not be resolved.") != NULL,
          "body carries the plain-language explanation");
    CHECK(strstr(text, "nosuchhost.example.invalid") != NULL, "body names the host that failed to resolve");
    CHECK(strstr(text, "TNFS rc") == NULL,
          "no TNFS rc is shown for a DNS failure -- no MOUNT was ever sent, so there is no rc");
    CHECK(strstr(text, "Mount:") == NULL, "no mount path is shown for a DNS failure");
    for (size_t i = 0; i < len; i++)
    {
        if ((unsigned char)text[i] > 0x7Fu)
        {
            CHECK(false, "body is plain 7-bit ASCII");
            break;
        }
    }
    CHECK(strstr(text, "\r\n") != NULL, "body uses the existing CRLF newline convention");
    CHECK(text[len - 1] == '\n' && text[len - 2] == '\r', "body ends on a complete CRLF");

    // The whole body, spelled out: this is what the user actually reads on
    // the Atari, so it is asserted literally rather than by keyword.
    static const char expected[] =
        "SideTNFS drive N: not available\r\n"
        "Reason: DNS FAILED\r\n"
        "The TNFS server hostname could not be resolved.\r\n"
        "Host: nosuchhost.example.invalid\r\n";
    CHECK(strcmp(text, expected) == 0, "the DNS body is exactly the expected text");
    CHECK(len == strlen(expected), "the reported length matches that exact text");
}

// Item 2 variant: an unreachable DNS server behaves the same as NXDOMAIN
// from lwIP's side -- a callback carrying NULL after the retries run out.
static void test_2b_dns_server_unreachable(void)
{
    printf("Test 2b: DNS server unreachable -> DNS_FAILED\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "tnfs.example.invalid", 16384, "/Atari.ST");

    sidetnfs_probe_mount_runtime_slots();

    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "a failed lookup against an unreachable DNS server is DNS_FAILED");
    CHECK(!g_last_send.pending, "no MOUNT packet is sent");
}

// Item 2 variant: lwIP rejects the request synchronously (no DNS server
// configured at all, malformed name) -- the error must not be swallowed.
static void test_2c_immediate_resolver_error(void)
{
    printf("Test 2c: synchronous resolver error -> DNS_FAILED\n");
    scenario_reset(DNS_IMMEDIATE_ERROR, 0);
    set_slot(0, "tnfs.example.invalid", 16384, "/Atari.ST");

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_dns_queries == 1, "the resolver was asked once");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "an immediate resolver error is DNS_FAILED, never a silent success");
    CHECK(!g_last_send.pending, "no MOUNT packet is sent");
}

// Item 2 variant: an empty hostname must not even reach the resolver.
static void test_2d_empty_hostname(void)
{
    printf("Test 2d: empty hostname -> DNS_FAILED without a query\n");
    scenario_reset(DNS_ASYNC_OK, ADDR_10_9_0_1);
    sidetnfs_drive_config_t empty_host_cfg;
    build_cfg(&empty_host_cfg, 0, "", 16384, "/Atari.ST");
    CHECK(sidetnfs_config_set_drive(0, &empty_host_cfg) != SIDETNFS_CONFIG_STATUS_OK,
          "the config layer refuses to persist an empty TNFS hostname in the first place");
    // The runtime guard still has to hold on its own, for a slot context
    // that somehow reaches the resolver with nothing to look up.
    repopulate_slot(0, "", 16384, "/Atari.ST");

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_dns_queries == 0, "an empty hostname is rejected before any DNS query is made");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "an empty hostname is DNS_FAILED");
    CHECK(!g_last_send.pending, "no MOUNT packet is sent");
}

// Item 3: the query is accepted but no answer ever arrives; the bounded
// wait has to end the attempt rather than hang.
static void test_3_dns_timeout(void)
{
    printf("Test 3: DNS answer never arrives -> bounded wait ends in DNS_FAILED\n");
    scenario_reset(DNS_NEVER_ANSWERS, 0);
    set_slot(0, "slow.example.invalid", 16384, "/Atari.ST");

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_dns_queries == 1, "the query was issued");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "a lookup that times out is DNS_FAILED");
    CHECK(!g_last_send.pending, "no MOUNT packet is sent after a DNS timeout");

    // The bounded wait is the resolver's own budget, exercised end to end
    // above; assert the constant it is built on is still a real bound.
    CHECK(SIDETNFS_RESOLVE_TIMEOUT_MS > 0 && SIDETNFS_RESOLVE_STEP_MS > 0,
          "the DNS wait is bounded by a non-zero timeout and step");
}

// Item 4: the name resolves and the server answers, but rejects the mount.
// That must stay MOUNT_FAILED -- never be blamed on DNS.
static void test_4_resolved_but_mount_fails(void)
{
    printf("Test 4: name resolves, server reachable, mount rejected -> MOUNT_FAILED\n");
    scenario_reset(DNS_ASYNC_OK, ADDR_10_9_0_1);
    set_slot(0, "tnfs.example.invalid", 16384, "/Rejected");
    script_add("/Rejected", true, 0, 0x02); // a real TNFS rejection code

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_dns_queries == 1, "the name was resolved");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_MOUNT_FAILED,
          "a rejected mount stays MOUNT_FAILED after a successful DNS lookup");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    sidetnfs_build_net_err_text(0, 'N', true, text, sizeof(text));
    CHECK(strstr(text, "MOUNT FAILED") != NULL, "body states MOUNT FAILED");
    CHECK(strstr(text, "hostname could not be resolved") == NULL,
          "a mount failure is never described as a DNS failure");
    CHECK(strstr(text, "Mount: /Rejected") != NULL, "the mount path is shown, since a MOUNT really was attempted");
}

// Item 4 variant: the server never answers at all -> SERVER_UNREACHABLE,
// still distinct from both DNS_FAILED and MOUNT_FAILED.
static void test_4b_server_unreachable_stays_distinct(void)
{
    printf("Test 4b: name resolves, server silent -> SERVER_UNREACHABLE\n");
    scenario_reset(DNS_CACHE_HIT, ADDR_10_9_0_1);
    set_slot(0, "tnfs.example.invalid", 16384, "/Silent");
    // No script entry for "/Silent" -- the mount wait times out.

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_last_send.mount_path[0] != '\0', "a MOUNT really was sent, so this is not a DNS problem");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_SERVER_UNREACHABLE,
          "a silent server is SERVER_UNREACHABLE");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    sidetnfs_build_net_err_text(0, 'N', true, text, sizeof(text));
    CHECK(strstr(text, "SERVER UNREACHABLE") != NULL, "body states SERVER UNREACHABLE");
    CHECK(strstr(text, "hostname could not be resolved") == NULL,
          "an unreachable server is never described as a DNS failure");
}

// Item 5: the size the directory entry reports is exactly the number of
// bytes Fread can deliver -- for the DNS case specifically, since that is
// the body this audit changed.
static void test_5_size_matches_readable_content(void)
{
    printf("Test 5: Fsfirst size == bytes Fread delivers (DNS body)\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "nosuchhost.example.invalid", 16384, "/Atari.ST");
    sidetnfs_probe_mount_runtime_slots();

    SidetnfsAtariDirEntry entry;
    CHECK(sidetnfs_net_err_search_start(0xDEAD0001u, 0, 'N', true, "/", "*.*", FS_ST_ARCH, &entry) ==
              SIDETNFS_DIR_SEARCH_FOUND,
          "the virtual root yields the NET_ERR.TXT entry");
    CHECK(strcmp(entry.name, SIDETNFS_NET_ERR_NAME) == 0, "the entry is named NET_ERR.TXT");

    SidetnfsAtariDirEntry next;
    CHECK(sidetnfs_fake_search_next(0xDEAD0001u, &next) == SIDETNFS_DIR_SEARCH_NOT_FOUND,
          "the virtual root holds exactly one entry, no '.'/'..'");

    MirrorFd fd;
    CHECK(mirror_fopen(&fd, 0, 'N', true, 0, SIDETNFS_NET_ERR_NAME) == 0, "Fopen of NET_ERR.TXT succeeds");
    CHECK(fd.config_flash_size == entry.size,
          "the Fopen handle's size is exactly the size Fsfirst reported -- no estimate anywhere");

    uint8_t buf[SIDETNFS_NET_ERR_TEXT_MAX * 2];
    uint32_t total = 0;
    uint32_t n;
    while ((n = mirror_fread(&fd, 64, buf + total)) > 0)
    {
        total += n;
    }
    CHECK(total == entry.size, "reading to EOF delivers exactly as many bytes as the directory advertised");
    CHECK(memcmp(buf, fd.text, total) == 0, "the delivered bytes are exactly the generated body");
    CHECK(mirror_fread(&fd, 64, buf) == 0, "a further read at EOF delivers nothing, never a stale tail");
}

// Item 6: a partial read leaves the handle positioned for the remainder.
static void test_6_partial_fread(void)
{
    printf("Test 6: partial Fread\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "nosuchhost.example.invalid", 16384, "/Atari.ST");
    sidetnfs_probe_mount_runtime_slots();

    MirrorFd fd;
    CHECK(mirror_fopen(&fd, 0, 'N', true, 0, SIDETNFS_NET_ERR_NAME) == 0, "Fopen succeeds");
    uint32_t size = fd.config_flash_size;
    CHECK(size > 20, "the generated body is long enough for this test to be meaningful");

    uint8_t first[16];
    CHECK(mirror_fread(&fd, 16, first) == 16, "a 16-byte read delivers 16 bytes");
    CHECK(memcmp(first, fd.text, 16) == 0, "the first 16 bytes are the start of the body");
    CHECK(fd.offset == 16, "the offset advanced by exactly what was read");

    uint8_t rest[SIDETNFS_NET_ERR_TEXT_MAX];
    uint32_t n = mirror_fread(&fd, size, rest);
    CHECK(n == size - 16, "the next read is clamped to what is actually left, never past the end");
    CHECK(memcmp(rest, fd.text + 16, n) == 0, "the remainder continues exactly where the first read stopped");
    CHECK(fd.offset == size, "the handle now sits at EOF");
}

// Item 7: Fseek from the beginning, the current position and the end, and
// a read after each lands on the right bytes.
static void test_7_fseek(void)
{
    printf("Test 7: Fseek SET/CUR/END over the synthetic content\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "nosuchhost.example.invalid", 16384, "/Atari.ST");
    sidetnfs_probe_mount_runtime_slots();

    MirrorFd fd;
    CHECK(mirror_fopen(&fd, 0, 'N', true, 0, SIDETNFS_NET_ERR_NAME) == 0, "Fopen succeeds");
    uint32_t size = fd.config_flash_size;

    CHECK(mirror_fseek(&fd, 0, 10) == 10, "SEEK_SET to 10");
    uint8_t b[8];
    CHECK(mirror_fread(&fd, 4, b) == 4, "a read after SEEK_SET delivers 4 bytes");
    CHECK(memcmp(b, fd.text + 10, 4) == 0, "those bytes are the ones at offset 10");

    CHECK(mirror_fseek(&fd, 1, 5) == 19, "SEEK_CUR +5 from the post-read position 14");
    CHECK(mirror_fseek(&fd, 1, -100) == 0, "SEEK_CUR below zero clamps to 0");

    CHECK(mirror_fseek(&fd, 2, 0) == size, "SEEK_END 0 lands exactly at the end");
    CHECK(mirror_fread(&fd, 16, b) == 0, "a read at the end delivers nothing");
    CHECK(mirror_fseek(&fd, 2, -2) == size - 2, "SEEK_END -2 lands two bytes before the end");
    CHECK(mirror_fread(&fd, 16, b) == 2, "exactly the final two bytes are still readable");
    CHECK(b[0] == '\r' && b[1] == '\n', "those final two bytes are the closing CRLF");

    CHECK(mirror_fseek(&fd, 0, 99999) == size, "SEEK_SET past the end clamps to the size, never beyond");
    CHECK(mirror_fread(&fd, 16, b) == 0, "and reading there stays safely empty");
}

// Item 8: the file is read-only on every path that can modify it.
static void test_8_read_only(void)
{
    printf("Test 8: read-only behaviour\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "nosuchhost.example.invalid", 16384, "/Atari.ST");
    sidetnfs_probe_mount_runtime_slots();

    MirrorFd fd;
    CHECK(mirror_fopen(&fd, 0, 'N', true, 1, SIDETNFS_NET_ERR_NAME) == MIRROR_GEMDOS_EACCDN,
          "Fopen for writing is EACCDN");
    CHECK(mirror_fopen(&fd, 0, 'N', true, 2, SIDETNFS_NET_ERR_NAME) == MIRROR_GEMDOS_EACCDN,
          "Fopen for read/write is EACCDN");
    CHECK(mirror_fopen(&fd, 0, 'N', true, 0, SIDETNFS_NET_ERR_NAME) == 0, "Fopen for reading succeeds");
    CHECK(mirror_fwrite() == MIRROR_GEMDOS_EACCDN, "Fwrite on the handle is EACCDN");
    CHECK((mirror_fattrib_inquire() & (uint32_t)FS_ST_READONLY) != 0, "Fattrib reports the read-only bit");
    CHECK(mirror_fopen(&fd, 0, 'N', true, 0, "OTHER.TXT") == MIRROR_GEMDOS_EFILNF,
          "no other name exists on this virtual root");
}

// Item 9: a DNS failure on N: must not touch O:, which mounts normally.
static void test_9_two_slots_different_states(void)
{
    printf("Test 9: N: DNS-failed, O: mounted -- full isolation\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    // Slot 0 uses a name that will not resolve; slot 1 uses a literal, so
    // it never depends on the resolver outcome scripted for slot 0.
    set_slot(0, "nosuchhost.example.invalid", 16384, "/First");
    set_slot(1, "10.9.0.2", 16384, "/Second");
    script_add("/Second", true, 0x4242, 0x00); // TNFS_OK

    sidetnfs_probe_mount_runtime_slots();

    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED, "N: is DNS_FAILED");
    CHECK(sidetnfs_probe_classify_slot_error(1, true) == SIDETNFS_DRIVE_ERR_NONE,
          "O: mounted normally and serves its real root, unaffected by N:'s DNS failure");

    sidetnfs_slot_tnfs_context_t ctx1;
    CHECK(sidetnfs_probe_get_slot_context(1, &ctx1), "O: has a slot context");
    CHECK(!ctx1.host_unresolvable, "O: was never marked host_unresolvable by N:'s failure");
    CHECK(ctx1.session_established, "O: holds its own live session");

    // Only the failing drive gets a virtual root; each body is its own.
    SidetnfsAtariDirEntry entry_n;
    CHECK(sidetnfs_net_err_search_start(0xCAFE0000u, 0, 'N', true, "/", "*.*", FS_ST_ARCH, &entry_n) ==
              SIDETNFS_DIR_SEARCH_FOUND,
          "N: serves NET_ERR.TXT");

    char text_n[SIDETNFS_NET_ERR_TEXT_MAX];
    sidetnfs_build_net_err_text(0, 'N', true, text_n, sizeof(text_n));
    CHECK(strstr(text_n, "drive N") != NULL, "N:'s body names drive N");
    CHECK(strstr(text_n, "nosuchhost.example.invalid") != NULL, "N:'s body names N:'s own host");
    CHECK(strstr(text_n, "10.9.0.2") == NULL, "N:'s body never leaks O:'s host");

    // And a hypothetical error body for O: would describe O:, not N:.
    char text_o[SIDETNFS_NET_ERR_TEXT_MAX];
    sidetnfs_build_net_err_text(1, 'O', true, text_o, sizeof(text_o));
    CHECK(strcmp(text_n, text_o) != 0, "the two slots never share one body");
    CHECK(strstr(text_o, "drive O") != NULL, "O:'s body would name drive O");
}

// Item 10: correcting the hostname and rebooting must clear the error and
// bring the real TNFS root back, with no sticky DNS state.
static void test_10_recovery(void)
{
    printf("Test 10: corrected hostname + reboot -> real root returns\n");
    scenario_reset(DNS_ASYNC_FAIL, 0);
    set_slot(0, "typo.example.invalid", 16384, "/Recovers");
    script_add("/Recovers", true, 0x7777, 0x00); // the server is fine; only the name was wrong

    sidetnfs_probe_mount_runtime_slots();
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "before the fix, N: is DNS_FAILED");

    sidetnfs_slot_tnfs_context_t before;
    CHECK(sidetnfs_probe_get_slot_context(0, &before), "N: has a slot context");
    CHECK(before.host_unresolvable, "the slot is marked host_unresolvable");

    // The user corrects the hostname and reboots: the slot context is
    // repopulated from config, which is what must clear the stale state.
    g_dns_outcome = DNS_ASYNC_OK;
    g_dns_addr = ADDR_10_9_0_1;
    g_dns_queries = 0;
    repopulate_slot(0, "tnfs.example.invalid", 16384, "/Recovers");

    sidetnfs_slot_tnfs_context_t after_repopulate;
    CHECK(sidetnfs_probe_get_slot_context(0, &after_repopulate), "the repopulated slot has a context");
    CHECK(!after_repopulate.host_unresolvable, "repopulating the slot clears host_unresolvable -- it is not sticky");
    CHECK(after_repopulate.resolve.state == SIDETNFS_RESOLVE_IDLE,
          "repopulating the slot also clears any earlier resolver result");

    sidetnfs_probe_mount_runtime_slots();

    CHECK(g_dns_queries == 1, "the corrected name is looked up again");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_NONE,
          "N: is healthy again -- the real TNFS root is served, not NET_ERR.TXT");

    // With the slot healthy, Fsfirst no longer routes to the virtual root:
    // that decision is exactly classify() != NONE in gemdrive_backend_fsfirst().
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_NONE,
          "NET_ERR.TXT disappears because the routing gate reads classify() == NONE");
}

// A resolved name must reuse the cached address instead of querying DNS
// again for every mount of the same slot.
static void test_11_resolution_cached_per_slot(void)
{
    printf("Test 11: a resolved name is looked up once per slot\n");
    scenario_reset(DNS_ASYNC_OK, ADDR_10_9_0_1);
    set_slot(0, "tnfs.example.invalid", 16384, "/Cached");
    script_add("/Cached", true, 0x1111, 0x00);

    sidetnfs_probe_mount_runtime_slots();
    CHECK(g_dns_queries == 1, "the first mount resolves the name");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_NONE, "the slot mounted");

    sidetnfs_probe_mount_runtime_slots();
    CHECK(g_dns_queries == 1, "a second mount of the same slot reuses the resolved address");
}

int main(void)
{
    test_1_wifi_text();
    test_2_hostname_unresolved();
    test_2b_dns_server_unreachable();
    test_2c_immediate_resolver_error();
    test_2d_empty_hostname();
    test_3_dns_timeout();
    test_4_resolved_but_mount_fails();
    test_4b_server_unreachable_stays_distinct();
    test_5_size_matches_readable_content();
    test_6_partial_fread();
    test_7_fseek();
    test_8_read_only();
    test_9_two_slots_different_states();
    test_10_recovery();
    test_11_resolution_cached_per_slot();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
