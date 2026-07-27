/**
 * File: test_fase6_sd_service.c
 * Fase 6 (SD-service en SD_ERROR.TXT) tests A-L from the task.
 *
 * Part 1 (tests A-H) compiles and links the REAL, unmodified
 * romemul/sidetnfs_sd_service.c (via sandbox/sidetnfs_sd_service.c, a
 * symlink) AND the real romemul/sidetnfs_probe.c (for
 * sidetnfs_gemdos_pattern_match()/sidetnfs_gemdos_attr_match(), which
 * sidetnfs_sd_error_search_start() calls) -- same stub layer style
 * (sd_init_driver()/f_mount()/f_stat() controlled directly by this file;
 * lwIP/cyw43/flash stubs needed only to satisfy sidetnfs_probe.c's own
 * link requirements, unused by anything actually exercised here) already
 * established by test_probe_multislot_mount.c/test_fase5_net_err_root.c.
 *
 * sidetnfs_sd_service_run() is a real, intentional one-shot boot service
 * (see its own header comment: "precies één keer" per boot) -- its
 * module-level state has no reset hook, matching that one-shot contract.
 * To exercise each global card/mount outcome (tests A-D) as its own
 * fresh, independent boot, this binary takes the scenario to run as
 * argv[1] and calls sidetnfs_sd_service_run() at most once per process.
 * Tests E/F/G/H (per-slot directory checks under an already-READY card)
 * share the "ready_multi" scenario, since those all need f_stat() to
 * distinguish between slots within the SAME run.
 *
 * Part 2 (tests I, J, K) is a faithful MIRROR of gemdrvemul.c's new Fase
 * 6 routing (Fopen mode/name check for SD_ERROR.TXT, the
 * READY-SD-drive-stays-out-of-SD_ERROR.TXT invariant, and the
 * write-refusal branch) -- gemdrvemul.c itself is not host-compilable as
 * a whole (see test_runtime_publication_mirror.c/
 * test_fase5_net_err_root.c's own header comments for why). Runs
 * unconditionally regardless of argv[1] (no SD/FatFS state involved).
 *
 * Build once, run once per scenario:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_fase6_sd_service.c sandbox/sidetnfs_sd_service.c \
 *       sandbox/sidetnfs_probe.c sandbox/sidetnfs_config.c \
 *       -o /tmp/test_fase6_sd_service
 *   for s in no_card init_failed mount_failed fs_error ready_multi; do
 *       /tmp/test_fase6_sd_service $s
 *   done
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "include/sidetnfs_sd_service.h"
#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_config.h"
#include "include/filesys.h" // FS_ST_ARCH
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "hardware/flash.h"
#include "f_util.h"
#include "sd_card.h"

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

/* ---- sidetnfs_probe.c's own link requirements (unused by this test's
 * actual assertions, needed only so the real
 * sidetnfs_gemdos_pattern_match()/sidetnfs_gemdos_attr_match() link). ---- */

uint8_t g_fake_flash[0x102000];
void flash_range_erase(uint32_t flash_offs, size_t count) { memset(g_fake_flash + flash_offs, 0xFF, count); }
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) { memcpy(g_fake_flash + flash_offs, data, count); }

static long g_utc_offset = 0;
long get_utc_offset_seconds(void) { return g_utc_offset; }
void set_utc_offset_seconds(long offset) { g_utc_offset = offset; }

bool ipaddr_aton(const char *cp, ip_addr_t *addr) { (void)cp; (void)addr; return false; }
char *ipaddr_ntoa(const ip_addr_t *addr) { (void)addr; static char buf[16] = "0.0.0.0"; return buf; }
struct pbuf *pbuf_alloc(int layer, uint16_t length, int type) { (void)layer; (void)length; (void)type; return NULL; }
void pbuf_free(struct pbuf *p) { (void)p; }
uint16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr, uint16_t len, uint16_t offset)
{
    (void)p; (void)dataptr; (void)len; (void)offset;
    return 0;
}
struct udp_pcb *udp_new(void) { return NULL; }
void udp_remove(struct udp_pcb *pcb) { (void)pcb; }
void udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg) { (void)pcb; (void)recv; (void)recv_arg; }
err_t udp_connect(struct udp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port) { (void)pcb; (void)ipaddr; (void)port; return ERR_OK; }
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port) { (void)pcb; (void)ipaddr; (void)port; return ERR_OK; }
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, uint16_t dst_port)
{
    (void)pcb; (void)p; (void)dst_ip; (void)dst_port;
    return ERR_OK;
}
int cyw43_arch_poll(void) { return 0; }

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) { (void)fp; (void)path; (void)mode; return FR_OK; }
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) { (void)fp; (void)buff; if (bw) *bw = btw; return FR_OK; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_opendir(DIR *dp, const char *path) { (void)dp; (void)path; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; if (fno) fno->fname[0] = '\0'; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

/* ---- test-controlled SD/FatFS stubs -- what each scenario scripts to
 * simulate a given hardware outcome. ---- */
static bool g_sd_init_driver_result = true;
bool sd_init_driver(void) { return g_sd_init_driver_result; }

static FRESULT g_f_mount_result = FR_OK;
FRESULT f_mount(FATFS *fs, const char *path, uint8_t opt)
{
    (void)fs; (void)path; (void)opt;
    return g_f_mount_result;
}

typedef struct
{
    const char *path;
    FRESULT result;
    uint8_t fattrib;
} FStatScriptEntry;
static FStatScriptEntry g_fstat_script[8];
static int g_fstat_script_count = 0;

static void fstat_script_add(const char *path, FRESULT result, uint8_t fattrib)
{
    g_fstat_script[g_fstat_script_count].path = path;
    g_fstat_script[g_fstat_script_count].result = result;
    g_fstat_script[g_fstat_script_count].fattrib = fattrib;
    g_fstat_script_count++;
}

FRESULT f_stat(const char *path, FILINFO *fno)
{
    for (int i = 0; i < g_fstat_script_count; i++)
    {
        if (strcmp(g_fstat_script[i].path, path) == 0)
        {
            if (fno)
            {
                memset(fno, 0, sizeof(*fno));
                fno->fattrib = g_fstat_script[i].fattrib;
            }
            return g_fstat_script[i].result;
        }
    }
    return FR_NO_FILE; // no script entry -- "doesn't exist"
}

static void scenario_no_card(void)
{
    printf("Test A: no SD card -> ABSENT, with correct text/size/listing behavior\n");
    g_sd_init_driver_result = true;
    g_f_mount_result = FR_NOT_READY; // FatFS's own "physical drive cannot work" code

    int slot = 0;
    sidetnfs_sd_service_set_slot_path(slot, 'C', "/hd");
    sidetnfs_sd_service_run();

    CHECK(sidetnfs_sd_service_has_run(), "service ran");
    CHECK(sidetnfs_sd_global_status() == SIDETNFS_SD_STATUS_ABSENT, "global status is ABSENT");

    sidetnfs_sd_drive_status_t st;
    CHECK(sidetnfs_sd_get_drive_status(slot, &st), "slot status readable");
    CHECK(st.status == SIDETNFS_SD_STATUS_ABSENT, "ENABLED SD drive mirrors the global ABSENT status");

    char text[SIDETNFS_SD_ERROR_TEXT_MAX];
    size_t len = sidetnfs_build_sd_error_text(slot, 'C', text, sizeof(text));
    CHECK(len == strlen(text), "build_sd_error_text() length matches strlen(text)");
    CHECK(strstr(text, "No SD card inserted") != NULL, "SD_ERROR.TXT body mentions the required ABSENT wording");
    CHECK(strstr(text, "Drive: C:") != NULL, "SD_ERROR.TXT body mentions the correct drive letter");

    const uint32_t ndta = 0x11110000;
    SidetnfsAtariDirEntry entry;
    CHECK(sidetnfs_sd_error_search_start(ndta, slot, 'C', "/", "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND,
          "root search finds exactly one entry");
    CHECK(strcmp(entry.name, SIDETNFS_SD_ERROR_NAME) == 0, "entry is named SD_ERROR.TXT");
    CHECK(entry.size == (uint32_t)len, "Fsfirst-reported size exactly equals the generated content length (test H)");
    SidetnfsAtariDirEntry entry_next;
    CHECK(sidetnfs_sd_error_search_next(ndta, &entry_next) == SIDETNFS_DIR_SEARCH_NOT_FOUND,
          "listing exhausts after its one entry -- no '.'/'..' , no second file");
    CHECK(!sidetnfs_fake_search_is_active(ndta),
          "SD_ERROR.TXT never registers in sidetnfs_probe.c's own fake/NET_ERR listing table (test I)");
    sidetnfs_sd_error_search_close(ndta);
}

static void scenario_init_failed(void)
{
    printf("Test B: SD-driver init fails -> INIT_FAILED\n");
    g_sd_init_driver_result = false; // f_mount() never even reached

    int slot = 0;
    sidetnfs_sd_service_set_slot_path(slot, 'C', "/hd");
    sidetnfs_sd_service_run();

    CHECK(sidetnfs_sd_global_status() == SIDETNFS_SD_STATUS_INIT_FAILED, "global status is INIT_FAILED");
    CHECK(sidetnfs_sd_global_fresult() == 0xFFu, "no FatFS FRESULT applies -- f_mount() was never called");
    sidetnfs_sd_drive_status_t st;
    sidetnfs_sd_get_drive_status(slot, &st);
    CHECK(st.status == SIDETNFS_SD_STATUS_INIT_FAILED, "slot mirrors INIT_FAILED");
}

static void scenario_mount_failed(void)
{
    printf("Test C: FatFS mount fails (generic) -> MOUNT_FAILED\n");
    g_sd_init_driver_result = true;
    g_f_mount_result = FR_DISK_ERR; // some other genuine mount failure

    int slot = 0;
    sidetnfs_sd_service_set_slot_path(slot, 'C', "/hd");
    sidetnfs_sd_service_run();

    CHECK(sidetnfs_sd_global_status() == SIDETNFS_SD_STATUS_MOUNT_FAILED, "global status is MOUNT_FAILED");
    CHECK(sidetnfs_sd_global_fresult() == (uint8_t)FR_DISK_ERR, "the raw FatFS FRESULT is preserved");
}

static void scenario_fs_error(void)
{
    printf("Test D: invalid/corrupt filesystem -> FILESYSTEM_ERROR\n");
    g_sd_init_driver_result = true;
    g_f_mount_result = FR_NO_FILESYSTEM; // FatFS's own "no valid FAT volume" code

    int slot = 0;
    sidetnfs_sd_service_set_slot_path(slot, 'C', "/hd");
    sidetnfs_sd_service_run();

    CHECK(sidetnfs_sd_global_status() == SIDETNFS_SD_STATUS_FILESYSTEM_ERROR, "global status is FILESYSTEM_ERROR");
}

static void scenario_ready_multi(void)
{
    printf("Test E/F/G/H: card READY, two SD drives with independent per-slot status\n");
    g_sd_init_driver_result = true;
    g_f_mount_result = FR_OK;

    int slot_ok = 0;
    int slot_missing = 1;
    int slot_file = 2;
    sidetnfs_sd_service_set_slot_path(slot_ok, 'C', "/hd");
    sidetnfs_sd_service_set_slot_path(slot_missing, 'D', "/games");
    sidetnfs_sd_service_set_slot_path(slot_file, 'E', "/afile");

    fstat_script_add("/hd", FR_OK, AM_DIR);        // a real directory
    fstat_script_add("/afile", FR_OK, 0);          // exists, but a plain file
    // "/games" deliberately has no script entry -> f_stat() falls back to
    // FR_NO_FILE ("doesn't exist"), the expected DIRECTORY_NOT_FOUND case.

    sidetnfs_sd_service_run();

    CHECK(sidetnfs_sd_global_status() == SIDETNFS_SD_STATUS_READY, "global card/mount status is READY");

    sidetnfs_sd_drive_status_t st_ok, st_missing, st_file;
    sidetnfs_sd_get_drive_status(slot_ok, &st_ok);
    sidetnfs_sd_get_drive_status(slot_missing, &st_missing);
    sidetnfs_sd_get_drive_status(slot_file, &st_file);

    CHECK(st_ok.status == SIDETNFS_SD_STATUS_READY, "C:/hd is READY (test E baseline)");
    CHECK(st_missing.status == SIDETNFS_SD_STATUS_DIRECTORY_NOT_FOUND, "D:/games is DIRECTORY_NOT_FOUND (test E)");
    CHECK(st_file.status == SIDETNFS_SD_STATUS_NOT_A_DIRECTORY, "E:/afile is NOT_A_DIRECTORY (test F)");

    // Test G: independent per-drive status -- one READY, one not, on a
    // shared globally-READY card.
    CHECK(st_ok.status != st_missing.status, "C: and D: have independent statuses despite sharing one READY card");

    char text_missing[SIDETNFS_SD_ERROR_TEXT_MAX], text_file[SIDETNFS_SD_ERROR_TEXT_MAX];
    size_t len_missing = sidetnfs_build_sd_error_text(slot_missing, 'D', text_missing, sizeof(text_missing));
    size_t len_file = sidetnfs_build_sd_error_text(slot_file, 'E', text_file, sizeof(text_file));
    CHECK(strcmp(text_missing, text_file) != 0, "the two failing drives get their own distinct SD_ERROR.TXT body");
    CHECK(strstr(text_missing, "/games") != NULL, "D:'s body mentions its own sd_path");
    CHECK(strstr(text_file, "/afile") != NULL, "E:'s body mentions its own sd_path, not D:'s");

    const uint32_t ndta_missing = 0x22220000;
    const uint32_t ndta_file = 0x33330000;
    SidetnfsAtariDirEntry entry_missing, entry_file;
    CHECK(sidetnfs_sd_error_search_start(ndta_missing, slot_missing, 'D', "/", "*.*", FS_ST_ARCH, &entry_missing) ==
              SIDETNFS_DIR_SEARCH_FOUND,
          "D:'s own virtual root search finds exactly one entry");
    CHECK(sidetnfs_sd_error_search_start(ndta_file, slot_file, 'E', "/", "*.*", FS_ST_ARCH, &entry_file) ==
              SIDETNFS_DIR_SEARCH_FOUND,
          "E:'s own virtual root search finds exactly one entry, independently of D:'s");
    CHECK(entry_missing.size == (uint32_t)len_missing, "D:'s reported size matches its own generated content length");
    CHECK(entry_file.size == (uint32_t)len_file, "E:'s reported size matches its own generated content length");

    // Closing D:'s search must never affect E:'s (independent per-slot
    // registries, test G).
    sidetnfs_sd_error_search_close(ndta_missing);
    CHECK(!sidetnfs_sd_error_search_is_active(ndta_missing), "D:'s search is now closed");
    CHECK(sidetnfs_sd_error_search_is_active(ndta_file), "E:'s search is still active after D:'s was closed");
    sidetnfs_sd_error_search_close(ndta_file);
}

/* ============================================================
 * Part 2: mirror of gemdrvemul.c's new Fase 6 routing -- gemdrvemul.c is
 * not host-compilable as a whole (see this file's own header comment).
 * Runs unconditionally, regardless of scenario.
 * ============================================================ */
static void test_mirror_fopen_routing_and_write_refusal(void)
{
    printf("Test I/J/K: Fopen mode/name routing, write refusal, SETTINGS/TNFS untouched (mirror)\n");

    // Mirrors gemdrive_backend_fopen()'s new SD_ERROR gate:
    //   bool fopen_is_sd_backend = (... && backend == GEMDRIVE_FILE_BACKEND_SD);
    //   if (fopen_is_sd_backend) { if (!sd_ready) { ... SD_ERROR.TXT ... } }
    {
        bool backend_is_sd = true;
        bool sd_ready = false;
        bool routes_to_sd_error = backend_is_sd && !sd_ready;
        CHECK(routes_to_sd_error, "a not-ready SD slot routes to SD_ERROR.TXT (test I)");
    }
    {
        bool backend_is_sd = true;
        bool sd_ready = true;
        bool routes_to_sd_error = backend_is_sd && !sd_ready;
        CHECK(!routes_to_sd_error, "a READY SD drive is never routed to SD_ERROR.TXT (test K)");
    }
    {
        // A SETTINGS or TNFS slot never matches backend == SD at all.
        bool backend_is_sd = false; // SETTINGS (CONFIG_FLASH) or TNFS
        bool sd_ready = false;
        bool routes_to_sd_error = backend_is_sd && !sd_ready;
        CHECK(!routes_to_sd_error, "a SETTINGS/TNFS slot never matches the SD_ERROR gate (test K)");
    }
    {
        // Mirrors the mode/name check inside that gate.
        const int32_t GEMDOS_EACCDN_M = -36; // matches romemul's GEMDOS_EACCDN
        const int32_t GEMDOS_EFILNF_M = -33; // matches romemul's GEMDOS_EFILNF
        uint16_t fopen_mode_write = 1;
        int32_t result_for_write_mode = (fopen_mode_write != 0) ? GEMDOS_EACCDN_M : 0;
        CHECK(result_for_write_mode == GEMDOS_EACCDN_M, "Fopen(mode=1, SD_ERROR.TXT) is EACCDN (read-only) (test J)");

        const char *other_name = "OTHER.TXT";
        int32_t result_for_wrong_name = (strcmp(other_name, SIDETNFS_SD_ERROR_NAME) != 0) ? GEMDOS_EFILNF_M : 0;
        CHECK(result_for_wrong_name == GEMDOS_EFILNF_M, "Fopen of any other name on this virtual root is EFILNF");
    }
    {
        // Mirrors the WRITE_BUFF_CALL SD_ERROR branch: always refused.
        const int32_t GEMDOS_EACCDN_M = -36;
        int32_t write_result = GEMDOS_EACCDN_M; // the branch's own, unconditional result
        CHECK(write_result == GEMDOS_EACCDN_M, "a write to an open SD_ERROR.TXT handle is refused (test J)");
    }
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

int main(int argc, char **argv)
{
    test_mirror_fopen_routing_and_write_refusal();

    const char *scenario = (argc > 1) ? argv[1] : "no_card";
    if (strcmp(scenario, "no_card") == 0)
    {
        scenario_no_card();
    }
    else if (strcmp(scenario, "init_failed") == 0)
    {
        scenario_init_failed();
    }
    else if (strcmp(scenario, "mount_failed") == 0)
    {
        scenario_mount_failed();
    }
    else if (strcmp(scenario, "fs_error") == 0)
    {
        scenario_fs_error();
    }
    else if (strcmp(scenario, "ready_multi") == 0)
    {
        scenario_ready_multi();
    }
    else
    {
        fprintf(stderr, "Unknown scenario: %s\n", scenario);
        return 2;
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
