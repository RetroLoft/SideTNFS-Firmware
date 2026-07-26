/**
 * File: test_probe_multislot_mount.c
 * Fase 13 (runtime-drive-publication audit follow-up) Tests F, G, H, I --
 * compiles and links the REAL romemul/sidetnfs_probe.c (via
 * sandbox/sidetnfs_probe.c, a symlink) against a stub lwIP/cyw43/FatFS
 * layer, not a hand-written mirror. Exercises the actual, unmodified
 * production functions:
 *   - sidetnfs_probe_set_slot_context() / sidetnfs_probe_get_slot_context()
 *   - sidetnfs_probe_mount_runtime_slots() (the generic per-slot mount loop)
 *   - send_slot_mount_request() / sidetnfs_send_mount_probe() (static,
 *     exercised indirectly -- never called directly from this file)
 *   - tnfs_recv_callback() (static, exercised indirectly via the udp_recv()
 *     registration hook -- see cyw43_arch_poll() below)
 *
 * The stub cyw43_arch_poll() below is the injection point: it captures the
 * lwIP udp_recv() callback sidetnfs_probe.c registers, and on each poll
 * iteration of the real wait_for_mount_response() loop, looks up a
 * per-test "script" (keyed by the actual mount_path bytes the real send
 * function put on the wire) to decide whether to synthesize a MOUNT
 * response pbuf and invoke that real callback -- exactly the shape lwIP
 * itself would deliver a real UDP packet in. A script entry that says
 * "don't respond" naturally reproduces a timed-out mount (the real
 * bounded wait_for_mount_response() loop then times out for real,
 * because sleep_us() below is a no-op, this happens near-instantly).
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_probe_multislot_mount.c sandbox/sidetnfs_probe.c \
 *       sandbox/sidetnfs_config.c \
 *       -o /tmp/test_probe_multislot_mount && \
 *       /tmp/test_probe_multislot_mount
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_config.h"
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

/* ---- fake flash + FatFS/rtc stubs, needed only to satisfy the linker
 * for code paths this test never actually exercises. ---- */
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

/* ---- minimal, real IPv4 dotted-quad parsing -- not a mock, an actual
 * (if minimal) implementation, so address/port validation inside the
 * real tnfs_recv_callback() is genuinely exercised, not bypassed. ---- */
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

/* ---- real-enough pbuf allocator: heap backed, so pbuf_free() (called by
 * the real tnfs_recv_callback()) is always safe. ---- */
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

/* ---- lwIP UDP stubs: udp_recv() captures the real
 * tnfs_recv_callback() function pointer sidetnfs_probe.c registers;
 * udp_sendto() records what the real send functions just put on the
 * wire (dest address/port/mount_path), which cyw43_arch_poll() below
 * uses to decide whether/how to answer. ---- */
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
    // Wire shape (see sidetnfs_send_mount_probe()/send_slot_mount_request()):
    // [0..1] session(0) [2] seq(0) [3] cmd [4] proto_minor [5] proto_major
    // [6..] NUL-terminated canonical mount path.
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

/* ---- scripted response table: the test sets this up before calling
 * sidetnfs_probe_mount_runtime_slots(), keyed by the mount_path the real
 * send function will actually put on the wire. ---- */
typedef struct
{
    const char *mount_path;
    bool should_respond; // false == simulate a timed-out mount
    uint16_t sid;
    uint8_t rc;
} ScriptEntry;
static ScriptEntry g_script[16];
static int g_script_count = 0;
static int g_polls_with_injection = 0;

static void script_reset(void)
{
    g_script_count = 0;
    g_last_send.pending = false;
    g_polls_with_injection = 0;
}

static void script_add(const char *mount_path, bool respond, uint16_t sid, uint8_t rc)
{
    g_script[g_script_count].mount_path = mount_path;
    g_script[g_script_count].should_respond = respond;
    g_script[g_script_count].sid = sid;
    g_script[g_script_count].rc = rc;
    g_script_count++;
}

// The one function that actually drives the real, unmodified
// tnfs_recv_callback() -- called by the real wait_for_mount_response()
// loop inside sidetnfs_probe_mount_runtime_slots(), exactly like lwIP's
// own polling would.
int cyw43_arch_poll(void)
{
    if (!g_last_send.pending || g_recv_fn == NULL)
    {
        return 0;
    }
    g_last_send.pending = false; // answer each send at most once
    for (int i = 0; i < g_script_count; i++)
    {
        if (strcmp(g_script[i].mount_path, g_last_send.mount_path) == 0)
        {
            if (!g_script[i].should_respond)
            {
                return 0; // simulate a timed-out mount -- no callback fires
            }
            uint8_t resp[5];
            resp[0] = (uint8_t)(g_script[i].sid & 0xFF);
            resp[1] = (uint8_t)(g_script[i].sid >> 8);
            resp[2] = 0x00; // seq -- no longer used for routing by the real code
            resp[3] = 0x00; // TNFS_CMD_MOUNT
            resp[4] = g_script[i].rc;
            struct pbuf *resp_pbuf = pbuf_alloc(0, sizeof(resp), 1);
            memcpy(resp_pbuf->payload, resp, sizeof(resp));
            g_polls_with_injection++;
            g_recv_fn(g_recv_arg, &g_fake_pcb, resp_pbuf, &g_last_send.dst, g_last_send.port);
            return 0;
        }
    }
    return 0; // no script entry for this mount_path -- treat as timeout too
}

/* ---- test helpers ----
 * Fase 13: slot 0's own MOUNT (sidetnfs_send_mount_probe(), unchanged
 * production code) deliberately still reads the legacy single-session
 * s_active_host/s_active_port/s_active_mount_path globals -- populated
 * in real production by sidetnfs_probe_load_active_server() scanning the
 * persisted config store, never settable directly. So slot 0 here goes
 * through the REAL config store (sidetnfs_config_set_drive() +
 * sidetnfs_probe_load_active_server()) exactly like gemdrvemul.c's own
 * boot flow does, while slots 1.. are populated directly via
 * sidetnfs_probe_set_slot_context() -- exactly what
 * sidetnfs_runtime_drives_init()'s own ordinary-drive loop does for
 * every slot beyond 0. */
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
        sidetnfs_probe_load_active_server(); // populates s_active_host/port/mount_path -- see above
        CHECK(sidetnfs_probe_has_active_server(), "active server resolved from the config store");
    }
    sidetnfs_probe_set_slot_context(slot, &cfg);
}

static void test_setup(void)
{
    sidetnfs_config_init();
    g_next_config_slot = 0;
}

// Test F: three independent TNFS slots, all mounted successfully with
// different session ids -- each slot's context must reflect only its
// own response, never another slot's.
static void test_F_three_independent_tnfs_sessions(void)
{
    printf("Test F: three TNFS slots -> three independent session contexts\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.0.0.1", 16384, "/Atari.ST");
    set_slot(1, SIDETNFS_DRIVE_TNFS, "10.0.0.2", 16384, "/DOS");
    set_slot(2, SIDETNFS_DRIVE_TNFS, "10.0.0.3", 16384, "/Games");
    script_add("/Atari.ST", true, 0x1111, 0x00);
    script_add("/DOS", true, 0x2222, 0x00);
    script_add("/Games", true, 0x3333, 0x00);

    sidetnfs_probe_mount_runtime_slots();

    sidetnfs_slot_tnfs_context_t ctx0, ctx1, ctx2;
    CHECK(sidetnfs_probe_get_slot_context(0, &ctx0), "slot 0 context readable");
    CHECK(sidetnfs_probe_get_slot_context(1, &ctx1), "slot 1 context readable");
    CHECK(sidetnfs_probe_get_slot_context(2, &ctx2), "slot 2 context readable");

    CHECK(ctx0.session_established && ctx0.session_id == 0x1111, "slot 0 has its own session id");
    CHECK(ctx1.session_established && ctx1.session_id == 0x2222, "slot 1 has its own session id");
    CHECK(ctx2.session_established && ctx2.session_id == 0x3333, "slot 2 has its own session id");
    CHECK(ctx0.session_id != ctx1.session_id && ctx1.session_id != ctx2.session_id, "no session id cross-contamination");
}

// Test G: slot 1's mount times out -- slot 0 and slot 2 must still be
// attempted and keep their own correct status.
static void test_G_one_slot_fails_others_proceed(void)
{
    printf("Test G: mount slot 1 fails -> slot 0 and slot 2 keep their own status\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.0.1.1", 16384, "/OkFirst");
    set_slot(1, SIDETNFS_DRIVE_TNFS, "10.0.1.2", 16384, "/WillTimeOut");
    set_slot(2, SIDETNFS_DRIVE_TNFS, "10.0.1.3", 16384, "/OkThird");
    script_add("/OkFirst", true, 0xAAAA, 0x00);
    script_add("/WillTimeOut", false, 0, 0); // no response at all -- real bounded wait times out
    script_add("/OkThird", true, 0xCCCC, 0x00);

    sidetnfs_probe_mount_runtime_slots();

    sidetnfs_slot_tnfs_context_t ctx0, ctx1, ctx2;
    sidetnfs_probe_get_slot_context(0, &ctx0);
    sidetnfs_probe_get_slot_context(1, &ctx1);
    sidetnfs_probe_get_slot_context(2, &ctx2);

    CHECK(ctx0.session_established && ctx0.session_id == 0xAAAA, "slot 0 mounted fine despite slot 1's failure being later");
    CHECK(!ctx1.session_established && !ctx1.response_received, "slot 1 timed out -- no session, no response");
    CHECK(ctx2.session_established && ctx2.session_id == 0xCCCC, "slot 2 mounted fine despite slot 1's earlier failure");
}

// Test H: a SETTINGS-tagged slot (backend_type == SIDETNFS_DRIVE_SD, the
// same tagging gemdrvemul.c's sidetnfs_runtime_drives_init() uses for
// the SETTINGS disk) must never receive a TNFS session.
static void test_H_settings_never_gets_tnfs_session(void)
{
    printf("Test H: a SETTINGS-tagged slot never gets a TNFS session\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.0.2.1", 16384, "/Real");
    set_slot(1, SIDETNFS_DRIVE_SD, NULL, 0, NULL); // SETTINGS-style tagging
    script_add("/Real", true, 0x5555, 0x00);
    // Deliberately no script entry for slot 1 -- it must never even send.

    sidetnfs_probe_mount_runtime_slots();

    sidetnfs_slot_tnfs_context_t ctx1;
    sidetnfs_probe_get_slot_context(1, &ctx1);
    CHECK(!ctx1.session_established, "SETTINGS-tagged slot has no session");
    CHECK(!ctx1.response_received, "SETTINGS-tagged slot was never sent a MOUNT (no response ever attributed)");
    CHECK(!ctx1.mount_pending, "SETTINGS-tagged slot never left pending");
}

// Test I: TNFS + SD + SETTINGS mix -- correct backend tags, only the
// TNFS slot is ever mounted.
static void test_I_mixed_backends_only_tnfs_mounted(void)
{
    printf("Test I: TNFS + SD + SETTINGS mix -> correct backend tags, only TNFS mounted\n");
    test_setup();
    script_reset();
    set_slot(0, SIDETNFS_DRIVE_TNFS, "10.0.3.1", 16384, "/OnlyTnfs");
    set_slot(1, SIDETNFS_DRIVE_SD, NULL, 0, NULL);   // e.g. a real future SD slot
    set_slot(2, SIDETNFS_DRIVE_SD, NULL, 0, NULL);   // SETTINGS, always last
    script_add("/OnlyTnfs", true, 0x7777, 0x00);

    sidetnfs_probe_mount_runtime_slots();

    sidetnfs_slot_tnfs_context_t ctx0, ctx1, ctx2;
    sidetnfs_probe_get_slot_context(0, &ctx0);
    sidetnfs_probe_get_slot_context(1, &ctx1);
    sidetnfs_probe_get_slot_context(2, &ctx2);

    CHECK(ctx0.backend_type == SIDETNFS_DRIVE_TNFS, "slot 0 correctly tagged TNFS");
    CHECK(ctx1.backend_type == SIDETNFS_DRIVE_SD, "slot 1 correctly tagged SD/non-TNFS");
    CHECK(ctx2.backend_type == SIDETNFS_DRIVE_SD, "slot 2 correctly tagged SD/non-TNFS (SETTINGS)");
    CHECK(ctx0.session_established, "only the TNFS slot got mounted");
    CHECK(!ctx1.response_received && !ctx2.response_received, "neither SD/SETTINGS slot was ever attempted as TNFS");
}

int main(void)
{
    test_F_three_independent_tnfs_sessions();
    test_G_one_slot_fails_others_proceed();
    test_H_settings_never_gets_tnfs_session();
    test_I_mixed_backends_only_tnfs_mounted();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
