/**
 * File: test_system_config.c
 * Fase 2: host test for sidetnfs_system_config.c -- its own magic/
 * version/CRC/structure validation, factory fallback, one-time
 * RAM-only migration from a simulated old ConfigEntry store, and that
 * sidetnfs_netconfig.c/sidetnfs_rtcconfig.c's "read current, merge,
 * set, save" pattern (exercised here directly against the system-config
 * API, matching what those two files now do) never clobbers the fields
 * the other one owns.
 *
 * Compiles the REAL sidetnfs_system_config.c (symlinked into sandbox/)
 * against stub hardware/flash.h + hardware/sync.h + config.h -- same
 * pattern tests/host_config/test_config_slot_states.c uses for
 * sidetnfs_config.c. configData/find_entry() are provided directly by
 * this test file (never a real load_all_entries()).
 *
 * Run:
 *   gcc -std=c11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_system_config.c sandbox/sidetnfs_system_config.c \
 *       -o /tmp/test_system_config && /tmp/test_system_config
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "include/sidetnfs_system_config.h"
#include "include/config.h"
#include "hardware/flash.h"

uint8_t g_fake_flash[0x102000];

void flash_range_erase(uint32_t flash_offs, size_t count)
{
    memset(g_fake_flash + flash_offs, 0xFF, count);
}

void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count)
{
    memcpy(g_fake_flash + flash_offs, data, count);
}

// ---- Minimal configData/find_entry() the test populates directly,
// standing in for a real load_all_entries() readout. ----
ConfigData configData;

// Fase 2B: mirrors config.c's own config_loaded_from_real_flash() --
// settable directly by the test (real config.c sets it inside
// load_all_entries() based on whether a matching magic was actually
// found in flash; this test simulates that boolean directly instead of
// simulating raw flash bytes for the legacy store too).
static bool g_test_loaded_from_real_flash = false;

bool config_loaded_from_real_flash(void)
{
    return g_test_loaded_from_real_flash;
}

ConfigEntry *find_entry(const char *key)
{
    for (size_t i = 0; i < configData.count; i++)
    {
        if (strncmp(configData.entries[i].key, key, MAX_KEY_LENGTH) == 0)
        {
            return &configData.entries[i];
        }
    }
    return NULL;
}

static void set_entry(const char *key, const char *value)
{
    ConfigEntry *e = find_entry(key);
    if (e == NULL)
    {
        e = &configData.entries[configData.count++];
        memset(e->key, 0, sizeof(e->key));
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    memset(e->value, 0, sizeof(e->value));
    strncpy(e->value, value, sizeof(e->value) - 1);
}

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                             \
    do                                                                \
    {                                                                 \
        g_checks++;                                                   \
        if (!(cond))                                                  \
        {                                                             \
            g_failures++;                                             \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        }                                                              \
    } while (0)

static void clear_fake_flash(void)
{
    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
}

static void clear_config_data(void)
{
    memset(&configData, 0, sizeof(configData));
}

// Test 1: blank flash, blank ConfigEntry -> factory defaults.
static void test_1_blank_everything_factory_defaults(void)
{
    printf("Test 1: blank flash + blank ConfigEntry -> factory defaults\n");
    clear_fake_flash();
    clear_config_data();
    g_test_loaded_from_real_flash = false;

    sidetnfs_system_config_init();

    CHECK(!sidetnfs_system_config_loaded_from_flash(), "not loaded from flash");
    CHECK(!sidetnfs_system_config_migrated_from_legacy(), "not migrated (nothing to migrate)");

    sidetnfs_system_settings_t s;
    sidetnfs_system_config_get(&s);
    CHECK(s.use_dhcp == 1, "factory default use_dhcp=1");
    CHECK(strcmp(s.country, "XX") == 0, "factory default country=XX");
    CHECK(strcmp(s.primary_dns, "8.8.8.8") == 0, "factory default dns=8.8.8.8");
    CHECK(s.rtc_enabled == 1, "factory default rtc_enabled=1");
    CHECK(strcmp(s.ntp_server, "pool.ntp.org") == 0, "factory default ntp_server=pool.ntp.org");
    CHECK(strcmp(s.utc_offset, "+1") == 0, "factory default utc_offset=+1");
    CHECK(s.ssid[0] == '\0', "factory default ssid empty");
}

// Test 2: blank flash, but legacy ConfigEntry has real, non-default
// values -> migration picks them up (RAM only, never written to flash).
static void test_2_migrate_real_legacy_values(void)
{
    printf("Test 2 (= Fase 2B Test E): blank flash + real legacy ConfigEntry values -> migrated\n");
    clear_fake_flash();
    clear_config_data();
    g_test_loaded_from_real_flash = true; // real legacy flash was actually found (config.c's own signal)
    set_entry(PARAM_WIFI_SSID, "MyRealNetwork");
    set_entry(PARAM_WIFI_PASSWORD, "hunter2");
    set_entry(PARAM_WIFI_AUTH, "4");
    set_entry(PARAM_WIFI_DHCP, "false");
    set_entry(PARAM_WIFI_COUNTRY, "NL");
    set_entry(PARAM_WIFI_IP, "192.168.1.50");
    set_entry(PARAM_WIFI_NETMASK, "255.255.255.0");
    set_entry(PARAM_WIFI_GATEWAY, "192.168.1.1");
    set_entry(PARAM_WIFI_DNS, "1.1.1.1");
    set_entry(PARAM_GEMDRIVE_RTC, "false");
    set_entry(PARAM_RTC_NTP_SERVER_HOST, "time.example.org");
    set_entry(PARAM_RTC_UTC_OFFSET, "-5");

    sidetnfs_system_config_init();

    CHECK(!sidetnfs_system_config_loaded_from_flash(), "migration is not 'loaded from flash'");
    CHECK(sidetnfs_system_config_migrated_from_legacy(), "flagged as migrated");

    sidetnfs_system_settings_t s;
    sidetnfs_system_config_get(&s);
    CHECK(strcmp(s.ssid, "MyRealNetwork") == 0, "ssid migrated");
    CHECK(strcmp(s.password, "hunter2") == 0, "password migrated");
    CHECK(s.auth_mode == 4, "auth_mode migrated");
    CHECK(s.use_dhcp == 0, "use_dhcp migrated (false)");
    CHECK(strcmp(s.country, "NL") == 0, "country migrated");
    CHECK(strcmp(s.ip_address, "192.168.1.50") == 0, "ip migrated");
    CHECK(strcmp(s.netmask, "255.255.255.0") == 0, "netmask migrated");
    CHECK(strcmp(s.gateway, "192.168.1.1") == 0, "gateway migrated");
    CHECK(strcmp(s.primary_dns, "1.1.1.1") == 0, "dns migrated");
    CHECK(s.rtc_enabled == 0, "rtc_enabled migrated (false)");
    CHECK(strcmp(s.ntp_server, "time.example.org") == 0, "ntp_server migrated");
    CHECK(strcmp(s.utc_offset, "-5") == 0, "utc_offset migrated");

    // Never written to flash by migration alone.
    const sidetnfs_system_flash_t *flash_ptr = (const sidetnfs_system_flash_t *)(g_fake_flash + SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET);
    CHECK(flash_ptr->magic != SIDETNFS_SYSTEM_CONFIG_MAGIC, "flash sector untouched by migration alone");
}

// Test 3: a valid flash block already exists -> used directly, no migration.
static void test_3_valid_flash_used_directly(void)
{
    printf("Test 3: valid flash block -> used directly, no migration\n");
    clear_fake_flash();
    clear_config_data();
    g_test_loaded_from_real_flash = true; // even with real legacy data present, flash must win
    set_entry(PARAM_WIFI_SSID, "ShouldNotBeUsed");

    sidetnfs_system_settings_t s;
    memset(&s, 0, sizeof(s));
    strncpy(s.ssid, "FlashSSID", sizeof(s.ssid) - 1);
    strncpy(s.country, "US", sizeof(s.country) - 1);
    strncpy(s.primary_dns, "8.8.8.8", sizeof(s.primary_dns) - 1);
    strncpy(s.ntp_server, "pool.ntp.org", sizeof(s.ntp_server) - 1);
    strncpy(s.utc_offset, "0", sizeof(s.utc_offset) - 1);
    s.use_dhcp = 1;
    s.rtc_enabled = 1;
    CHECK(sidetnfs_system_config_set(&s) == SIDETNFS_SYSCONFIG_STATUS_OK, "set() ok");
    CHECK(sidetnfs_system_config_save() == SIDETNFS_SYSCONFIG_STATUS_OK, "save() ok");

    // Fresh boot: re-init from the (now valid) fake flash.
    sidetnfs_system_config_init();
    CHECK(sidetnfs_system_config_loaded_from_flash(), "loaded from flash on second init");
    CHECK(!sidetnfs_system_config_migrated_from_legacy(), "not migrated -- flash took priority");

    sidetnfs_system_settings_t reloaded;
    sidetnfs_system_config_get(&reloaded);
    CHECK(strcmp(reloaded.ssid, "FlashSSID") == 0, "flash value used, not the legacy ConfigEntry value");
}

// Test 4: corrupt flash (bad CRC) + valid legacy data -> migration path taken.
static void test_4_corrupt_flash_falls_back_to_migration(void)
{
    printf("Test 4: corrupt flash + legacy data -> migration\n");
    clear_fake_flash();
    clear_config_data();
    g_test_loaded_from_real_flash = true;
    set_entry(PARAM_WIFI_SSID, "LegacyAfterCorruption");

    sidetnfs_system_flash_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.magic = SIDETNFS_SYSTEM_CONFIG_MAGIC;
    bad.version = SIDETNFS_SYSTEM_CONFIG_FLASH_VERSION;
    bad.crc32 = 0xDEADBEEF; // wrong on purpose
    memcpy(g_fake_flash + SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET, &bad, sizeof(bad));

    sidetnfs_system_config_init();
    CHECK(!sidetnfs_system_config_loaded_from_flash(), "corrupt flash not trusted");
    CHECK(sidetnfs_system_config_migrated_from_legacy(), "falls back to legacy migration");

    sidetnfs_system_settings_t s;
    sidetnfs_system_config_get(&s);
    CHECK(strcmp(s.ssid, "LegacyAfterCorruption") == 0, "legacy value used after corrupt-flash fallback");
}

// Fase 2B Test F: legacy store was NEVER actually configured -- configData
// still holds SOMETHING (config.c's load_default_entries() always seeds
// every key), but config_loaded_from_real_flash() is false, so this must
// be reported as a plain factory-default fallback, never "migrated". This
// is exactly the bug sidetnfs_system_config_migrate_from_legacy() used to
// have when it only checked configData.count (always nonzero).
static void test_F_never_configured_legacy_is_not_migration(void)
{
    printf("Test F: legacy store never really configured -> factory defaults, NOT 'migrated'\n");
    clear_fake_flash();
    clear_config_data();
    g_test_loaded_from_real_flash = false; // the critical bit: no real magic was ever found in legacy flash

    // Even though configData.count > 0 here (mirrors load_default_entries()
    // always seeding defaults before any real overlay is attempted) --
    // this must NOT be enough to claim a migration happened.
    set_entry(PARAM_WIFI_DHCP, "true");
    set_entry(PARAM_WIFI_DNS, "8.8.8.8");
    set_entry(PARAM_GEMDRIVE_RTC, "true");
    set_entry(PARAM_RTC_NTP_SERVER_HOST, "pool.ntp.org");
    set_entry(PARAM_RTC_UTC_OFFSET, "+1");
    CHECK(configData.count > 0, "sanity: configData is non-empty (mirrors load_default_entries() always seeding it)");

    sidetnfs_system_config_init();
    CHECK(!sidetnfs_system_config_loaded_from_flash(), "not loaded from flash");
    CHECK(!sidetnfs_system_config_migrated_from_legacy(),
          "NOT reported as migrated -- configData.count alone must never imply real legacy data");

    sidetnfs_system_settings_t s;
    sidetnfs_system_config_get(&s);
    CHECK(strcmp(s.ntp_server, "pool.ntp.org") == 0, "factory default value used (happens to match the legacy default too)");
}

// Test 5/6: netconfig-style and rtcconfig-style "read current, merge one
// group of fields, set, save" never clobbers the other group -- this is
// the exact pattern sidetnfs_netconfig_save()/sidetnfs_rtcconfig_save()
// use against this same module.
static void test_5_6_cross_field_preservation(void)
{
    printf("Test 5/6: netconfig-save and rtcconfig-save preserve each other's fields\n");
    clear_fake_flash();
    clear_config_data();
    sidetnfs_system_config_init(); // factory defaults

    // Simulate sidetnfs_netconfig_save(): change only WiFi/Network fields.
    sidetnfs_system_settings_t sys;
    sidetnfs_system_config_get(&sys);
    strncpy(sys.ssid, "NetconfigWrote", sizeof(sys.ssid) - 1);
    sys.ssid[sizeof(sys.ssid) - 1] = '\0';
    CHECK(sidetnfs_system_config_set(&sys) == SIDETNFS_SYSCONFIG_STATUS_OK, "netconfig-style set ok");
    CHECK(sidetnfs_system_config_save() == SIDETNFS_SYSCONFIG_STATUS_OK, "netconfig-style save ok");

    sidetnfs_system_settings_t after_net;
    sidetnfs_system_config_get(&after_net);
    CHECK(strcmp(after_net.ntp_server, "pool.ntp.org") == 0, "RTC fields (ntp_server) untouched by netconfig-style save");
    CHECK(strcmp(after_net.utc_offset, "+1") == 0, "RTC fields (utc_offset) untouched by netconfig-style save");
    CHECK(after_net.rtc_enabled == 1, "RTC fields (rtc_enabled) untouched by netconfig-style save");

    // Now simulate sidetnfs_rtcconfig_save(): change only RTC fields.
    sidetnfs_system_config_get(&sys);
    strncpy(sys.ntp_server, "RtcconfigWrote", sizeof(sys.ntp_server) - 1);
    sys.ntp_server[sizeof(sys.ntp_server) - 1] = '\0';
    CHECK(sidetnfs_system_config_set(&sys) == SIDETNFS_SYSCONFIG_STATUS_OK, "rtcconfig-style set ok");
    CHECK(sidetnfs_system_config_save() == SIDETNFS_SYSCONFIG_STATUS_OK, "rtcconfig-style save ok");

    sidetnfs_system_settings_t after_rtc;
    sidetnfs_system_config_get(&after_rtc);
    CHECK(strcmp(after_rtc.ssid, "NetconfigWrote") == 0, "WiFi fields (ssid) untouched by rtcconfig-style save");
    CHECK(strcmp(after_rtc.ntp_server, "RtcconfigWrote") == 0, "RTC field actually updated");

    // Reload from (fake) flash to confirm both changes actually persisted together.
    sidetnfs_system_config_init();
    sidetnfs_system_settings_t reloaded;
    sidetnfs_system_config_get(&reloaded);
    CHECK(strcmp(reloaded.ssid, "NetconfigWrote") == 0, "ssid persisted after reload");
    CHECK(strcmp(reloaded.ntp_server, "RtcconfigWrote") == 0, "ntp_server persisted after reload");
}

// Test 7: invalid structure rejected by set(), never persisted.
static void test_7_invalid_structure_rejected(void)
{
    printf("Test 7: invalid structure rejected by set()\n");
    clear_fake_flash();
    clear_config_data();
    sidetnfs_system_config_init();

    sidetnfs_system_settings_t before;
    sidetnfs_system_config_get(&before);

    sidetnfs_system_settings_t bad;
    sidetnfs_system_config_get(&bad);
    bad.use_dhcp = 5; // invalid -- must be 0 or 1

    CHECK(sidetnfs_system_config_set(&bad) == SIDETNFS_SYSCONFIG_STATUS_INVALID_STRUCTURE, "invalid use_dhcp rejected");

    sidetnfs_system_settings_t after;
    sidetnfs_system_config_get(&after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0, "active settings unchanged after rejected set()");
}

int main(void)
{
    test_1_blank_everything_factory_defaults();
    test_2_migrate_real_legacy_values();
    test_3_valid_flash_used_directly();
    test_4_corrupt_flash_falls_back_to_migration();
    test_F_never_configured_legacy_is_not_migration();
    test_5_6_cross_field_preservation();
    test_7_invalid_structure_rejected();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
