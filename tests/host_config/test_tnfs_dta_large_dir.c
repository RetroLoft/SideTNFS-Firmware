/**
 * File: test_tnfs_dta_large_dir.c
 * XELITE.IT investigation follow-up: an exact (non-wildcard) Fsfirst on
 * N:\...\EPSLQ\XELITE.IT (51-entry directory, XELITE.IT at raw position 30)
 * returns GEMDOS EFILNF on real hardware, even though Fopen/Fread on the
 * exact same path succeed and read the file correctly, and even though the
 * SAME filename in a small (5-entry) directory is found fine by the same
 * exact-Fsfirst call, and even though the real bulk-copy's own
 * wildcard-Fsfirst + repeated-Fsnext directory walk finds XELITE.IT without
 * any problem.
 *
 * Root cause (see the accompanying report): sidetnfs_tnfs_dta_start() and
 * sidetnfs_tnfs_dta_next() both fetch entries one at a time (READDIRX,
 * SIDETNFS_READDIRX_MAX_ENTRIES==1 per round-trip) via the SAME shared
 * loop, tnfs_dta_find_next_match() -- there is only one enumeration engine,
 * not two. That loop is bounded to SIDETNFS_TNFS_DTA_MAX_ROUNDS (32) READDIRX
 * round-trips PER CALL. An Fsnext-driven wildcard walk only ever needs ~1
 * round per call (GEMDOS/Desktop itself supplies the "advance one step"
 * loop externally, one Fsnext per visible entry), so the 32-round cap is
 * never exercised in practice. A single exact-filename Fsfirst call has to
 * walk however many non-matching entries precede the target ENTIRELY
 * inside that one call -- for XELITE.IT at raw position 30 (out of 51,
 * with "." and ".." as the first two raw TNFS entries), that is enough
 * rounds to hit the 32-round cap, which returns SIDETNFS_DIR_SEARCH_ERROR;
 * gemdrive_backend_fsfirst() then collapses that ERROR into plain GEMDOS
 * EFILNF, indistinguishable from a genuine "not found" (see
 * gemdrvemul.c:~1842-1858).
 *
 * This test drives the REAL, unmodified sidetnfs_tnfs_dta_start()/
 * sidetnfs_tnfs_dta_next() (via sandbox/sidetnfs_probe.c, a symlink) against
 * a scripted fake TNFS server that serves OPENDIRX/READDIRX/CLOSEDIR
 * exactly on the wire protocol the real code speaks -- one entry per
 * READDIRX response, "." and ".." included as real raw entries (as any
 * POSIX-backed TNFS server would emit them), so the round-for-round
 * arithmetic in the test matches a real server exactly.
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_tnfs_dta_large_dir.c sandbox/sidetnfs_probe.c \
 *       sandbox/sidetnfs_config.c \
 *       -o /tmp/test_tnfs_dta_large_dir && /tmp/test_tnfs_dta_large_dir
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
#include "lwip/dns.h"

/* lwIP DNS/sleep_ms are not host-buildable; sidetnfs_probe.c only ever
 * reaches these for a non-literal host, which this test does not
 * configure (dotted-quad host only). */
void sleep_ms(uint32_t ms) { (void)ms; }
err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr, dns_found_callback found, void *callback_arg)
{
    (void)hostname;
    (void)addr;
    (void)found;
    (void)callback_arg;
    return ERR_INPROGRESS;
}

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
 * for code paths this test never actually exercises (same boilerplate as
 * test_probe_multislot_mount.c). ---- */
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
static ip_addr_t g_fake_addr;

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

/* ---- outgoing-request capture ---- */
typedef struct
{
    bool pending;
    uint8_t buf[64];
    uint16_t len;
} LastSend;
static LastSend g_last_send;

err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, uint16_t dst_port)
{
    (void)pcb;
    (void)dst_ip;
    (void)dst_port;
    g_last_send.pending = true;
    g_last_send.len = p->tot_len < sizeof(g_last_send.buf) ? p->tot_len : (uint16_t)sizeof(g_last_send.buf);
    memcpy(g_last_send.buf, p->payload, g_last_send.len);
    return ERR_OK;
}

/* ---- fake directory: an ordered list of raw TNFS entry names, "." and
 * ".." included explicitly when the test wants them (a real POSIX-backed
 * TNFS server always emits them first) -- served one per READDIRX
 * round-trip, in order, exactly matching SIDETNFS_READDIRX_MAX_ENTRIES==1.
 * ---- */
#define MAX_FAKE_ENTRIES 600
static char g_dir_entries[MAX_FAKE_ENTRIES][14];
static int g_dir_entry_count = 0;
static int g_next_readdirx_index = 0;
static uint8_t g_opendirx_handle = 7;
static bool g_opendirx_fail = false;
static int g_readdirx_round_count = 0; // rounds actually consumed by the last search

static void dir_reset(void)
{
    g_dir_entry_count = 0;
    g_next_readdirx_index = 0;
    g_readdirx_round_count = 0;
    g_opendirx_fail = false;
}

static void dir_add(const char *name)
{
    if (g_dir_entry_count >= MAX_FAKE_ENTRIES)
    {
        return;
    }
    strncpy(g_dir_entries[g_dir_entry_count], name, sizeof(g_dir_entries[0]) - 1);
    g_dir_entries[g_dir_entry_count][sizeof(g_dir_entries[0]) - 1] = '\0';
    g_dir_entry_count++;
}

// Adds "." and ".." first, then `count` synthetic 8.3 filler filenames
// (FILE0001.TXT style), matching a real POSIX-backed TNFS server's raw
// entry order.
static void dir_fill(int count)
{
    dir_reset();
    dir_add(".");
    dir_add("..");
    for (int i = 0; i < count; i++)
    {
        char name[16];
        snprintf(name, sizeof(name), "F%05d.TXT", i % 100000);
        dir_add(name);
    }
}

// TNFS_CMD_* values, mirrored from sidetnfs_probe.c (not exported via any
// header) -- these are wire-protocol constants, not implementation
// details, so hardcoding them here is the same kind of contract the real
// send/recv functions already hardcode.
#define T_CMD_OPENDIRX 0x17u
#define T_CMD_READDIRX 0x18u
#define T_CMD_CLOSEDIR 0x12u
#define T_OK 0x00u
#define T_EOF 0x21u

int cyw43_arch_poll(void)
{
    if (!g_last_send.pending || g_recv_fn == NULL)
    {
        return 0;
    }
    g_last_send.pending = false;
    uint8_t sid_lo = g_last_send.buf[0];
    uint8_t sid_hi = g_last_send.buf[1];
    uint8_t seq = g_last_send.buf[2];
    uint8_t cmd = g_last_send.buf[3];

    uint8_t resp[300];
    uint16_t resp_len = 0;

    if (cmd == T_CMD_OPENDIRX)
    {
        resp[0] = sid_lo;
        resp[1] = sid_hi;
        resp[2] = seq;
        resp[3] = T_CMD_OPENDIRX;
        resp[4] = g_opendirx_fail ? 0x01u : T_OK;
        resp[5] = g_opendirx_handle;
        resp_len = 6;
        g_next_readdirx_index = 0;
        g_readdirx_round_count = 0;
    }
    else if (cmd == T_CMD_READDIRX)
    {
        g_readdirx_round_count++;
        resp[0] = sid_lo;
        resp[1] = sid_hi;
        resp[2] = seq;
        resp[3] = T_CMD_READDIRX;
        if (g_next_readdirx_index >= g_dir_entry_count)
        {
            resp[4] = T_EOF;
            resp[5] = 0; // batch = 0, no entry this round
            resp[6] = 0;
            resp[7] = 0;
            resp[8] = 0;
            resp_len = 9;
        }
        else
        {
            const char *name = g_dir_entries[g_next_readdirx_index];
            g_next_readdirx_index++;
            resp[4] = T_OK; // real entries always TNFS_OK; a separate
                             // batch=0/TNFS_EOF round follows the last one
            resp[5] = 1;    // batch = 1 entry in this response
            resp[6] = 0;
            resp[7] = 0;
            resp[8] = 0;
            uint16_t off = 9;
            resp[off++] = 0x00; // flags -- plain file
            uint32_t size = 1357;
            resp[off++] = (uint8_t)(size & 0xFF);
            resp[off++] = (uint8_t)((size >> 8) & 0xFF);
            resp[off++] = (uint8_t)((size >> 16) & 0xFF);
            resp[off++] = (uint8_t)((size >> 24) & 0xFF);
            uint32_t mtime = 0x60000000;
            resp[off++] = (uint8_t)(mtime & 0xFF);
            resp[off++] = (uint8_t)((mtime >> 8) & 0xFF);
            resp[off++] = (uint8_t)((mtime >> 16) & 0xFF);
            resp[off++] = (uint8_t)((mtime >> 24) & 0xFF);
            resp[off++] = 0; // ctime (unused by the real parser)
            resp[off++] = 0;
            resp[off++] = 0;
            resp[off++] = 0;
            size_t nlen = strlen(name);
            memcpy(&resp[off], name, nlen);
            off = (uint16_t)(off + nlen);
            resp[off++] = 0; // NUL terminator
            resp_len = off;
        }
    }
    else if (cmd == T_CMD_CLOSEDIR)
    {
        resp[0] = sid_lo;
        resp[1] = sid_hi;
        resp[2] = seq;
        resp[3] = T_CMD_CLOSEDIR;
        resp[4] = T_OK;
        resp_len = 5;
    }
    else
    {
        return 0; // unrecognized command -- ignore, same as a dropped packet
    }

    struct pbuf *resp_pbuf = pbuf_alloc(0, resp_len, 1);
    memcpy(resp_pbuf->payload, resp, resp_len);
    g_recv_fn(g_recv_arg, &g_fake_pcb, resp_pbuf, &g_fake_addr, 0);
    return 0;
}

static void set_slot(int slot, const char *host, uint16_t port)
{
    sidetnfs_drive_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state = SIDETNFS_DRIVE_SLOT_ENABLED;
    cfg.drive_letter = (uint8_t)('N' + slot);
    cfg.type = SIDETNFS_DRIVE_TNFS;
    cfg.transport = SIDETNFS_TRANSPORT_UDP;
    strncpy(cfg.host, host, sizeof(cfg.host) - 1);
    cfg.port = port;
    strncpy(cfg.mount_path, "/Atari.ST", sizeof(cfg.mount_path) - 1);
    sidetnfs_probe_set_slot_context(slot, &cfg);
}

#define TEST_SLOT 1

static void test_setup(void)
{
    sidetnfs_config_init();
    set_slot(TEST_SLOT, "10.0.0.9", 16384);
    g_last_send.pending = false;
}

/* ==== Test 1: exact Fsfirst on the FIRST real entry ==== */
static void test_first_entry(void)
{
    printf("Test 1: exact Fsfirst on the first real entry (round 2, after . and ..)\n");
    test_setup();
    dir_fill(10);
    uint32_t ndta = 0x1001;
    SidetnfsAtariDirEntry entry;
    SidetnfsDirSearchResult r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/EPSLQTEST", "F00000.TXT", 0x37, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND, "first real entry is found");
    CHECK(strcmp(entry.name, "F00000.TXT") == 0, "found entry has the expected name");
    printf("  rounds consumed: %d\n", g_readdirx_round_count);
    sidetnfs_tnfs_dta_release(ndta);
}

/* ==== Test 2: exact Fsfirst exactly AT the current round cap boundary
 * (last round the current 32-round loop still executes) -- must succeed
 * both before and after the fix. ==== */
static void test_at_round_cap_boundary(void)
{
    printf("Test 2: exact Fsfirst on the entry at the last in-budget round (round index 31)\n");
    test_setup();
    // ". " ".." consume rounds 0,1. 30 filler files consume rounds 2..31
    // (30 rounds). The 30th filler file (index 29, name F00029.TXT) is
    // therefore read on round index 31 -- the LAST round the current
    // `for (round = 0; round < 32; round++)` loop still executes.
    dir_fill(30);
    uint32_t ndta = 0x1002;
    SidetnfsAtariDirEntry entry;
    SidetnfsDirSearchResult r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/EPSLQTEST", "F00029.TXT", 0x37, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND, "entry at round index 31 (still in budget) is found");
    printf("  rounds consumed: %d\n", g_readdirx_round_count);
    sidetnfs_tnfs_dta_release(ndta);
}

/* ==== Test 3: exact Fsfirst exactly ONE ROUND PAST the current cap --
 * this is the test that must FAIL on today's code (proving the bug) and
 * PASS after the fix. Mirrors the real EPSLQ case (XELITE.IT at raw
 * position 30 out of 51) structurally, just phrased at the precise
 * boundary instead of relying on the real server's exact entry count. ==== */
static void test_one_round_past_cap_boundary(void)
{
    printf("Test 3 (THE bug): exact Fsfirst one round past the current cap (round index 32)\n");
    test_setup();
    // ". "/".." consume rounds 0,1. 31 filler files consume rounds 2..32
    // (31 rounds). The 31st filler file (index 30, F00030.TXT) is read on
    // round index 32 -- one past `round < 32`, so the CURRENT loop gives up
    // (SIDETNFS_DIR_SEARCH_ERROR) before ever reading it.
    dir_fill(31);
    uint32_t ndta = 0x1003;
    SidetnfsAtariDirEntry entry;
    SidetnfsDirSearchResult r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/EPSLQTEST", "F00030.TXT", 0x37, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND, "entry one round past the old cap is found (fails pre-fix, passes post-fix)");
    printf("  rounds consumed: %d, result=%d (0=FOUND,1=NOT_FOUND,2=ERROR)\n", g_readdirx_round_count, (int)r);
    sidetnfs_tnfs_dta_release(ndta);
}

/* ==== Test 4: exact Fsfirst well past the OLD cap, matching the real
 * EPSLQ scenario (51 entries, target at raw position 30) as closely as
 * practical -- and the analogous "last entry" case in a much bigger
 * (200-entry) directory, to prove the fix scales, not just barely clears
 * the old boundary. ==== */
static void test_realistic_epslq_case(void)
{
    printf("Test 4a: exact Fsfirst matching the real EPSLQ shape (51 entries, target at raw position 30)\n");
    test_setup();
    dir_reset();
    dir_add(".");
    dir_add("..");
    static const char *epslq_like[] = {
        "XPROP.FNT", "PICABRBI.PF", "PICABRBD.PF", "PICABD.PF", "XPROP.IT", "PICABRIT.PF", "XSCHMAL.IT",
        "XSCHMAL.SML", "BOXLINE.PF", "ELITEBI.PF", "XELITE.FNT", "ELITE.PF", "XPICABRE.SIT", "SCHMALIT.PF",
        "XBOXLINE.FNT", "XPICA.IT", "ELITEIT.PF", "XELITBRE.IT", "PROP.PF", "XELITE.SIT", "PICABI.PF", "PICA.PF",
        "XELITBRE.SIT", "PROPBD.PF", "PICABR.PF", "XELITE.SML", "PFLISTWP", "ELITBR.PF", "ELITEBD.PF", "XELITE.IT"};
    for (size_t i = 0; i < sizeof(epslq_like) / sizeof(epslq_like[0]); i++)
    {
        dir_add(epslq_like[i]);
    }
    for (int i = 0; i < 21; i++) // pad out to 51 total, matching the real directory's size
    {
        char name[14];
        snprintf(name, sizeof(name), "PAD%05d.PF", i);
        dir_add(name);
    }
    CHECK(g_dir_entry_count == 51 + 2, "synthetic EPSLQ directory has 51 real entries plus . and ..");

    uint32_t ndta = 0x1004;
    SidetnfsAtariDirEntry entry;
    SidetnfsDirSearchResult r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/TOOLS/WORDPLUS/WRDPLUS4.TKT/WP4SYS/EPSLQ",
                                                           "XELITE.IT", 0x37, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND, "XELITE.IT is found via exact Fsfirst on the realistic 51-entry layout");
    printf("  rounds consumed: %d, result=%d\n", g_readdirx_round_count, (int)r);
    sidetnfs_tnfs_dta_release(ndta);

    printf("Test 4b: exact Fsfirst on the LAST entry of a 200-entry directory (proves the fix scales)\n");
    test_setup();
    dir_fill(200);
    ndta = 0x1005;
    char last_name[14];
    snprintf(last_name, sizeof(last_name), "F%05d.TXT", 199);
    r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/BIGDIR", last_name, 0x37, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND, "last entry of a 200-entry directory is found");
    printf("  rounds consumed: %d\n", g_readdirx_round_count);
    sidetnfs_tnfs_dta_release(ndta);
}

/* ==== Test 5: exact Fsfirst for a name that genuinely does not exist --
 * must return NOT_FOUND (not FOUND, not a hang), unchanged by the fix. ==== */
static void test_nonexistent_entry(void)
{
    printf("Test 5: exact Fsfirst for a nonexistent filename -> NOT_FOUND\n");
    test_setup();
    dir_fill(10);
    uint32_t ndta = 0x1006;
    SidetnfsAtariDirEntry entry;
    SidetnfsDirSearchResult r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/EPSLQTEST", "NOSUCH.IT", 0x37, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_NOT_FOUND, "nonexistent filename correctly reports NOT_FOUND, not FOUND");
    CHECK(!sidetnfs_tnfs_dta_is_active(ndta), "search slot was released after NOT_FOUND (no leaked registry entry)");
}

/* ==== Test 6: wildcard Fsfirst("*") + repeated Fsnext must still walk the
 * ENTIRE realistic 51-entry directory correctly -- the already-working
 * control path, must remain unaffected by the fix. ==== */
static void test_wildcard_walk_unaffected(void)
{
    printf("Test 6: wildcard Fsfirst(\"*\")+Fsnext walks all 51 entries of the realistic directory\n");
    test_setup();
    dir_reset();
    dir_add(".");
    dir_add("..");
    for (int i = 0; i < 51; i++)
    {
        char name[14];
        snprintf(name, sizeof(name), "W%05d.PF", i);
        dir_add(name);
    }

    uint32_t ndta = 0x1007;
    SidetnfsAtariDirEntry entry;
    int found_count = 0;
    bool saw_target = false;
    SidetnfsDirSearchResult r = sidetnfs_tnfs_dta_start(ndta, TEST_SLOT, "/WILDDIR", "*", 0x37, &entry);
    while (r == SIDETNFS_DIR_SEARCH_FOUND)
    {
        found_count++;
        if (strcmp(entry.name, "W00030.PF") == 0)
        {
            saw_target = true;
        }
        r = sidetnfs_tnfs_dta_next(ndta, &entry);
    }
    CHECK(r == SIDETNFS_DIR_SEARCH_NOT_FOUND, "wildcard walk ends with a clean NOT_FOUND (real EOF), not ERROR");
    CHECK(found_count == 51, "wildcard walk visits exactly all 51 real entries");
    CHECK(saw_target, "wildcard walk visits the entry at the same raw position the exact-lookup bug affects");
}

int main(void)
{
    test_first_entry();
    test_at_round_cap_boundary();
    test_one_round_past_cap_boundary();
    test_realistic_epslq_case();
    test_nonexistent_entry();
    test_wildcard_walk_unaffected();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
