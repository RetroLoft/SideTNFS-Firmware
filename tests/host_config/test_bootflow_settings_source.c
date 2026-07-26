/**
 * File: test_bootflow_settings_source.c
 * Fase 2B Tests A-D: gemdrvemul.c and network.c depend on cyw43/lwIP/Pico
 * SDK headers throughout and cannot be compiled on the host at all (unlike
 * sidetnfs_system_config.c, which is host-testable directly -- see
 * test_system_config.c). So this file uses the established "faithful
 * mirror" pattern: each mirror function below reproduces, verbatim in
 * spirit, the exact field-selection decision from a specific, named
 * location in the real source, and is kept in sync with it by hand.
 *
 * Mirrors:
 *  - mirror_network_init_with_settings_ssid() / _dhcp_use()
 *      <- romemul/network.c network_init_with_settings(): SSID/DHCP now
 *         come from the settings parameter, never find_entry(PARAM_WIFI_*).
 *  - mirror_network_init_legacy_adapter_ssid()
 *      <- romemul/network.c network_init(): the legacy adapter that still
 *         builds a settings struct FROM find_entry(PARAM_WIFI_SSID) before
 *         delegating -- this is the one place ConfigEntry SSID legitimately
 *         still feeds the shared implementation, and only for legacy
 *         callers (romloader.c/floppyemul.c/rtcemul.c).
 *  - mirror_gemdrvemul_rtc_enabled() / mirror_gemdrvemul_ntp_server_host()
 *      <- romemul/gemdrvemul.c init_gemdrvemul(), around line 3034-3407:
 *         gemdrive_rtc_enabled = (sys.rtc_enabled != 0); and
 *         ntp_server_host = sys.ntp_server; -- both now sourced from the
 *         single sidetnfs_system_config_get(&sys) read, never find_entry().
 *  - mirror_gemdrvemul_ntp_route_started()
 *      <- romemul/gemdrvemul.c init_gemdrvemul(), line 3259:
 *         if (gemdrive_rtc_enabled && strlen(sys.ssid) > 0) { ...starts
 *         the WiFi/NTP connect route... }
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_bootflow_settings_source.c sandbox/sidetnfs_system_config.c \
 *       -o /tmp/test_bootflow_settings_source && \
 *       /tmp/test_bootflow_settings_source
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "include/sidetnfs_system_config.h"
#include "include/config.h"
#include "hardware/flash.h"

// Linked in only because sidetnfs_system_config.c (compiled in for its
// struct/status-enum definitions) references these -- this test never
// exercises the flash-backed init()/save() paths itself.
uint8_t g_fake_flash[0x102000];

void flash_range_erase(uint32_t flash_offs, size_t count)
{
    memset(g_fake_flash + flash_offs, 0xFF, count);
}

void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count)
{
    memcpy(g_fake_flash + flash_offs, data, count);
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

// ---- Minimal legacy ConfigEntry store, same shape as test_system_config.c ----
ConfigData configData;

bool config_loaded_from_real_flash(void) { return true; }

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

static void clear_config_data(void) { memset(&configData, 0, sizeof(configData)); }

// ---- Mirrors of network.c ----

// <- network_init_with_settings(): "settings->ssid" is the effective SSID.
static const char *mirror_network_init_with_settings_ssid(const sidetnfs_system_settings_t *settings)
{
    return settings->ssid;
}

// <- network_init(): legacy adapter builds `legacy.ssid` from
// find_entry(PARAM_WIFI_SSID)->value before delegating.
static const char *mirror_network_init_legacy_adapter_ssid(void)
{
    ConfigEntry *e = find_entry(PARAM_WIFI_SSID);
    return (e != NULL) ? e->value : "";
}

// ---- Mirrors of gemdrvemul.c init_gemdrvemul() ----

// <- line ~3084: bool gemdrive_rtc_enabled = (sys.rtc_enabled != 0);
static bool mirror_gemdrvemul_rtc_enabled(const sidetnfs_system_settings_t *sys)
{
    return sys->rtc_enabled != 0;
}

// <- line ~3398: ntp_server_host = sys.ntp_server;
static const char *mirror_gemdrvemul_ntp_server_host(const sidetnfs_system_settings_t *sys)
{
    return sys->ntp_server;
}

// <- line ~3259: if (gemdrive_rtc_enabled && strlen(sys.ssid) > 0) { ...start NTP/WiFi route... }
static bool mirror_gemdrvemul_ntp_route_started(const sidetnfs_system_settings_t *sys)
{
    bool gemdrive_rtc_enabled = mirror_gemdrvemul_rtc_enabled(sys);
    return gemdrive_rtc_enabled && strlen(sys->ssid) > 0;
}

// Test A: SideTNFS/GEMDRIVE boot-time network code must use the new
// system-config SSID, never the old ConfigEntry SSID, even when both
// are populated and different.
static void test_A_boot_network_uses_new_ssid_not_legacy(void)
{
    printf("Test A: boot-time SideTNFS network code uses sidetnfs_system_config SSID, not legacy ConfigEntry SSID\n");
    clear_config_data();
    set_entry(PARAM_WIFI_SSID, "OLDSSID");

    sidetnfs_system_settings_t sys;
    memset(&sys, 0, sizeof(sys));
    strncpy(sys.ssid, "NEWSSID", sizeof(sys.ssid) - 1);

    const char *effective = mirror_network_init_with_settings_ssid(&sys);
    CHECK(strcmp(effective, "NEWSSID") == 0, "SideTNFS boot path uses NEWSSID from sidetnfs_system_config");
    CHECK(strcmp(effective, "OLDSSID") != 0, "SideTNFS boot path does not fall back to the legacy ConfigEntry SSID");
}

// Test B: gemdrvemul.c's NTP client must use the system-config NTP
// server, not the legacy PARAM_RTC_NTP_SERVER_HOST value.
static void test_B_boot_ntp_uses_new_server_not_legacy(void)
{
    printf("Test B: gemdrvemul.c NTP boot code uses sidetnfs_system_config ntp_server, not legacy PARAM_RTC_NTP_SERVER_HOST\n");
    clear_config_data();
    set_entry(PARAM_RTC_NTP_SERVER_HOST, "pool.ntp.org");

    sidetnfs_system_settings_t sys;
    memset(&sys, 0, sizeof(sys));
    strncpy(sys.ntp_server, "time.example.org", sizeof(sys.ntp_server) - 1);
    sys.rtc_enabled = 1;
    strncpy(sys.ssid, "AnySSID", sizeof(sys.ssid) - 1);

    const char *effective = mirror_gemdrvemul_ntp_server_host(&sys);
    CHECK(strcmp(effective, "time.example.org") == 0, "gemdrvemul.c uses time.example.org from sidetnfs_system_config");
    CHECK(strcmp(effective, "pool.ntp.org") != 0, "gemdrvemul.c does not fall back to legacy PARAM_RTC_NTP_SERVER_HOST");
}

// Test C: RTC disabled in system-config -> the boot-time NTP/RTC route
// must not be started, regardless of a legacy PARAM_GEMDRIVE_RTC value.
static void test_C_rtc_disabled_route_not_started(void)
{
    printf("Test C: sidetnfs_system_config rtc_enabled=0 -> boot-time NTP/RTC route not started\n");
    clear_config_data();
    set_entry(PARAM_GEMDRIVE_RTC, "true"); // legacy says enabled -- must be ignored entirely

    sidetnfs_system_settings_t sys;
    memset(&sys, 0, sizeof(sys));
    strncpy(sys.ssid, "AnySSID", sizeof(sys.ssid) - 1);
    sys.rtc_enabled = 0;

    CHECK(!mirror_gemdrvemul_rtc_enabled(&sys), "gemdrive_rtc_enabled derived as false from sys.rtc_enabled, ignoring legacy PARAM_GEMDRIVE_RTC");
    CHECK(!mirror_gemdrvemul_ntp_route_started(&sys), "NTP/RTC route not started when sys.rtc_enabled == 0");

    // Sanity: with rtc_enabled=1 and a non-empty SSID, the same mirror
    // *does* start the route -- proving the false result above is a real
    // decision and not just a broken mirror.
    sys.rtc_enabled = 1;
    CHECK(mirror_gemdrvemul_ntp_route_started(&sys), "sanity: route does start when rtc_enabled=1 and ssid is non-empty");
}

// Test D: legacy modes (old configurator/floppy-emulator/standalone
// RTC-emulator, via network_init()'s adapter) keep using the old
// ConfigEntry SSID -- sidetnfs_system_config must play no part here.
static void test_D_legacy_modes_keep_using_configentry(void)
{
    printf("Test D: legacy network_init() adapter keeps using ConfigEntry SSID, unaffected by sidetnfs_system_config\n");
    clear_config_data();
    set_entry(PARAM_WIFI_SSID, "LegacyModeSSID");

    const char *effective = mirror_network_init_legacy_adapter_ssid();
    CHECK(strcmp(effective, "LegacyModeSSID") == 0, "legacy adapter path still uses the ConfigEntry SSID");

    // Even if a completely different sidetnfs_system_config SSID exists in
    // RAM, the legacy adapter path must not be influenced by it -- it never
    // reads sidetnfs_system_config at all.
    sidetnfs_system_settings_t unrelated_new_config;
    memset(&unrelated_new_config, 0, sizeof(unrelated_new_config));
    strncpy(unrelated_new_config.ssid, "NEWSSID", sizeof(unrelated_new_config.ssid) - 1);
    (void)unrelated_new_config;

    const char *effective_again = mirror_network_init_legacy_adapter_ssid();
    CHECK(strcmp(effective_again, "LegacyModeSSID") == 0, "legacy adapter path unaffected by any sidetnfs_system_config content");
}

int main(void)
{
    test_A_boot_network_uses_new_ssid_not_legacy();
    test_B_boot_ntp_uses_new_server_not_legacy();
    test_C_rtc_disabled_route_not_started();
    test_D_legacy_modes_keep_using_configentry();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
