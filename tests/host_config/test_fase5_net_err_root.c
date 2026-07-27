/**
 * File: test_fase5_net_err_root.c
 * Fase 5 (virtuele NET_ERR.TXT-root) tests A-I from the task.
 *
 * Part 1 (tests A, B, C, D, E, H) compiles and links the REAL, unmodified
 * romemul/sidetnfs_probe.c (via sandbox/sidetnfs_probe.c, a symlink) --
 * exactly the same real-code harness test_probe_multislot_mount.c already
 * established (stub lwIP/cyw43/FatFS layer, real ipaddr_aton()/pbuf/udp
 * plumbing) -- and exercises the actual, unmodified production functions:
 *   - sidetnfs_probe_classify_slot_error()
 *   - sidetnfs_build_net_err_text()
 *   - sidetnfs_net_err_search_start()/sidetnfs_fake_search_next()/
 *     is_active()/close()
 *   - send_slot_mount_request()/sidetnfs_send_mount_probe() (static,
 *     exercised indirectly, including their new host_unresolvable capture)
 *
 * Part 2 (tests F, G, I) is a faithful MIRROR of the new routing/arithmetic
 * gemdrvemul.c's GEMDRVEMUL_FOPEN_CALL/WRITE_BUFF_CALL/FSEEK_CALL cases now
 * contain -- gemdrvemul.c itself is not host-compilable as a whole
 * (tinyusb/USB-mass-storage/SD-card FatFS/cyw43/lwIP/PIO/DMA/shared-memory
 * dependencies run throughout its ~9000 lines), the same documented,
 * established limitation test_runtime_publication_mirror.c already applies
 * to any gemdrvemul.c-only logic. The mirrored code below is copied
 * verbatim (structure/constants) from the real gemdrvemul.c branches added
 * for Fase 5 -- see gemdrvemul.c's own "Fase 5 (virtual NET_ERR.TXT root)"
 * comments at: gemdrive_backend_fopen() (Fopen routing/mode/name check),
 * the WRITE_BUFF_CALL NET_ERR branch (write refusal), and the FSEEK_CALL
 * CONFIG_FLASH/NET_ERR branch (offset clamp arithmetic, shared verbatim
 * with the pre-existing CONFIG_FLASH backend). Must be kept in sync by
 * hand with gemdrvemul.c if that logic ever changes.
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_fase5_net_err_root.c sandbox/sidetnfs_probe.c \
 *       sandbox/sidetnfs_config.c \
 *       -o /tmp/test_fase5_net_err_root && \
 *       /tmp/test_fase5_net_err_root
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_config.h"
#include "include/filesys.h" // FS_ST_ARCH
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
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
 * Part 1: real sidetnfs_probe.c, same stub layer as
 * test_probe_multislot_mount.c (duplicated here rather than shared, to
 * keep each test file independently buildable per this repo's own
 * convention).
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

static void script_reset(void)
{
    g_script_count = 0;
    g_last_send.pending = false;
}

static void script_add(const char *mount_path, bool respond, uint16_t sid, uint8_t rc)
{
    g_script[g_script_count].mount_path = mount_path;
    g_script[g_script_count].should_respond = respond;
    g_script[g_script_count].sid = sid;
    g_script[g_script_count].rc = rc;
    g_script_count++;
}

int cyw43_arch_poll(void)
{
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
    return 0; // no script entry (or an unparseable host that never even sent) -- timeout
}

static int g_next_config_slot = 0;

static void set_slot(int slot, uint8_t type, const char *host, uint16_t port, const char *mount_path)
{
    sidetnfs_drive_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state = SIDETNFS_DRIVE_SLOT_ENABLED;
    cfg.drive_letter = (uint8_t)('N' + slot);
    cfg.type = type;
    cfg.transport = SIDETNFS_TRANSPORT_UDP;
    if (host != NULL)
    {
        strncpy(cfg.host, host, sizeof(cfg.host) - 1);
    }
    cfg.port = port;
    if (mount_path != NULL)
    {
        strncpy(cfg.mount_path, mount_path, sizeof(cfg.mount_path) - 1);
    }

    if (slot == 0)
    {
        CHECK(sidetnfs_config_set_drive((uint8_t)g_next_config_slot++, &cfg) == SIDETNFS_CONFIG_STATUS_OK,
              "slot 0's backing config record accepted");
        sidetnfs_probe_load_active_server();
    }
    sidetnfs_probe_set_slot_context(slot, &cfg);
}

static void test_setup(void)
{
    sidetnfs_config_init();
    g_next_config_slot = 0;
}

// Test A: no WiFi -> NET_ERR.TXT, category NO_WIFI, regardless of any
// per-slot state (checked first, before even reading the slot context).
static void test_A_no_wifi(void)
{
    printf("Test A: no WiFi -> NO_WIFI category\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.1.0.1", 16384, "/Atari.ST");
    // Deliberately never mounted -- wifi_ok=false must win regardless.

    CHECK(sidetnfs_probe_classify_slot_error(0, false) == SIDETNFS_DRIVE_ERR_NO_WIFI,
          "classify() reports NO_WIFI when wifi_ok is false");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    size_t len = sidetnfs_build_net_err_text(0, 'N', false, text, sizeof(text));
    CHECK(len == strlen(text), "build_net_err_text() returned length matches strlen(text)");
    CHECK(strstr(text, "NO WIFI") != NULL, "NET_ERR.TXT body mentions NO WIFI");
    CHECK(strstr(text, "drive N") != NULL, "NET_ERR.TXT body mentions the correct drive letter");
}

// Test B: DNS/host-parse failure -> the real ipaddr_aton() failure inside
// send_slot_mount_request()/sidetnfs_send_mount_probe() is captured into
// host_unresolvable and surfaces as DNS_FAILED -- never a fabricated code.
static void test_B_dns_failed(void)
{
    printf("Test B: unparseable host -> DNS_FAILED category\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "not-a-valid-ip", 16384, "/Broken");
    // No script entry for "/Broken" -- and none is ever needed, since the
    // real send function returns before ever calling udp_sendto() for an
    // unparseable host (g_last_send.pending stays false).

    sidetnfs_probe_mount_runtime_slots();

    CHECK(!g_last_send.pending, "an unparseable host never actually sent a MOUNT packet");
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_DNS_FAILED,
          "classify() reports DNS_FAILED for an unparseable host");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    size_t len = sidetnfs_build_net_err_text(0, 'N', true, text, sizeof(text));
    CHECK(len == strlen(text), "build_net_err_text() length matches strlen for the DNS-failed case too");
    CHECK(strstr(text, "DNS FAILED") != NULL, "NET_ERR.TXT body mentions DNS FAILED");
}

// Test C: mount error on one drive -> only that drive is not-ready;
// a sibling drive that mounted fine reports SIDETNFS_DRIVE_ERR_NONE.
static void test_C_mount_error_one_drive(void)
{
    printf("Test C: mount error on one drive -> only that drive is affected\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.2.0.1", 16384, "/Good");
    set_slot(1, SIDETNFS_DRIVE_TNFS, "10.2.0.2", 16384, "/Bad");
    script_add("/Good", true, 0x1234, 0x00);   // TNFS_OK
    script_add("/Bad", true, 0, 0x02);          // TNFS_ENOENT-shaped mount rejection

    sidetnfs_probe_mount_runtime_slots();

    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_NONE,
          "the correctly-mounted drive is SIDETNFS_DRIVE_ERR_NONE (real root, not NET_ERR.TXT)");
    CHECK(sidetnfs_probe_classify_slot_error(1, true) == SIDETNFS_DRIVE_ERR_MOUNT_FAILED,
          "the rejected drive is MOUNT_FAILED");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    sidetnfs_build_net_err_text(1, 'O', true, text, sizeof(text));
    CHECK(strstr(text, "MOUNT FAILED") != NULL, "NET_ERR.TXT body mentions MOUNT FAILED for the rejected drive");
}

// Test D: two failing drives, each with its own NET_ERR.TXT, independent
// search state (different ndta) -- closing one must never affect the
// other's active search or content.
static void test_D_two_failing_drives_independent(void)
{
    printf("Test D: two failing drives -> two independent NET_ERR.TXT entries\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.3.0.1", 16384, "/First");
    set_slot(1, SIDETNFS_DRIVE_TNFS, "10.3.0.2", 16384, "/Second");
    script_add("/First", false, 0, 0);  // timeout
    script_add("/Second", false, 0, 0); // timeout

    sidetnfs_probe_mount_runtime_slots();

    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_SERVER_UNREACHABLE,
          "drive 0 (N:) is SERVER_UNREACHABLE");
    CHECK(sidetnfs_probe_classify_slot_error(1, true) == SIDETNFS_DRIVE_ERR_SERVER_UNREACHABLE,
          "drive 1 (O:) is SERVER_UNREACHABLE too, independently");

    const uint32_t ndta_n = 0xAAAA0000;
    const uint32_t ndta_o = 0xBBBB0000;
    SidetnfsAtariDirEntry entry_n, entry_o;
    CHECK(sidetnfs_net_err_search_start(ndta_n, 0, 'N', true, "/", "*.*", FS_ST_ARCH, &entry_n) == SIDETNFS_DIR_SEARCH_FOUND,
          "N:'s own virtual root search finds exactly one entry");
    CHECK(sidetnfs_net_err_search_start(ndta_o, 1, 'O', true, "/", "*.*", FS_ST_ARCH, &entry_o) == SIDETNFS_DIR_SEARCH_FOUND,
          "O:'s own virtual root search finds exactly one entry, independently of N:'s");

    CHECK(strcmp(entry_n.name, SIDETNFS_NET_ERR_NAME) == 0, "N:'s entry is named NET_ERR.TXT");
    CHECK(strcmp(entry_o.name, SIDETNFS_NET_ERR_NAME) == 0, "O:'s entry is named NET_ERR.TXT");

    char text_n[SIDETNFS_NET_ERR_TEXT_MAX], text_o[SIDETNFS_NET_ERR_TEXT_MAX];
    sidetnfs_build_net_err_text(0, 'N', true, text_n, sizeof(text_n));
    sidetnfs_build_net_err_text(1, 'O', true, text_o, sizeof(text_o));
    CHECK(strcmp(text_n, text_o) != 0, "N: and O: get their own distinct NET_ERR.TXT body (different drive/host)");
    CHECK(strstr(text_n, "10.3.0.1") != NULL, "N:'s body mentions its own host");
    CHECK(strstr(text_o, "10.3.0.2") != NULL, "O:'s body mentions its own host, not N:'s");

    // Closing N:'s search must never affect O:'s.
    sidetnfs_fake_search_close(ndta_n);
    CHECK(!sidetnfs_fake_search_is_active(ndta_n), "N:'s search is now closed");
    CHECK(sidetnfs_fake_search_is_active(ndta_o), "O:'s search is still active after N:'s was closed");

    SidetnfsAtariDirEntry entry_o_next;
    CHECK(sidetnfs_fake_search_next(ndta_o, &entry_o_next) == SIDETNFS_DIR_SEARCH_NOT_FOUND,
          "O:'s search correctly exhausts after its one entry (no '.'/'..' , no second file)");
}

// Test E: the reported file size (Fsfirst entry.size) must exactly equal
// the actual generated body length (what Fopen+Fread would return) --
// both computed from the very same sidetnfs_build_net_err_text() call.
static void test_E_size_matches_content(void)
{
    printf("Test E: NET_ERR.TXT size exactly matches its generated content length\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.4.0.1", 16384, "/SizeCheck");
    script_add("/SizeCheck", true, 0, 0x0D); // some real TNFS rejection code

    sidetnfs_probe_mount_runtime_slots();
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_MOUNT_FAILED, "setup: mount rejected");

    char text[SIDETNFS_NET_ERR_TEXT_MAX];
    size_t built_len = sidetnfs_build_net_err_text(0, 'N', true, text, sizeof(text));

    const uint32_t ndta = 0xCCCC0000;
    SidetnfsAtariDirEntry entry;
    CHECK(sidetnfs_net_err_search_start(ndta, 0, 'N', true, "/", "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND,
          "root search finds the one entry");
    CHECK(entry.size == (uint32_t)built_len,
          "Fsfirst-reported size exactly equals the generated NET_ERR.TXT content length (test E)");
    CHECK(entry.size == (uint32_t)strlen(text), "size also equals strlen() of the exact bytes Fopen+Fread would serve");
}

// Test H: automatic recovery -- once the slot's underlying MOUNT actually
// succeeds (the existing mount/session mechanism, not a new background
// service), classify() must immediately report SIDETNFS_DRIVE_ERR_NONE on
// the very next call, live, with no stale/cached state anywhere in this
// layer.
static void test_H_recovery_after_mount_succeeds(void)
{
    printf("Test H: backend recovers -> classify() flips to NONE on the very next call\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.5.0.1", 16384, "/WillRecover");
    script_add("/WillRecover", false, 0, 0); // first attempt: timeout

    sidetnfs_probe_mount_runtime_slots();
    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_SERVER_UNREACHABLE,
          "initially not ready (server unreachable)");

    // Simulate whatever existing mechanism re-attempts this slot's MOUNT
    // (e.g. an Atari-reset GEMDRVEMUL_PING reinit, or the initial boot
    // sequence trying again) -- from this layer's point of view, it is
    // simply another sidetnfs_probe_set_slot_context()+mount attempt that
    // this time succeeds. No new background service is introduced or
    // required here.
    g_next_config_slot = 0; // re-populate the SAME config slot -- same drive, not a new one
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.5.0.1", 16384, "/WillRecover");
    script_reset();
    script_add("/WillRecover", true, 0x9999, 0x00);
    sidetnfs_probe_mount_runtime_slots();

    CHECK(sidetnfs_probe_classify_slot_error(0, true) == SIDETNFS_DRIVE_ERR_NONE,
          "classify() reports NONE immediately after the slot's session is established -- real root, not NET_ERR.TXT");
}

/* ============================================================
 * Part 2: mirror of gemdrvemul.c's new Fase 5 routing/arithmetic --
 * gemdrvemul.c is not host-compilable as a whole (see this file's own
 * header comment). Copied verbatim from the real branches.
 * ============================================================ */

typedef enum
{
    M_BACKEND_SD = 0,
    M_BACKEND_TNFS,
    M_BACKEND_CONFIG_FLASH,
    M_BACKEND_NET_ERR
} MirrorBackend;

// Mirrors gemdrive_backend_fopen()'s new Fase 5 gate: only an ordinary
// (non-SETTINGS) slot whose g_runtime_drives[].backend is TNFS, and whose
// classify() result isn't NONE, is ever routed to the virtual file.
static bool mirror_fopen_routes_to_net_err(bool is_settings_slot, MirrorBackend drive_backend,
                                             SidetnfsDriveErrorCategory category)
{
    if (is_settings_slot)
    {
        return false; // checked first in the real code, unconditionally
    }
    bool is_tnfs_backend = (drive_backend == M_BACKEND_TNFS);
    return is_tnfs_backend && category != SIDETNFS_DRIVE_ERR_NONE;
}

// Mirrors gemdrive_backend_fopen()'s NET_ERR mode/name check. Values match
// romemul/include/gemdrvemul.h's GEMDOS_EACCDN (-36) / GEMDOS_EFILNF (-33).
static const int32_t MIRROR_GEMDOS_EACCDN = -36;
static const int32_t MIRROR_GEMDOS_EFILNF = -33;

static int32_t mirror_fopen_net_err(uint16_t fopen_mode, const char *name83)
{
    if (fopen_mode != 0)
    {
        return MIRROR_GEMDOS_EACCDN;
    }
    if (strcmp(name83, "NET_ERR.TXT") != 0)
    {
        return MIRROR_GEMDOS_EFILNF;
    }
    return 0; // GEMDOS_EOK -- a real fd would be allocated here
}

// Mirrors the WRITE_BUFF_CALL NET_ERR branch: always refused.
static int32_t mirror_write_net_err(void)
{
    return MIRROR_GEMDOS_EACCDN;
}

// Mirrors the FSEEK_CALL CONFIG_FLASH/NET_ERR branch's offset clamp --
// copied verbatim (SEEK_SET=0/SEEK_CUR=1/SEEK_END=2, clamped to
// [0, size]).
static uint32_t mirror_fseek_net_err(uint32_t current_offset, uint32_t size, int mode, int32_t offset)
{
    int64_t new_offset;
    switch (mode)
    {
    case 0:
        new_offset = (int64_t)offset;
        break;
    case 1:
        new_offset = (int64_t)current_offset + (int32_t)offset;
        break;
    case 2:
        new_offset = (int64_t)size + (int32_t)offset;
        break;
    default:
        new_offset = (int64_t)current_offset;
        break;
    }
    if (new_offset < 0)
    {
        new_offset = 0;
    }
    if (new_offset > (int64_t)size)
    {
        new_offset = (int64_t)size;
    }
    return (uint32_t)new_offset;
}

// Test F: Fseek works correctly for NET_ERR.TXT -- SEEK_SET/CUR/END, and
// clamped both below 0 and past the real content length.
static void test_F_fseek_arithmetic(void)
{
    printf("Test F: Fseek offset arithmetic for NET_ERR.TXT (mirror)\n");
    const uint32_t size = 42;

    CHECK(mirror_fseek_net_err(0, size, 0, 10) == 10, "SEEK_SET to 10");
    CHECK(mirror_fseek_net_err(10, size, 1, 5) == 15, "SEEK_CUR +5 from 10");
    CHECK(mirror_fseek_net_err(15, size, 1, -20) == 0, "SEEK_CUR below zero clamps to 0");
    CHECK(mirror_fseek_net_err(0, size, 2, 0) == size, "SEEK_END with 0 offset reaches exactly the real size");
    CHECK(mirror_fseek_net_err(0, size, 2, -2) == size - 2, "SEEK_END -2 lands two bytes before the end");
    CHECK(mirror_fseek_net_err(0, size, 0, 9999) == size, "SEEK_SET past the end clamps to size, never beyond it");
}

// Test G: write operations are refused exactly like a read-only file.
static void test_G_write_refused(void)
{
    printf("Test G: write to NET_ERR.TXT is refused (mirror)\n");
    CHECK(mirror_write_net_err() == MIRROR_GEMDOS_EACCDN, "GEMDRVEMUL_WRITE_BUFF_CALL on a NET_ERR.TXT handle is EACCDN");
}

// Test I: SETTINGS is never given NET_ERR.TXT treatment, and neither is a
// non-TNFS ordinary (SD) slot, regardless of what classify() would say.
static void test_I_settings_never_affected(void)
{
    printf("Test I: SETTINGS (and SD) are never routed to NET_ERR.TXT (mirror)\n");
    CHECK(!mirror_fopen_routes_to_net_err(true, M_BACKEND_CONFIG_FLASH, SIDETNFS_DRIVE_ERR_NO_WIFI),
          "SETTINGS is excluded even when every other signal says 'not ready'");
    CHECK(!mirror_fopen_routes_to_net_err(false, M_BACKEND_SD, SIDETNFS_DRIVE_ERR_NO_WIFI),
          "a non-TNFS/SD slot is never routed to NET_ERR.TXT either -- SD backend stays exactly as before");
    CHECK(mirror_fopen_routes_to_net_err(false, M_BACKEND_TNFS, SIDETNFS_DRIVE_ERR_NO_WIFI),
          "sanity: an ordinary TNFS slot that IS not ready DOES route to NET_ERR.TXT");
    CHECK(!mirror_fopen_routes_to_net_err(false, M_BACKEND_TNFS, SIDETNFS_DRIVE_ERR_NONE),
          "sanity: an ordinary TNFS slot with a live session is never routed to NET_ERR.TXT");

    CHECK(mirror_fopen_net_err(0, "NET_ERR.TXT") == 0, "Fopen(mode=0, NET_ERR.TXT) succeeds");
    CHECK(mirror_fopen_net_err(1, "NET_ERR.TXT") == MIRROR_GEMDOS_EACCDN, "Fopen(mode=1, NET_ERR.TXT) is EACCDN (read-only)");
    CHECK(mirror_fopen_net_err(0, "OTHER.TXT") == MIRROR_GEMDOS_EFILNF, "Fopen of any other name on this virtual root is EFILNF");
}


/* lwIP DNS is not host-buildable; sidetnfs_probe.c only ever reaches this
   for a non-literal host, which these tests do not configure. */
#include "lwip/dns.h"
void sleep_ms(uint32_t ms) { (void)ms; }
err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr, dns_found_callback found, void *callback_arg)
{
    (void)hostname; (void)addr; (void)found; (void)callback_arg;
    return ERR_INPROGRESS;
}

int main(void)
{
    test_A_no_wifi();
    test_B_dns_failed();
    test_C_mount_error_one_drive();
    test_D_two_failing_drives_independent();
    test_E_size_matches_content();
    test_F_fseek_arithmetic();
    test_G_write_refused();
    test_H_recovery_after_mount_succeeds();
    test_I_settings_never_affected();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
