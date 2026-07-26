/**
 * File: test_runtime_publication_mirror.c
 * Fase 13 (runtime-drive-publication audit follow-up) Tests A-E --
 * gemdrvemul.c is not host-compilable as a whole (tinyusb/USB-mass-
 * storage/SD-card FatFS/cyw43/lwIP dependencies run throughout its
 * ~9000 lines, well beyond sidetnfs_runtime_drives_init() itself), so
 * unlike test_probe_multislot_mount.c (which links and exercises the
 * REAL, unmodified sidetnfs_probe.c), this file is a faithful mirror of
 * sidetnfs_runtime_drives_init()'s new publication logic only -- the
 * exact three-step rule documented in that function's own comment in
 * romemul/gemdrvemul.c (search for "Fase 13 (runtime-drive-publication
 * audit follow-up)"):
 *   1. active_drive_letter resolves to slot 0 (SETTINGS directly if it
 *      IS the settings letter; otherwise the real ENABLED record with
 *      that letter, if one exists; otherwise nothing yet).
 *   2. every other ENABLED ordinary config slot (0..7, skipping whatever
 *      step 1 already placed), ascending, backend derived from .type.
 *   3. SETTINGS appended last, unless step 1 already placed it at slot 0.
 * This mirror must be kept in sync by hand with gemdrvemul.c -- the same
 * documented limitation this codebase's established convention already
 * applies to any gemdrvemul.c/network.c host test (see
 * test_bootflow_settings_source.c's own header comment from an earlier
 * phase).
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_runtime_publication_mirror.c sandbox/sidetnfs_config.c \
 *       -o /tmp/test_runtime_publication_mirror && \
 *       /tmp/test_runtime_publication_mirror
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "include/sidetnfs_config.h"
#include "hardware/flash.h"

#define GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES (SIDETNFS_MAX_DRIVES + 1)

typedef enum
{
    GEMDRIVE_FILE_BACKEND_SD = 0,
    GEMDRIVE_FILE_BACKEND_TNFS,
    GEMDRIVE_FILE_BACKEND_CONFIG_FLASH
} GemdriveFileBackend;

typedef struct
{
    bool valid;
    uint32_t drive_number;
    sidetnfs_drive_config_t config;
    GemdriveFileBackend backend;
    int config_slot;
} sidetnfs_runtime_drive_t;

static sidetnfs_runtime_drive_t g_runtime_drives[GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES];

/* ---- fake flash, real sidetnfs_config.c linked in ---- */
uint8_t g_fake_flash[0x102000];
void flash_range_erase(uint32_t flash_offs, size_t count) { memset(g_fake_flash + flash_offs, 0xFF, count); }
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) { memcpy(g_fake_flash + flash_offs, data, count); }

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

/* ---- mirror of sidetnfs_config_find_drive_by_letter() (gemdrvemul.c) ---- */
static bool find_drive_by_letter(char letter, sidetnfs_drive_config_t *out, int *out_config_slot)
{
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_drive_config_t cfg;
        if (sidetnfs_config_get_drive(i, &cfg) != SIDETNFS_CONFIG_STATUS_OK)
        {
            continue;
        }
        if (!sidetnfs_drive_slot_is_enabled(&cfg))
        {
            continue;
        }
        if (cfg.drive_letter == (uint8_t)letter)
        {
            *out = cfg;
            if (out_config_slot != NULL)
            {
                *out_config_slot = (int)i;
            }
            return true;
        }
    }
    return false;
}

/* ---- mirror of sidetnfs_runtime_backend_for_config_type() (gemdrvemul.c) ---- */
static GemdriveFileBackend backend_for_type(uint8_t type)
{
    return (type == SIDETNFS_DRIVE_SD) ? GEMDRIVE_FILE_BACKEND_SD : GEMDRIVE_FILE_BACKEND_TNFS;
}

/* ---- mirror of sidetnfs_runtime_drives_init()'s three-step publication
 * rule, minus shared-memory publish / DPRINTF / SIDETNFS_ENABLE_DIAG_UART
 * logging / SIDETNFS_UART_DIAG_DUMP_ON_SELECT snapshot bookkeeping (none
 * of that affects g_runtime_drives[] itself). ---- */
static void runtime_drives_init_mirror(char active_drive_letter, uint32_t active_drive_number)
{
    for (int i = 0; i < GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES; i++)
    {
        g_runtime_drives[i].valid = false;
        g_runtime_drives[i].drive_number = 0xFFFFFFFF;
        g_runtime_drives[i].config_slot = -1;
        memset(&g_runtime_drives[i].config, 0, sizeof(g_runtime_drives[i].config));
    }

    uint8_t settings_letter = sidetnfs_config_get_config_drive_letter();
    if (settings_letter == 0)
    {
        settings_letter = 'S';
    }

    bool active_is_settings = (active_drive_letter != '\0') && ((uint8_t)active_drive_letter == settings_letter);
    int active_config_slot = -1;
    int next_runtime_slot = 0;
    bool settings_placed = false;

    if (active_is_settings)
    {
        g_runtime_drives[0].valid = true;
        g_runtime_drives[0].drive_number = (uint32_t)(settings_letter - 'A');
        g_runtime_drives[0].backend = GEMDRIVE_FILE_BACKEND_CONFIG_FLASH;
        g_runtime_drives[0].config.state = SIDETNFS_DRIVE_SLOT_ENABLED;
        g_runtime_drives[0].config.drive_letter = settings_letter;
        g_runtime_drives[0].config.type = SIDETNFS_DRIVE_SD;
        g_runtime_drives[0].config_slot = -1;
        settings_placed = true;
        next_runtime_slot = 1;
    }
    else if (active_drive_letter >= 'A' && active_drive_letter <= 'Z')
    {
        sidetnfs_drive_config_t active_cfg;
        int found_slot = -1;
        if (find_drive_by_letter(active_drive_letter, &active_cfg, &found_slot))
        {
            g_runtime_drives[0].valid = true;
            g_runtime_drives[0].drive_number = active_drive_number;
            g_runtime_drives[0].config = active_cfg;
            g_runtime_drives[0].backend = backend_for_type(active_cfg.type);
            g_runtime_drives[0].config_slot = found_slot;
            active_config_slot = found_slot;
            next_runtime_slot = 1;
        }
    }

    for (uint8_t config_slot = 0; config_slot < SIDETNFS_MAX_DRIVES; config_slot++)
    {
        if ((int)config_slot == active_config_slot)
        {
            continue;
        }
        sidetnfs_drive_config_t cfg;
        if (sidetnfs_config_get_drive(config_slot, &cfg) != SIDETNFS_CONFIG_STATUS_OK)
        {
            continue;
        }
        if (!sidetnfs_drive_slot_is_enabled(&cfg))
        {
            continue;
        }
        if (next_runtime_slot >= GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES - 1)
        {
            break;
        }
        g_runtime_drives[next_runtime_slot].valid = true;
        g_runtime_drives[next_runtime_slot].drive_number = (uint32_t)(cfg.drive_letter - 'A');
        g_runtime_drives[next_runtime_slot].config = cfg;
        g_runtime_drives[next_runtime_slot].backend = backend_for_type(cfg.type);
        g_runtime_drives[next_runtime_slot].config_slot = (int)config_slot;
        next_runtime_slot++;
    }

    if (!settings_placed)
    {
        int settings_slot = next_runtime_slot;
        g_runtime_drives[settings_slot].valid = true;
        g_runtime_drives[settings_slot].drive_number = (uint32_t)(settings_letter - 'A');
        g_runtime_drives[settings_slot].backend = GEMDRIVE_FILE_BACKEND_CONFIG_FLASH;
        g_runtime_drives[settings_slot].config.state = SIDETNFS_DRIVE_SLOT_ENABLED;
        g_runtime_drives[settings_slot].config.drive_letter = settings_letter;
        g_runtime_drives[settings_slot].config.type = SIDETNFS_DRIVE_SD;
        g_runtime_drives[settings_slot].config_slot = -1;
    }
}

/* ---- test helpers ---- */
static void set_drive(uint8_t config_slot, sidetnfs_drive_slot_state_t state, char letter)
{
    sidetnfs_drive_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state = state;
    cfg.drive_letter = (uint8_t)letter;
    cfg.type = SIDETNFS_DRIVE_TNFS;
    cfg.transport = SIDETNFS_TRANSPORT_UDP;
    strncpy(cfg.host, "10.0.0.1", sizeof(cfg.host) - 1);
    cfg.port = 16384;
    strncpy(cfg.mount_path, "/Atari.ST", sizeof(cfg.mount_path) - 1);
    CHECK(sidetnfs_config_set_drive(config_slot, &cfg) == SIDETNFS_CONFIG_STATUS_OK, "set_drive accepted");
}

// sidetnfs_config_init()'s own factory defaults ENABLE config_slot 0
// with letter 'N' out of the box (SIDETNFS_DEFAULT_DRIVE, matching the
// product's original single-drive default) -- every test here wants
// deliberate, explicit control over which slots are ENABLED/DISABLED/
// EMPTY, so this clears all 8 back to EMPTY first.
static void clear_all_slots(void)
{
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        CHECK(sidetnfs_config_set_drive_state(i, SIDETNFS_DRIVE_SLOT_EMPTY) == SIDETNFS_CONFIG_STATUS_OK, "slot cleared to EMPTY");
    }
}

static int count_valid(void)
{
    int n = 0;
    for (int i = 0; i < GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES; i++)
    {
        if (g_runtime_drives[i].valid)
        {
            n++;
        }
    }
    return n;
}

// Test A: all ordinary config slots EMPTY -> only SETTINGS, at runtime slot 0.
static void test_A_all_empty_only_settings(void)
{
    printf("Test A: all ordinary slots EMPTY -> only SETTINGS\n");
    sidetnfs_config_init();
    clear_all_slots();

    // No active TNFS/UDP server exists -> select_gemdrive_drive_letter()'s
    // real fallback (PARAM_GEMDRIVE_DRIVE, default 'C') is mirrored here
    // directly as the active_drive_letter argument.
    runtime_drives_init_mirror('C', 0);

    CHECK(count_valid() == 1, "exactly one runtime entry");
    CHECK(g_runtime_drives[0].valid, "runtime slot 0 valid");
    CHECK(g_runtime_drives[0].backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH, "runtime slot 0 is SETTINGS");
    CHECK(g_runtime_drives[0].config_slot == -1, "SETTINGS has no config_slot");
}

// Test B: one ENABLED slot -> ordinary + SETTINGS.
static void test_B_one_enabled_ordinary_plus_settings(void)
{
    printf("Test B: one ENABLED slot -> ordinary + SETTINGS\n");
    sidetnfs_config_init();
    clear_all_slots();
    set_drive(3, SIDETNFS_DRIVE_SLOT_ENABLED, 'N');

    runtime_drives_init_mirror('N', 0); // select_gemdrive_drive_letter() would resolve to 'N' via sidetnfs_probe

    CHECK(count_valid() == 2, "two runtime entries");
    CHECK(g_runtime_drives[0].valid && g_runtime_drives[0].backend == GEMDRIVE_FILE_BACKEND_TNFS, "runtime slot 0 is the ordinary drive");
    CHECK(g_runtime_drives[0].config_slot == 3, "runtime slot 0 maps back to config_slot 3");
    CHECK(g_runtime_drives[1].valid && g_runtime_drives[1].backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH, "runtime slot 1 is SETTINGS");
}

// Test C: config_slot 0 ENABLED, config_slot 1 DISABLED, config_slot 2
// ENABLED -> two ordinary runtime drives with config_slot 0 and 2.
static void test_C_disabled_slot_skipped(void)
{
    printf("Test C: slot 0 ENABLED, slot 1 DISABLED, slot 2 ENABLED -> two ordinary drives, config_slot 0 and 2\n");
    sidetnfs_config_init();
    clear_all_slots();
    set_drive(0, SIDETNFS_DRIVE_SLOT_ENABLED, 'N');
    set_drive(1, SIDETNFS_DRIVE_SLOT_DISABLED, 'O');
    set_drive(2, SIDETNFS_DRIVE_SLOT_ENABLED, 'P');

    runtime_drives_init_mirror('N', 0);

    CHECK(count_valid() == 3, "three runtime entries (2 ordinary + SETTINGS)");
    CHECK(g_runtime_drives[0].config_slot == 0, "runtime slot 0 <- config_slot 0");
    CHECK(g_runtime_drives[1].valid && g_runtime_drives[1].backend == GEMDRIVE_FILE_BACKEND_TNFS, "runtime slot 1 is ordinary");
    CHECK(g_runtime_drives[1].config_slot == 2, "runtime slot 1 <- config_slot 2 (slot 1/DISABLED skipped)");
    CHECK(g_runtime_drives[2].backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH, "runtime slot 2 is SETTINGS");
}

// Test D: only config_slot 7 ENABLED -> runtime slot 0 refers to config_slot 7.
static void test_D_only_slot7_enabled(void)
{
    printf("Test D: only config_slot 7 ENABLED -> runtime slot 0 refers to config_slot 7\n");
    sidetnfs_config_init();
    clear_all_slots();
    set_drive(7, SIDETNFS_DRIVE_SLOT_ENABLED, 'Z');

    runtime_drives_init_mirror('Z', 0);

    CHECK(count_valid() == 2, "two runtime entries");
    CHECK(g_runtime_drives[0].valid && g_runtime_drives[0].config_slot == 7, "runtime slot 0 <- config_slot 7");
    CHECK(g_runtime_drives[0].config.drive_letter == 'Z', "runtime slot 0 carries the real letter Z");
}

// Test E: eight ENABLED ordinary slots -> nine runtime entries, no overflow.
static void test_E_eight_enabled_no_overflow(void)
{
    printf("Test E: eight ENABLED ordinary slots -> nine runtime entries, no overflow\n");
    sidetnfs_config_init();
    clear_all_slots();
    const char letters[8] = {'N', 'O', 'P', 'Q', 'R', 'T', 'U', 'V'}; // 'S' reserved for SETTINGS by default
    for (uint8_t i = 0; i < 8; i++)
    {
        set_drive(i, SIDETNFS_DRIVE_SLOT_ENABLED, letters[i]);
    }

    runtime_drives_init_mirror('N', 0); // sidetnfs_probe_load_active_server() picks the first ENABLED slot, config_slot 0

    CHECK(count_valid() == 9, "nine runtime entries (8 ordinary + SETTINGS)");
    for (int i = 0; i < 8; i++)
    {
        CHECK(g_runtime_drives[i].valid && g_runtime_drives[i].backend == GEMDRIVE_FILE_BACKEND_TNFS, "ordinary runtime slot valid and tagged TNFS");
        CHECK(g_runtime_drives[i].config_slot == i, "runtime slot maps back to the matching config_slot, in order");
    }
    CHECK(g_runtime_drives[8].valid && g_runtime_drives[8].backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH, "runtime slot 8 (last) is SETTINGS");
    CHECK(g_runtime_drives[8].config_slot == -1, "SETTINGS has no config_slot");
}

int main(void)
{
    test_A_all_empty_only_settings();
    test_B_one_enabled_ordinary_plus_settings();
    test_C_disabled_slot_skipped();
    test_D_only_slot7_enabled();
    test_E_eight_enabled_no_overflow();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
