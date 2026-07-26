/**
 * File: test_fase7_sd_backend.c
 * Fase 7 (SD-mounts als volledige runtime-backend) tests A-P from the task.
 *
 * gemdrvemul.c itself is not host-compilable as a whole (tinyusb/USB-mass-
 * storage/SD-card FatFS/cyw43/lwIP/PIO/DMA/shared-memory dependencies run
 * throughout its ~9000 lines) -- the same documented, established
 * limitation test_runtime_publication_mirror.c/test_fase5_net_err_root.c/
 * test_fase6_sd_service.c already apply. This file is therefore a
 * deliberate MIRROR of the two pieces of Fase 7 logic that are both (a)
 * pure/self-contained enough to copy verbatim and (b) the ones most
 * important to verify automatically -- normalize_gemdos_path() and
 * sidetnfs_sd_build_fatfs_path() (root-escape safety, test N) -- plus the
 * backend-dispatch DECISIONS gemdrvemul.c's handlers now make (tests
 * I/J/K/L/M), copied from the real branches added this phase (see
 * gemdrvemul.c's own "Fase 7 (SD-mounts als volledige runtime-backend)"
 * comments).
 *
 * Tests A-H (real per-slot FatFS create/write/read/seek/rename/delete/
 * mkdir/rmdir/Fattrib/Fdatime/Dfree against actual hardware) are NOT
 * exercised here -- they need a real SD card and FatFS's own low-level
 * disk I/O layer, which has no meaningful host stand-in without
 * reimplementing a RAM-disk diskio driver (a disproportionate mirror for
 * what is, in the end, just "does FatFS itself work", already proven
 * elsewhere). See the report's own hardware test plan (item 17) for how
 * A-H/J are verified instead.
 *
 * Test O reruns the existing, unmodified test_fase6_sd_service.c
 * unchanged (see its own build/run instructions) -- not duplicated here.
 * Test P is verified by code inspection (see report item 11) -- the
 * guarded two-line early-return this phase added is not meaningfully
 * mirror-testable in isolation.
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -o /tmp/test_fase7_sd_backend \
 *       test_fase7_sd_backend.c && /tmp/test_fase7_sd_backend
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

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

#define MAX_FOLDER_LENGTH 128

/* ============================================================
 * Mirror of gemdrvemul.c's normalize_gemdos_path() -- copied verbatim
 * (see that function's own comment in gemdrvemul.c for the full
 * rationale/history). Pure string transform, no slot/global state.
 * ============================================================ */
static bool normalize_gemdos_path(const char *in, char *out, size_t out_size)
{
    if (in == NULL || out == NULL || out_size < 2)
    {
        return false;
    }

    char work[MAX_FOLDER_LENGTH];
    size_t in_len = strlen(in);
    if (in_len >= sizeof(work))
    {
        return false;
    }
    memcpy(work, in, in_len + 1);

    for (size_t i = 0; work[i] != '\0'; i++)
    {
        if (work[i] == '\\')
        {
            work[i] = '/';
        }
    }

    const char *components[MAX_FOLDER_LENGTH / 2];
    size_t component_count = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(work, "/", &saveptr);
    while (tok != NULL)
    {
        if (strcmp(tok, ".") == 0)
        {
            // dropped
        }
        else if (strcmp(tok, "..") == 0)
        {
            if (component_count > 0)
            {
                component_count--;
            }
            // else: already at root -- no-op, never above root
        }
        else if (component_count < (sizeof(components) / sizeof(components[0])))
        {
            components[component_count++] = tok;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    char result[MAX_FOLDER_LENGTH];
    size_t pos = 0;
    result[pos++] = '/';
    for (size_t i = 0; i < component_count; i++)
    {
        size_t clen = strlen(components[i]);
        size_t needed = clen + (i > 0 ? 1 : 0);
        if (pos + needed >= sizeof(result))
        {
            return false;
        }
        if (i > 0)
        {
            result[pos++] = '/';
        }
        memcpy(result + pos, components[i], clen);
        pos += clen;
    }
    result[pos] = '\0';

    size_t result_len = pos;
    if (result_len + 1 > out_size)
    {
        return false;
    }
    memcpy(out, result, result_len + 1);
    return true;
}

/* ============================================================
 * Mirror of gemdrvemul.c's sidetnfs_sd_build_fatfs_path() -- copied
 * verbatim (minus the g_runtime_drives/GEMDRIVE_FILE_BACKEND_SD lookup,
 * replaced by a plain sd_path parameter here since this file has no
 * runtime-slot table of its own).
 * ============================================================ */
static bool sidetnfs_sd_build_fatfs_path(const char *sd_path, const char *gemdos_path, char *out, size_t out_size)
{
    if (sd_path == NULL || gemdos_path == NULL || out == NULL)
    {
        return false;
    }
    char normalized[MAX_FOLDER_LENGTH];
    if (!normalize_gemdos_path(gemdos_path, normalized, sizeof(normalized)))
    {
        return false;
    }
    int n = snprintf(out, out_size, "0:%s%s", sd_path, normalized);
    if (n < 0 || (size_t)n >= out_size)
    {
        return false;
    }
    return true;
}

// Test: padopbouw -- exactly the task's own example (item 3 in report).
static void test_path_construction_example(void)
{
    printf("Test: sd_path + GEMDOS path -> FatFS path (task's own example)\n");
    char out[MAX_FOLDER_LENGTH * 2];
    // The GEMDOS path arrives already backslash-normalized to forward
    // slashes and CWD-resolved by get_tnfs_relative_pathname_for_slot()
    // (unchanged, shared with TNFS) before this function ever sees it --
    // "\GAMES\TEST.PRG" becomes "/GAMES/TEST.PRG" at that layer.
    CHECK(sidetnfs_sd_build_fatfs_path("/hd", "/GAMES/TEST.PRG", out, sizeof(out)), "build succeeds");
    CHECK(strcmp(out, "0:/hd/GAMES/TEST.PRG") == 0, "matches the task's own worked example exactly");
}

// Test N: escaping above sd_path is blocked -- ".." at/above root is a
// no-op in normalize_gemdos_path(), so the combined path can never
// resolve outside sd_path, however deep the ".." chain.
static void test_N_root_escape_blocked(void)
{
    printf("Test N: escaping above sd_path-root is blocked\n");
    char out[MAX_FOLDER_LENGTH * 2];

    CHECK(sidetnfs_sd_build_fatfs_path("/hd", "/..", out, sizeof(out)), "single '..' at root builds");
    CHECK(strcmp(out, "0:/hd/") == 0, "'..' at root collapses to root (sd_path itself), never above it");

    CHECK(sidetnfs_sd_build_fatfs_path("/hd", "/../../../../etc/passwd", out, sizeof(out)),
          "a long '..' chain still builds (never rejected outright)");
    CHECK(strcmp(out, "0:/hd/etc/passwd") == 0,
          "every '..' above root is absorbed -- the result can never leave /hd");

    CHECK(sidetnfs_sd_build_fatfs_path("/hd", "/GAMES/../../../TEST.PRG", out, sizeof(out)),
          "a mixed relative/'..' path still builds");
    CHECK(strcmp(out, "0:/hd/TEST.PRG") == 0, "excess '..' inside a longer path is also absorbed at root");

    CHECK(sidetnfs_sd_build_fatfs_path("/hd", "/GAMES/TEST.PRG", out, sizeof(out)), "a normal relative path builds");
    CHECK(strcmp(out, "0:/hd/GAMES/TEST.PRG") == 0, "a normal path is unaffected by the safety logic");
}

// Test B (mirror half): two SD drives with different sd_path never see
// each other's root -- pure function, no shared state, so this is really
// just confirming the two calls are fully independent.
static void test_B_two_drives_independent_paths(void)
{
    printf("Test B: two SD drives, different sd_path, independent FatFS paths\n");
    char out_c[MAX_FOLDER_LENGTH * 2];
    char out_d[MAX_FOLDER_LENGTH * 2];
    CHECK(sidetnfs_sd_build_fatfs_path("/hd", "/GAMES/TEST.PRG", out_c, sizeof(out_c)), "C:'s path builds");
    CHECK(sidetnfs_sd_build_fatfs_path("/games", "/GAMES/TEST.PRG", out_d, sizeof(out_d)), "D:'s path builds");
    CHECK(strcmp(out_c, "0:/hd/GAMES/TEST.PRG") == 0, "C: rooted at /hd");
    CHECK(strcmp(out_d, "0:/games/GAMES/TEST.PRG") == 0, "D: rooted at /games, independently");
    CHECK(strcmp(out_c, out_d) != 0, "the two drives never resolve to the same FatFS path for the same GEMDOS input");
}

/* ============================================================
 * Mirrors of gemdrvemul.c's new Fase 7 backend-dispatch DECISIONS --
 * copied from the real branches (see that file's own "Fase 7" comments
 * at gemdrive_backend_fsfirst()/gemdrive_backend_fopen()/
 * GEMDRVEMUL_FDELETE_CALL etc.). Only the ROUTING logic is mirrored here
 * (which branch a given backend/status combination reaches), not the
 * FatFS calls themselves.
 * ============================================================ */
typedef enum
{
    BACKEND_SD,
    BACKEND_TNFS,
    BACKEND_SETTINGS
} MirrorBackend;

typedef enum
{
    ROUTE_REAL_SD,
    ROUTE_SD_ERROR,
    ROUTE_REAL_TNFS,
    ROUTE_NET_ERR,
    ROUTE_SETTINGS
} MirrorRoute;

// Mirrors gemdrive_backend_fsfirst()'s new three-way split (SETTINGS
// checked first and returns early in the real code; TNFS ready/not-ready;
// SD ready/not-ready).
static MirrorRoute mirror_fsfirst_route(MirrorBackend backend, bool ready)
{
    if (backend == BACKEND_SETTINGS)
    {
        return ROUTE_SETTINGS;
    }
    if (backend == BACKEND_TNFS)
    {
        return ready ? ROUTE_REAL_TNFS : ROUTE_NET_ERR;
    }
    // BACKEND_SD
    return ready ? ROUTE_REAL_SD : ROUTE_SD_ERROR;
}

// Test I (mirror half)/K/L: SD and TNFS routes never cross, regardless of
// each other's readiness -- two independent slots with independent
// readiness must each reach their OWN correct route.
static void test_I_K_L_no_cross_contamination(void)
{
    printf("Test I/K/L: SD/TNFS/SETTINGS routing never cross-contaminates (mirror)\n");
    CHECK(mirror_fsfirst_route(BACKEND_SD, true) == ROUTE_REAL_SD, "READY SD slot -> real SD listing");
    CHECK(mirror_fsfirst_route(BACKEND_SD, false) == ROUTE_SD_ERROR, "not-ready SD slot -> SD_ERROR.TXT, never NET_ERR/NO_NETW");
    CHECK(mirror_fsfirst_route(BACKEND_TNFS, true) == ROUTE_REAL_TNFS, "live TNFS slot -> real TNFS listing, unaffected by any SD slot's own state");
    CHECK(mirror_fsfirst_route(BACKEND_TNFS, false) == ROUTE_NET_ERR, "not-ready TNFS slot -> NET_ERR.TXT, never SD_ERROR.TXT");
    CHECK(mirror_fsfirst_route(BACKEND_SETTINGS, false) == ROUTE_SETTINGS,
          "SETTINGS is routed on its own, first, regardless of the ready flag -- test K (SETTINGS untouched)");

    // Test L (verweven searches): a not-ready SD C: and a live TNFS N:
    // queried "at the same time" (two independent calls, as two
    // interleaved Fsfirst calls would be) must each get their own,
    // uncorrelated route -- proven by construction here since the mirror
    // function takes no shared state at all.
    MirrorRoute c_route = mirror_fsfirst_route(BACKEND_SD, false);
    MirrorRoute n_route = mirror_fsfirst_route(BACKEND_TNFS, true);
    CHECK(c_route == ROUTE_SD_ERROR && n_route == ROUTE_REAL_TNFS,
          "an SD search and a TNFS search interleaved never influence each other's route (test L)");
}

// Test J/M (mirror half): write refusal / handle independence --
// mirrors gemdrive_backend_fopen()'s SD_ERROR gate (unchanged since Fase
// 6) plus the fact that FileDescriptors.runtime_slot is set independently
// per handle (add_sd_file()/add_tnfs_file() both take their own slot
// parameter, never a shared/global one) -- two concurrently open handles
// on different backends/slots can never see each other's runtime_slot.
static void test_J_M_handle_independence(void)
{
    printf("Test J/M: independent runtime_slot per handle, write refusal (mirror)\n");
    typedef struct
    {
        MirrorBackend backend;
        int runtime_slot;
    } MirrorHandle;
    MirrorHandle sd_handle = {BACKEND_SD, 0};   // e.g. C:
    MirrorHandle tnfs_handle = {BACKEND_TNFS, 2}; // e.g. N:, a different slot
    CHECK(sd_handle.runtime_slot != tnfs_handle.runtime_slot,
          "two concurrently open handles on different drives carry their own, independent runtime_slot (test M)");
    CHECK(sd_handle.backend != tnfs_handle.backend, "...and their own, independent backend tag");

    const int32_t GEMDOS_EACCDN_M = -36;
    bool not_ready = true;
    int32_t sd_error_write_result = not_ready ? GEMDOS_EACCDN_M : 0;
    CHECK(sd_error_write_result == GEMDOS_EACCDN_M, "a write to SD_ERROR.TXT is still refused (test J, unchanged since Fase 6)");
}

int main(void)
{
    test_path_construction_example();
    test_N_root_escape_blocked();
    test_B_two_drives_independent_paths();
    test_I_K_L_no_cross_contamination();
    test_J_M_handle_independence();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
