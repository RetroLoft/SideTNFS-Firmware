/**
 * File: test_config_slot_states.c
 * Fase 12B: host test for sidetnfs_config.c's three-state drive-slot model
 * (EMPTY/DISABLED/ENABLED) and the version-2-to-3 migration path.
 *
 * Compiles the REAL sidetnfs_config.c (symlinked into sandbox/) against
 * stub hardware/flash.h + hardware/sync.h (the real ones pull in Pico SDK
 * headers a host toolchain can't build) backed by a fake flash buffer
 * (g_fake_flash) -- same pattern tests/host_netconfig/test_netconfig.c
 * uses for sidetnfs_netconfig.c.
 *
 * Not wired into build.sh/CMakeLists.txt -- a pure host-side check, same
 * category as tests/host_netconfig/ and tests/host_configdrive/. Run
 * directly, e.g.:
 *   gcc -std=c11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_config_slot_states.c sandbox/sidetnfs_config.c \
 *       -o /tmp/test_config_slot_states && /tmp/test_config_slot_states
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "include/sidetnfs_config.h"
#include "hardware/flash.h"

uint8_t g_fake_flash[0x102000];

void flash_range_erase(uint32_t flash_offs, size_t count)
{
    memset(g_fake_flash + flash_offs, 0xFF, count); // erased NOR flash reads as 0xFF
}

void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count)
{
    memcpy(g_fake_flash + flash_offs, data, count);
}

// ---- Independent CRC32 copy (same IEEE 802.3/zlib algorithm sidetnfs_config.c
// uses) -- needed to hand-construct valid flash blobs for injection tests;
// sidetnfs_crc32() itself is static/private to sidetnfs_config.c. ----
static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- Byte-identical mirrors of sidetnfs_config.c's private v2 types,
// purely so this test can hand-construct a version-2 flash blob. Must
// stay byte-identical to sidetnfs_drive_config_t/sidetnfs_drive_flash_t --
// the real file's own _Static_assert()s already guarantee that. ----
typedef struct
{
    uint8_t used;
    uint8_t drive_letter;
    uint8_t type;
    uint8_t transport;
    uint16_t port;
    uint8_t reserved0[2];
    char nickname[SIDETNFS_NICKNAME_LEN];
    char host[SIDETNFS_HOST_LEN];
    char mount_path[SIDETNFS_MOUNTPATH_LEN];
    char sd_path[SIDETNFS_SDPATH_LEN];
} test_drive_config_v2_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t config_drive_letter;
    uint8_t drive_count;
    uint8_t reserved[2];
    test_drive_config_v2_t drives[SIDETNFS_MAX_DRIVES];
    uint32_t crc32;
} test_drive_flash_v2_t;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                  \
    do                                                                    \
    {                                                                     \
        g_checks++;                                                      \
        if (!(cond))                                                     \
        {                                                                 \
            g_failures++;                                                \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
        }                                                                 \
    } while (0)

static void write_v2_blob_to_flash(const test_drive_flash_v2_t *blob_no_crc)
{
    test_drive_flash_v2_t blob = *blob_no_crc;
    blob.crc32 = test_crc32((const uint8_t *)&blob, offsetof(test_drive_flash_v2_t, crc32));
    memset(g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET, 0, SIDETNFS_CONFIG_FLASH_SIZE);
    memcpy(g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET, &blob, sizeof(blob));
}

static void write_v3_blob_to_flash(const sidetnfs_drive_flash_t *blob_no_crc)
{
    sidetnfs_drive_flash_t blob = *blob_no_crc;
    blob.crc32 = test_crc32((const uint8_t *)&blob, offsetof(sidetnfs_drive_flash_t, crc32));
    memset(g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET, 0, SIDETNFS_CONFIG_FLASH_SIZE);
    memcpy(g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET, &blob, sizeof(blob));
}

// ---- Test A: version-2 config, two used drives -> both ENABLED, rest EMPTY ----
static void test_a_migrate_two_used(void)
{
    printf("Test A: v2 migration, two used drives\n");
    test_drive_flash_v2_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.magic = SIDETNFS_CONFIG_MAGIC;
    v2.version = 2;
    v2.config_drive_letter = 'S';
    v2.drive_count = 2;
    v2.drives[0].used = 1;
    v2.drives[0].drive_letter = 'N';
    v2.drives[0].type = SIDETNFS_DRIVE_TNFS;
    v2.drives[0].transport = SIDETNFS_TRANSPORT_UDP;
    v2.drives[0].port = 16384;
    strncpy(v2.drives[0].nickname, "RetroLoft", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v2.drives[0].host, "192.168.178.10", SIDETNFS_HOST_LEN - 1);
    strncpy(v2.drives[0].mount_path, "Atari.ST", SIDETNFS_MOUNTPATH_LEN - 1);
    v2.drives[1].used = 1;
    v2.drives[1].drive_letter = 'O';
    v2.drives[1].type = SIDETNFS_DRIVE_TNFS;
    v2.drives[1].transport = SIDETNFS_TRANSPORT_UDP;
    v2.drives[1].port = 16385;
    strncpy(v2.drives[1].nickname, "SecondServer", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v2.drives[1].host, "192.168.178.11", SIDETNFS_HOST_LEN - 1);
    strncpy(v2.drives[1].mount_path, "Games.ST", SIDETNFS_MOUNTPATH_LEN - 1);

    write_v2_blob_to_flash(&v2);
    sidetnfs_config_init();

    CHECK(sidetnfs_config_loaded_from_flash(), "config should be loaded_from_flash after successful migration");
    CHECK(sidetnfs_config_migrated_from_version() == 2, "migrated_from_version should report 2");

    sidetnfs_drive_config_t d;
    CHECK(sidetnfs_config_get_drive(0, &d) == SIDETNFS_CONFIG_STATUS_OK, "GET slot0 status OK");
    CHECK(d.state == SIDETNFS_DRIVE_SLOT_ENABLED, "slot0 ENABLED after migration");
    CHECK(d.drive_letter == 'N', "slot0 letter preserved");
    CHECK(strcmp(d.host, "192.168.178.10") == 0, "slot0 host preserved");

    CHECK(sidetnfs_config_get_drive(1, &d) == SIDETNFS_CONFIG_STATUS_OK, "GET slot1 status OK");
    CHECK(d.state == SIDETNFS_DRIVE_SLOT_ENABLED, "slot1 ENABLED after migration");
    CHECK(d.drive_letter == 'O', "slot1 letter preserved");

    for (uint8_t i = 2; i < SIDETNFS_MAX_DRIVES; i++)
    {
        CHECK(sidetnfs_config_get_drive(i, &d) == SIDETNFS_CONFIG_STATUS_OK, "GET empty slot status OK");
        CHECK(d.state == SIDETNFS_DRIVE_SLOT_EMPTY, "slots 2..7 EMPTY after migration");
    }
    CHECK(sidetnfs_config_get_drive_count() == 2, "drive_count == 2 (configured) after migration");
    CHECK(sidetnfs_config_get_enabled_drive_count() == 2, "enabled_count == 2 after migration");
}

// ---- Test B: version-2 config, one used drive -> one ENABLED, seven EMPTY ----
static void test_b_migrate_one_used(void)
{
    printf("Test B: v2 migration, one used drive\n");
    test_drive_flash_v2_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.magic = SIDETNFS_CONFIG_MAGIC;
    v2.version = 2;
    v2.config_drive_letter = 'S';
    v2.drive_count = 1;
    v2.drives[0].used = 1;
    v2.drives[0].drive_letter = 'N';
    v2.drives[0].type = SIDETNFS_DRIVE_TNFS;
    v2.drives[0].transport = SIDETNFS_TRANSPORT_UDP;
    v2.drives[0].port = 16384;
    strncpy(v2.drives[0].nickname, "RetroLoft", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v2.drives[0].host, "192.168.178.10", SIDETNFS_HOST_LEN - 1);
    strncpy(v2.drives[0].mount_path, "Atari.ST", SIDETNFS_MOUNTPATH_LEN - 1);

    write_v2_blob_to_flash(&v2);
    sidetnfs_config_init();

    CHECK(sidetnfs_config_loaded_from_flash(), "loaded_from_flash after migration");
    CHECK(sidetnfs_config_migrated_from_version() == 2, "migrated_from_version == 2");
    CHECK(sidetnfs_config_get_enabled_drive_count() == 1, "exactly one ENABLED");
    CHECK(sidetnfs_config_get_drive_count() == 1, "drive_count == 1");

    int empty_count = 0;
    sidetnfs_drive_config_t d;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_config_get_drive(i, &d);
        if (d.state == SIDETNFS_DRIVE_SLOT_EMPTY) empty_count++;
    }
    CHECK(empty_count == 7, "seven EMPTY slots");
}

// ---- Test C: v3 config with a DISABLED drive -- valid, counted, persists across SAVE/reload ----
static void test_c_disabled_drive_persists(void)
{
    printf("Test C: DISABLED drive valid, persists across SAVE/reload\n");
    sidetnfs_drive_flash_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = SIDETNFS_CONFIG_MAGIC;
    v3.version = SIDETNFS_CONFIG_FLASH_VERSION;
    v3.config_drive_letter = 'S';
    v3.drive_count = 1;
    v3.drives[2].state = SIDETNFS_DRIVE_SLOT_DISABLED;
    v3.drives[2].drive_letter = 'P';
    v3.drives[2].type = SIDETNFS_DRIVE_TNFS;
    v3.drives[2].transport = SIDETNFS_TRANSPORT_UDP;
    v3.drives[2].port = 12345;
    strncpy(v3.drives[2].nickname, "ParkedDrive", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v3.drives[2].host, "10.0.0.5", SIDETNFS_HOST_LEN - 1);
    strncpy(v3.drives[2].mount_path, "Parked", SIDETNFS_MOUNTPATH_LEN - 1);

    write_v3_blob_to_flash(&v3);
    sidetnfs_config_init();

    CHECK(sidetnfs_config_loaded_from_flash(), "native v3 block loaded without fallback");
    CHECK(sidetnfs_config_migrated_from_version() == 0, "no migration for a native v3 block");
    CHECK(sidetnfs_config_get_drive_count() == 1, "drive_count counts the DISABLED slot");
    CHECK(sidetnfs_config_get_enabled_drive_count() == 0, "enabled_count excludes the DISABLED slot");

    sidetnfs_drive_config_t d;
    CHECK(sidetnfs_config_get_drive(2, &d) == SIDETNFS_CONFIG_STATUS_OK, "GET DISABLED slot OK");
    CHECK(d.state == SIDETNFS_DRIVE_SLOT_DISABLED, "slot2 reports DISABLED");
    CHECK(strcmp(d.host, "10.0.0.5") == 0, "DISABLED slot's full config present");

    // SAVE then reload (fresh sidetnfs_config_init()) -- must still be DISABLED.
    CHECK(sidetnfs_config_save() == SIDETNFS_CONFIG_STATUS_OK, "SAVE_CONFIG succeeds with a DISABLED slot present");
    sidetnfs_config_init();
    CHECK(sidetnfs_config_get_drive(2, &d) == SIDETNFS_CONFIG_STATUS_OK, "GET after reload OK");
    CHECK(d.state == SIDETNFS_DRIVE_SLOT_DISABLED, "state still DISABLED after SAVE + reload");
    CHECK(strcmp(d.host, "10.0.0.5") == 0, "fields still intact after SAVE + reload");
}

// ---- Test D: emptying a slot wipes it and frees its letter ----
static void test_d_empty_slot_wipes_and_frees_letter(void)
{
    printf("Test D: emptying a slot wipes it and frees its letter\n");
    sidetnfs_config_init(); // fresh factory defaults (fake flash still holds test C's blank-after-erase state at offset, but init() falls back safely regardless)

    sidetnfs_drive_config_t d;
    memset(&d, 0, sizeof(d));
    d.state = SIDETNFS_DRIVE_SLOT_ENABLED;
    d.drive_letter = 'Q';
    d.type = SIDETNFS_DRIVE_TNFS;
    d.transport = SIDETNFS_TRANSPORT_UDP;
    d.port = 9999;
    strncpy(d.nickname, "ToBeCleared", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(d.host, "10.0.0.9", SIDETNFS_HOST_LEN - 1);
    strncpy(d.mount_path, "X", SIDETNFS_MOUNTPATH_LEN - 1);

    CHECK(sidetnfs_config_set_drive(5, &d) == SIDETNFS_CONFIG_STATUS_OK, "SET slot5 ENABLED ok");
    uint8_t before_count = sidetnfs_config_get_drive_count();

    sidetnfs_drive_config_t empty;
    memset(&empty, 0, sizeof(empty)); // state == EMPTY (0)
    CHECK(sidetnfs_config_set_drive(5, &empty) == SIDETNFS_CONFIG_STATUS_OK, "SET slot5 EMPTY ok");

    sidetnfs_drive_config_t readback;
    CHECK(sidetnfs_config_get_drive(5, &readback) == SIDETNFS_CONFIG_STATUS_OK, "GET slot5 after empty OK");
    CHECK(readback.state == SIDETNFS_DRIVE_SLOT_EMPTY, "slot5 state EMPTY");
    CHECK(readback.drive_letter == 0, "slot5 drive_letter wiped");
    CHECK(readback.nickname[0] == '\0', "slot5 nickname wiped");
    CHECK(readback.host[0] == '\0', "slot5 host wiped");
    CHECK(sidetnfs_config_get_drive_count() == (uint8_t)(before_count - 1), "drive_count decreased by one");

    // Letter 'Q' must be free again -- re-use it in a different slot.
    d.drive_letter = 'Q';
    CHECK(sidetnfs_config_set_drive(6, &d) == SIDETNFS_CONFIG_STATUS_OK, "letter Q reusable after slot5 emptied");
    sidetnfs_config_delete_drive(6); // cleanup for subsequent tests
}

// ---- Test E: DISABLED <-> ENABLED preserves every other field ----
static void test_e_disabled_enabled_roundtrip(void)
{
    printf("Test E: DISABLED <-> ENABLED preserves fields\n");
    sidetnfs_drive_config_t d;
    memset(&d, 0, sizeof(d));
    d.state = SIDETNFS_DRIVE_SLOT_ENABLED;
    d.drive_letter = 'R';
    d.type = SIDETNFS_DRIVE_SD;
    strncpy(d.nickname, "SdCardOne", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(d.sd_path, "/games", SIDETNFS_SDPATH_LEN - 1);
    CHECK(sidetnfs_config_set_drive(3, &d) == SIDETNFS_CONFIG_STATUS_OK, "SET slot3 ENABLED ok");

    CHECK(sidetnfs_config_set_drive_state(3, SIDETNFS_DRIVE_SLOT_DISABLED) == SIDETNFS_CONFIG_STATUS_OK, "state -> DISABLED ok");
    sidetnfs_drive_config_t after_disable;
    sidetnfs_config_get_drive(3, &after_disable);
    CHECK(after_disable.state == SIDETNFS_DRIVE_SLOT_DISABLED, "state is DISABLED");
    CHECK(strcmp(after_disable.nickname, "SdCardOne") == 0, "nickname unchanged after disabling");
    CHECK(strcmp(after_disable.sd_path, "/games") == 0, "sd_path unchanged after disabling");
    CHECK(after_disable.drive_letter == 'R', "letter unchanged after disabling");

    CHECK(sidetnfs_config_set_drive_state(3, SIDETNFS_DRIVE_SLOT_ENABLED) == SIDETNFS_CONFIG_STATUS_OK, "state -> ENABLED ok");
    sidetnfs_drive_config_t after_enable;
    sidetnfs_config_get_drive(3, &after_enable);
    CHECK(after_enable.state == SIDETNFS_DRIVE_SLOT_ENABLED, "state is ENABLED again");
    CHECK(strcmp(after_enable.nickname, "SdCardOne") == 0, "nickname unchanged after re-enabling");
    CHECK(strcmp(after_enable.sd_path, "/games") == 0, "sd_path unchanged after re-enabling");
    sidetnfs_config_delete_drive(3); // cleanup
}

// ---- Test F: invalid state values are rejected ----
static void test_f_invalid_state_rejected(void)
{
    printf("Test F: invalid state values rejected\n");
    sidetnfs_drive_config_t before;
    sidetnfs_config_get_drive(4, &before);

    sidetnfs_drive_config_t d = before;
    d.state = 3;
    d.drive_letter = 'W';
    d.type = SIDETNFS_DRIVE_SD;
    strncpy(d.sd_path, "/x", SIDETNFS_SDPATH_LEN - 1);
    CHECK(sidetnfs_config_set_drive(4, &d) == SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE, "state=3 rejected");

    d.state = 255;
    CHECK(sidetnfs_config_set_drive(4, &d) == SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE, "state=255 rejected");

    sidetnfs_drive_config_t after;
    sidetnfs_config_get_drive(4, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0, "slot4 unchanged after rejected SET_DRIVE calls");

    CHECK(sidetnfs_config_set_drive_state(4, (sidetnfs_drive_slot_state_t)3) == SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE,
          "set_drive_state(3) rejected");

    // Also confirm an invalid state in a raw flash blob is rejected at boot.
    sidetnfs_drive_flash_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = SIDETNFS_CONFIG_MAGIC;
    v3.version = SIDETNFS_CONFIG_FLASH_VERSION;
    v3.config_drive_letter = 'S';
    v3.drive_count = 1;
    v3.drives[0].state = 200; // invalid
    v3.drives[0].drive_letter = 'Z';
    write_v3_blob_to_flash(&v3);
    sidetnfs_config_init();
    CHECK(!sidetnfs_config_loaded_from_flash(), "invalid state in flash falls back to defaults");
    CHECK(sidetnfs_config_get_fallback_reason() == SIDETNFS_CONFIG_FALLBACK_STRUCTURE_INVALID, "fallback reason STRUCTURE_INVALID");
    CHECK(sidetnfs_config_get_fallback_structure_status() == SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE,
          "fallback structure status INVALID_DRIVE_STATE");
}

// ---- Test G: duplicate letter across ENABLED + DISABLED rejected ----
static void test_g_duplicate_letter_enabled_disabled(void)
{
    printf("Test G: duplicate letter (one ENABLED, one DISABLED) rejected\n");

    // Layer 1: SET_DRIVE-level real-time rejection.
    sidetnfs_config_init(); // fresh factory defaults
    sidetnfs_drive_config_t d;
    memset(&d, 0, sizeof(d));
    d.state = SIDETNFS_DRIVE_SLOT_ENABLED;
    d.drive_letter = 'N'; // factory default already uses 'N' at slot0 -- pick a fresh slot/letter combo instead
    d.drive_letter = 'K';
    d.type = SIDETNFS_DRIVE_TNFS;
    d.transport = SIDETNFS_TRANSPORT_UDP;
    d.port = 1111;
    strncpy(d.nickname, "First", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(d.host, "10.0.0.1", SIDETNFS_HOST_LEN - 1);
    strncpy(d.mount_path, "A", SIDETNFS_MOUNTPATH_LEN - 1);
    CHECK(sidetnfs_config_set_drive(2, &d) == SIDETNFS_CONFIG_STATUS_OK, "SET slot2 letter K ENABLED ok");

    sidetnfs_drive_config_t d2 = d;
    strncpy(d2.nickname, "Second", SIDETNFS_NICKNAME_LEN - 1);
    d2.state = SIDETNFS_DRIVE_SLOT_DISABLED; // same letter 'K', different slot, DISABLED
    CHECK(sidetnfs_config_set_drive(3, &d2) == SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER,
          "SET slot3 same letter K (DISABLED) rejected in real time");
    sidetnfs_config_delete_drive(2); // cleanup

    // Layer 2: structure-validation at boot, injecting the conflict directly
    // (bypassing the API's own real-time check entirely).
    sidetnfs_drive_flash_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = SIDETNFS_CONFIG_MAGIC;
    v3.version = SIDETNFS_CONFIG_FLASH_VERSION;
    v3.config_drive_letter = 'S';
    v3.drive_count = 2;
    v3.drives[0].state = SIDETNFS_DRIVE_SLOT_ENABLED;
    v3.drives[0].drive_letter = 'K';
    v3.drives[0].type = SIDETNFS_DRIVE_TNFS;
    v3.drives[0].transport = SIDETNFS_TRANSPORT_UDP;
    v3.drives[0].port = 1;
    strncpy(v3.drives[0].nickname, "A", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v3.drives[0].host, "10.0.0.1", SIDETNFS_HOST_LEN - 1);
    strncpy(v3.drives[0].mount_path, "A", SIDETNFS_MOUNTPATH_LEN - 1);
    v3.drives[1] = v3.drives[0];
    v3.drives[1].state = SIDETNFS_DRIVE_SLOT_DISABLED;
    strncpy(v3.drives[1].nickname, "B", SIDETNFS_NICKNAME_LEN - 1);

    write_v3_blob_to_flash(&v3);
    sidetnfs_config_init();
    CHECK(!sidetnfs_config_loaded_from_flash(), "flash with ENABLED+DISABLED duplicate letter falls back");
    CHECK(sidetnfs_config_get_fallback_reason() == SIDETNFS_CONFIG_FALLBACK_STRUCTURE_INVALID, "fallback reason STRUCTURE_INVALID");
    CHECK(sidetnfs_config_get_fallback_structure_status() == SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER,
          "fallback structure status DUPLICATE_DRIVE_LETTER");
}

// ---- Test H: conflict with the SETTINGS drive letter ----
static void test_h_settings_letter_conflict(void)
{
    printf("Test H: conflict with SETTINGS drive letter\n");
    sidetnfs_config_init(); // fresh factory defaults, config_drive_letter == 'S'
    CHECK(sidetnfs_config_get_config_drive_letter() == 'S', "factory settings letter is S");

    sidetnfs_drive_config_t d;
    memset(&d, 0, sizeof(d));
    d.state = SIDETNFS_DRIVE_SLOT_DISABLED;
    d.drive_letter = 'S'; // same as settings letter
    d.type = SIDETNFS_DRIVE_TNFS;
    d.transport = SIDETNFS_TRANSPORT_UDP;
    d.port = 1;
    strncpy(d.nickname, "Conflict", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(d.host, "10.0.0.1", SIDETNFS_HOST_LEN - 1);
    strncpy(d.mount_path, "A", SIDETNFS_MOUNTPATH_LEN - 1);
    CHECK(sidetnfs_config_set_drive(4, &d) == SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER,
          "SET_DRIVE with settings letter (DISABLED) rejected in real time");

    // set_config_drive_letter() must also refuse to hand 'S' worth of
    // meaning to an already-configured ordinary slot's letter, and vice
    // versa: configure an ordinary DISABLED slot at letter 'T' first, then
    // try to move the settings letter onto 'T'.
    d.drive_letter = 'T';
    CHECK(sidetnfs_config_set_drive(4, &d) == SIDETNFS_CONFIG_STATUS_OK, "SET_DRIVE slot4 letter T (DISABLED) ok");
    CHECK(sidetnfs_config_set_config_drive_letter('T') == SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER,
          "SET_CONFIG_DRIVE onto a DISABLED ordinary drive's letter rejected");
    sidetnfs_config_delete_drive(4); // cleanup

    // Flash-validation layer: inject a DISABLED slot sharing the settings
    // letter directly.
    sidetnfs_drive_flash_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = SIDETNFS_CONFIG_MAGIC;
    v3.version = SIDETNFS_CONFIG_FLASH_VERSION;
    v3.config_drive_letter = 'S';
    v3.drive_count = 1;
    v3.drives[0].state = SIDETNFS_DRIVE_SLOT_DISABLED;
    v3.drives[0].drive_letter = 'S';
    v3.drives[0].type = SIDETNFS_DRIVE_TNFS;
    v3.drives[0].transport = SIDETNFS_TRANSPORT_UDP;
    v3.drives[0].port = 1;
    strncpy(v3.drives[0].nickname, "Conflict", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v3.drives[0].host, "10.0.0.1", SIDETNFS_HOST_LEN - 1);
    strncpy(v3.drives[0].mount_path, "A", SIDETNFS_MOUNTPATH_LEN - 1);

    write_v3_blob_to_flash(&v3);
    sidetnfs_config_init();
    CHECK(!sidetnfs_config_loaded_from_flash(), "flash with DISABLED slot sharing settings letter falls back");
    CHECK(sidetnfs_config_get_fallback_structure_status() == SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER,
          "fallback structure status DUPLICATE_DRIVE_LETTER");
}

// ---- Fase 12B2 Deel G: mirrors sidetnfs_probe_load_active_server()'s
// exact scan logic (sidetnfs_probe.c) -- scan slots 0..7 in array order,
// skip any slot that isn't OK/ENABLED/TNFS/UDP, return the first match's
// index (or -1). This is the same building block gemdrvemul.c's
// sidetnfs_config_find_drive_by_letter() and sidetnfs_probe.c's
// sidetnfs_probe_load_active_server() both rely on after their Fase 12B
// enabled-checks -- host-testable here since it only needs the public
// sidetnfs_config_get_drive()/sidetnfs_drive_slot_is_enabled() API,
// unlike gemdrvemul.c/sidetnfs_probe.c themselves (Pico-SDK-only). ----
static int find_first_enabled_tnfs_udp_slot(char *out_letter)
{
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_drive_config_t d;
        if (sidetnfs_config_get_drive(i, &d) != SIDETNFS_CONFIG_STATUS_OK)
            continue;
        if (!sidetnfs_drive_slot_is_enabled(&d))
            continue; // EMPTY or DISABLED -- Fase 12B guard
        if (d.type != SIDETNFS_DRIVE_TNFS || d.transport != SIDETNFS_TRANSPORT_UDP)
            continue;
        if (out_letter) *out_letter = (char)d.drive_letter;
        return (int)i;
    }
    return -1;
}

static void set_tnfs_drive(uint8_t index, char letter, sidetnfs_drive_slot_state_t state)
{
    sidetnfs_drive_config_t d;
    memset(&d, 0, sizeof(d));
    d.state = state;
    d.drive_letter = (uint8_t)letter;
    d.type = SIDETNFS_DRIVE_TNFS;
    d.transport = SIDETNFS_TRANSPORT_UDP;
    d.port = 1000 + index;
    snprintf(d.nickname, SIDETNFS_NICKNAME_LEN, "Drive%c", letter);
    strncpy(d.host, "10.0.0.1", SIDETNFS_HOST_LEN - 1);
    strncpy(d.mount_path, "X", SIDETNFS_MOUNTPATH_LEN - 1);
    sidetnfs_config_status_t rc = sidetnfs_config_set_drive(index, &d);
    CHECK(rc == SIDETNFS_CONFIG_STATUS_OK, "set_tnfs_drive() helper: SET_DRIVE ok");
}

// Test 2 -- v3, N enabled, O disabled: scan finds only N.
static void test_2_n_enabled_o_disabled(void)
{
    printf("Test 2: N enabled, O disabled -- scan finds only N\n");
    sidetnfs_config_init(); // fresh factory defaults (blank fake flash)
    // Deliberately store 'O' at index 0 and 'N' at index 3, proving no
    // assumption that "config slot 0" is letter N or that ordinary
    // runtime-slot 0 is config-slot 0.
    set_tnfs_drive(0, 'O', SIDETNFS_DRIVE_SLOT_DISABLED);
    set_tnfs_drive(3, 'N', SIDETNFS_DRIVE_SLOT_ENABLED);

    char letter = 0;
    int slot = find_first_enabled_tnfs_udp_slot(&letter);
    CHECK(slot == 3, "first enabled TNFS/UDP slot is index 3 (letter N), not index 0 (letter O, DISABLED)");
    CHECK(letter == 'N', "selected letter is N");
    sidetnfs_config_delete_drive(0);
    sidetnfs_config_delete_drive(3);
}

// Test 3 -- v3, N disabled, O enabled: scan finds only O.
static void test_3_n_disabled_o_enabled(void)
{
    printf("Test 3: N disabled, O enabled -- scan finds only O\n");
    sidetnfs_config_init();
    sidetnfs_config_delete_drive(0); // clear the factory-default N at slot0 first -- test places N at slot5 instead
    set_tnfs_drive(5, 'N', SIDETNFS_DRIVE_SLOT_DISABLED);
    set_tnfs_drive(1, 'O', SIDETNFS_DRIVE_SLOT_ENABLED);

    char letter = 0;
    int slot = find_first_enabled_tnfs_udp_slot(&letter);
    CHECK(slot == 1, "first enabled TNFS/UDP slot is index 1 (letter O)");
    CHECK(letter == 'O', "selected letter is O, N (DISABLED) never considered");
    sidetnfs_config_delete_drive(5);
    sidetnfs_config_delete_drive(1);
}

// Test 4 -- both N and O disabled: scan finds nothing, no crash.
static void test_4_both_disabled(void)
{
    printf("Test 4: N and O both disabled -- scan finds nothing\n");
    sidetnfs_config_init();
    set_tnfs_drive(0, 'N', SIDETNFS_DRIVE_SLOT_DISABLED);
    set_tnfs_drive(1, 'O', SIDETNFS_DRIVE_SLOT_DISABLED);

    int slot = find_first_enabled_tnfs_udp_slot(NULL);
    CHECK(slot == -1, "no ENABLED TNFS/UDP drive found when both N and O are DISABLED");
    sidetnfs_config_delete_drive(0);
    sidetnfs_config_delete_drive(1);
}

// Test 5 -- all eight slots EMPTY: scan finds nothing, no crash.
static void test_5_all_empty(void)
{
    printf("Test 5: all eight slots EMPTY -- scan finds nothing\n");
    sidetnfs_config_init();
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_config_delete_drive(i); // EMPTY_SLOT if already empty -- fine, ignored
    }
    CHECK(sidetnfs_config_get_drive_count() == 0, "drive_count 0 with all slots EMPTY");
    int slot = find_first_enabled_tnfs_udp_slot(NULL);
    CHECK(slot == -1, "no drive found when all eight slots are EMPTY");
}

// Test 7 -- staged toggle without SAVE: flash bytes stay byte-identical.
// Models the reported "toggled Active/Inactive, quit without Save"
// scenario: even if SET_DRIVE calls DID reach the firmware (which
// AtariConfig's actual code, per Deel A, never does on a toggle alone),
// as long as SAVE_CONFIG is never called, the flash sector itself must
// never change.
static void test_7_staged_toggle_without_save(void)
{
    printf("Test 7: SET_DRIVE without SAVE_CONFIG leaves flash byte-identical\n");
    sidetnfs_drive_flash_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = SIDETNFS_CONFIG_MAGIC;
    v3.version = SIDETNFS_CONFIG_FLASH_VERSION;
    v3.config_drive_letter = 'S';
    v3.drive_count = 1;
    v3.drives[0].state = SIDETNFS_DRIVE_SLOT_ENABLED;
    v3.drives[0].drive_letter = 'N';
    v3.drives[0].type = SIDETNFS_DRIVE_TNFS;
    v3.drives[0].transport = SIDETNFS_TRANSPORT_UDP;
    v3.drives[0].port = 16384;
    strncpy(v3.drives[0].nickname, "RetroLoft", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v3.drives[0].host, "192.168.178.10", SIDETNFS_HOST_LEN - 1);
    strncpy(v3.drives[0].mount_path, "Atari.ST", SIDETNFS_MOUNTPATH_LEN - 1);
    write_v3_blob_to_flash(&v3);
    sidetnfs_config_init();

    uint8_t snapshot[SIDETNFS_CONFIG_FLASH_SIZE];
    memcpy(snapshot, g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET, sizeof(snapshot));

    // Toggle the drive DISABLED then back to ENABLED via SET_DRIVE, and
    // also try SET_CONFIG_DRIVE and DELETE_DRIVE elsewhere -- simulating
    // worst-case protocol traffic a UI *could* send on a toggle, without
    // ever calling SAVE_CONFIG.
    CHECK(sidetnfs_config_set_drive_state(0, SIDETNFS_DRIVE_SLOT_DISABLED) == SIDETNFS_CONFIG_STATUS_OK, "toggle to DISABLED ok");
    CHECK(sidetnfs_config_set_drive_state(0, SIDETNFS_DRIVE_SLOT_ENABLED) == SIDETNFS_CONFIG_STATUS_OK, "toggle back to ENABLED ok");

    CHECK(memcmp(snapshot, g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET, sizeof(snapshot)) == 0,
          "flash sector byte-identical after SET_DRIVE-only traffic (no SAVE_CONFIG)");
}

// Test 8 -- factory reset persists a valid v3 block.
static void test_8_factory_reset(void)
{
    printf("Test 8: factory reset persists a valid v3 block\n");
    // Start from some unrelated, valid-but-different config so this
    // isn't a no-op.
    sidetnfs_drive_flash_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = SIDETNFS_CONFIG_MAGIC;
    v3.version = SIDETNFS_CONFIG_FLASH_VERSION;
    v3.config_drive_letter = 'Z';
    v3.drive_count = 1;
    v3.drives[6].state = SIDETNFS_DRIVE_SLOT_ENABLED;
    v3.drives[6].drive_letter = 'Q';
    v3.drives[6].type = SIDETNFS_DRIVE_SD;
    strncpy(v3.drives[6].nickname, "Other", SIDETNFS_NICKNAME_LEN - 1);
    strncpy(v3.drives[6].sd_path, "/x", SIDETNFS_SDPATH_LEN - 1);
    write_v3_blob_to_flash(&v3);
    sidetnfs_config_init();
    CHECK(sidetnfs_config_get_config_drive_letter() == 'Z', "pre-reset settings letter is Z");

    CHECK(sidetnfs_config_factory_reset() == SIDETNFS_CONFIG_STATUS_OK, "factory reset succeeds");

    // Reload from (fake) flash to prove it was actually persisted, not
    // just changed in RAM.
    sidetnfs_config_init();
    CHECK(sidetnfs_config_loaded_from_flash(), "post-reset config loaded_from_flash (really persisted)");
    CHECK(sidetnfs_config_migrated_from_version() == 0, "post-reset config is native v3, not a migration");
    CHECK(sidetnfs_config_get_config_drive_letter() == 'S', "post-reset settings letter is factory default S");

    sidetnfs_drive_config_t d;
    CHECK(sidetnfs_config_get_drive(0, &d) == SIDETNFS_CONFIG_STATUS_OK, "GET slot0 after reset ok");
    CHECK(d.state == SIDETNFS_DRIVE_SLOT_ENABLED, "factory default drive (slot0/N) is ENABLED");
    CHECK(d.drive_letter == 'N', "factory default drive letter is N");
    for (uint8_t i = 1; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_config_get_drive(i, &d);
        CHECK(d.state == SIDETNFS_DRIVE_SLOT_EMPTY, "every other slot is EMPTY after factory reset");
    }

    const sidetnfs_drive_flash_t *flash_ptr = (const sidetnfs_drive_flash_t *)(g_fake_flash + SIDETNFS_CONFIG_FLASH_OFFSET);
    uint32_t recomputed = test_crc32((const uint8_t *)flash_ptr, offsetof(sidetnfs_drive_flash_t, crc32));
    CHECK(flash_ptr->magic == SIDETNFS_CONFIG_MAGIC, "persisted magic correct");
    CHECK(flash_ptr->version == SIDETNFS_CONFIG_FLASH_VERSION, "persisted version == 3");
    CHECK(flash_ptr->crc32 == recomputed, "persisted CRC matches recomputed CRC");
}

int main(void)
{
    test_a_migrate_two_used();
    test_b_migrate_one_used();
    test_c_disabled_drive_persists();
    test_d_empty_slot_wipes_and_frees_letter();
    test_e_disabled_enabled_roundtrip();
    test_f_invalid_state_rejected();
    test_g_duplicate_letter_enabled_disabled();
    test_h_settings_letter_conflict();

    test_2_n_enabled_o_disabled();
    test_3_n_disabled_o_enabled();
    test_4_both_disabled();
    test_5_all_empty();
    test_7_staged_toggle_without_save();
    test_8_factory_reset();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
