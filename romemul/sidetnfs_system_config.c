/**
 * File: sidetnfs_system_config.c
 * Description: -- see sidetnfs_system_config.h. Only
 * sidetnfs_system_config_save() ever touches flash (erase+program);
 * every other function here only ever mutates the RAM copy. The only
 * dependency on the old ConfigEntry store (romemul/config.c) is the
 * one-time, read-only migration path inside sidetnfs_system_config_init()
 * -- nothing here ever calls put_string()/put_bool()/put_integer()/
 * write_all_entries().
 */
#include "include/sidetnfs_system_config.h"

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

// Read-only legacy bridge, used exactly once per boot (inside
// sidetnfs_system_config_init(), only when no valid flash block exists
// yet) to migrate PARAM_WIFI_*/PARAM_GEMDRIVE_RTC/PARAM_RTC_* out of the
// old ConfigEntry store. This is the ONLY file in the sidetnfs_system_config/
// sidetnfs_netconfig/sidetnfs_rtcconfig group that still includes
// config.h, and it never writes through it.
#include "include/config.h"

// Same built-in defaults romemul/config.c's own defaultEntries[]
// table ships (PARAM_WIFI_DHCP=true, PARAM_WIFI_DNS=8.8.8.8,
// PARAM_GEMDRIVE_RTC=true, PARAM_RTC_NTP_SERVER_HOST=pool.ntp.org,
// PARAM_RTC_UTC_OFFSET=+1, everything else empty) -- used only when
// neither a valid flash block nor a usable legacy ConfigEntry value
// exists.
static const sidetnfs_system_settings_t SIDETNFS_SYSTEM_DEFAULT_SETTINGS = {
    .auth_mode = 0,
    .use_dhcp = 1,
    .ssid = {0},
    .password = {0},
    .country = "XX",
    .ip_address = {0},
    .netmask = {0},
    .gateway = {0},
    .primary_dns = "8.8.8.8",
    .rtc_enabled = 1,
    .ntp_server = "pool.ntp.org",
    .utc_offset = "+1",
    .reserved = {0},
};

static sidetnfs_system_settings_t g_settings;
static bool g_ready = false;
static bool g_loaded_from_flash = false;
static bool g_migrated_from_legacy = false;

static uint32_t sidetnfs_system_crc32(const uint8_t *data, size_t len)
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

static void sidetnfs_system_config_force_nul_termination(sidetnfs_system_settings_t *s)
{
    s->ssid[sizeof(s->ssid) - 1] = '\0';
    s->password[sizeof(s->password) - 1] = '\0';
    s->country[sizeof(s->country) - 1] = '\0';
    s->ip_address[sizeof(s->ip_address) - 1] = '\0';
    s->netmask[sizeof(s->netmask) - 1] = '\0';
    s->gateway[sizeof(s->gateway) - 1] = '\0';
    s->primary_dns[sizeof(s->primary_dns) - 1] = '\0';
    s->ntp_server[sizeof(s->ntp_server) - 1] = '\0';
    s->utc_offset[sizeof(s->utc_offset) - 1] = '\0';
}

// Deliberately light -- NUL-termination and basic range sanity
// only. Semantic validation (country allow-list, IPv4 parsing, ntp_server
// character rules, utc_offset numeric range) is sidetnfs_netconfig.c's/
// sidetnfs_rtcconfig.c's own existing job, run on their own wire structs
// before a value is ever handed to sidetnfs_system_config_set(). This
// keeps that semantic logic in exactly one place (unchanged from before
// this phase) instead of duplicating it here.
static bool sidetnfs_system_config_validate_structure(const sidetnfs_system_settings_t *s)
{
    if (strnlen(s->ssid, sizeof(s->ssid)) >= sizeof(s->ssid)) return false;
    if (strnlen(s->password, sizeof(s->password)) >= sizeof(s->password)) return false;
    if (strnlen(s->country, sizeof(s->country)) >= sizeof(s->country)) return false;
    if (strnlen(s->ip_address, sizeof(s->ip_address)) >= sizeof(s->ip_address)) return false;
    if (strnlen(s->netmask, sizeof(s->netmask)) >= sizeof(s->netmask)) return false;
    if (strnlen(s->gateway, sizeof(s->gateway)) >= sizeof(s->gateway)) return false;
    if (strnlen(s->primary_dns, sizeof(s->primary_dns)) >= sizeof(s->primary_dns)) return false;
    if (strnlen(s->ntp_server, sizeof(s->ntp_server)) >= sizeof(s->ntp_server)) return false;
    if (strnlen(s->utc_offset, sizeof(s->utc_offset)) >= sizeof(s->utc_offset)) return false;

    if (s->use_dhcp != 0 && s->use_dhcp != 1) return false;
    if (s->rtc_enabled != 0 && s->rtc_enabled != 1) return false;
    if (s->auth_mode > 8) return false;

    return true;
}

// One-time, read-only migration from the old ConfigEntry store
// (romemul/config.c) -- mirrors sidetnfs_netconfig_get()'s/
// sidetnfs_rtcconfig_get()'s own existing field-by-field reads exactly
// (including their same fallback values), so a board upgrading from a
// Pre-build sees byte-for-byte the same effective settings it had
// before, just now sourced from configData instead of a valid
// sidetnfs_system_flash_t. Returns false (out untouched) if the legacy
// store was never actually configured -- see main.c's load_all_entries()
// call, which must run before this. uses
// config_loaded_from_real_flash() (config.c), NOT configData.count -- the
// latter is always nonzero regardless (load_all_entries() unconditionally
// seeds every PARAM_* key with its own default before ever attempting to
// overlay real flash values on top), so it could never actually tell a
// genuinely-never-configured board apart from one with real, saved
// legacy settings. config_loaded_from_real_flash() is the one signal
// config.c exposes that is true only when a matching magic was actually
// found in flash.
static bool sidetnfs_system_config_migrate_from_legacy(sidetnfs_system_settings_t *out)
{
    if (!config_loaded_from_real_flash())
    {
        return false;
    }

    *out = SIDETNFS_SYSTEM_DEFAULT_SETTINGS;

    ConfigEntry *auth_entry = find_entry(PARAM_WIFI_AUTH);
    out->auth_mode = (uint16_t)((auth_entry != NULL && strlen(auth_entry->value) > 0) ? atoi(auth_entry->value) : 0);

    ConfigEntry *dhcp_entry = find_entry(PARAM_WIFI_DHCP);
    out->use_dhcp = (uint16_t)((dhcp_entry != NULL && (dhcp_entry->value[0] == 't' || dhcp_entry->value[0] == 'T')) ? 1 : 0);

    ConfigEntry *ssid_entry = find_entry(PARAM_WIFI_SSID);
    if (ssid_entry != NULL) strncpy(out->ssid, ssid_entry->value, sizeof(out->ssid) - 1);

    ConfigEntry *password_entry = find_entry(PARAM_WIFI_PASSWORD);
    if (password_entry != NULL) strncpy(out->password, password_entry->value, sizeof(out->password) - 1);

    ConfigEntry *country_entry = find_entry(PARAM_WIFI_COUNTRY);
    if (country_entry != NULL && strlen(country_entry->value) > 0)
    {
        strncpy(out->country, country_entry->value, sizeof(out->country) - 1);
    }

    ConfigEntry *ip_entry = find_entry(PARAM_WIFI_IP);
    if (ip_entry != NULL) strncpy(out->ip_address, ip_entry->value, sizeof(out->ip_address) - 1);

    ConfigEntry *netmask_entry = find_entry(PARAM_WIFI_NETMASK);
    if (netmask_entry != NULL) strncpy(out->netmask, netmask_entry->value, sizeof(out->netmask) - 1);

    ConfigEntry *gateway_entry = find_entry(PARAM_WIFI_GATEWAY);
    if (gateway_entry != NULL) strncpy(out->gateway, gateway_entry->value, sizeof(out->gateway) - 1);

    ConfigEntry *dns_entry = find_entry(PARAM_WIFI_DNS);
    if (dns_entry != NULL) strncpy(out->primary_dns, dns_entry->value, sizeof(out->primary_dns) - 1);

    ConfigEntry *rtc_entry = find_entry(PARAM_GEMDRIVE_RTC);
    if (rtc_entry != NULL)
    {
        out->rtc_enabled = (uint16_t)((rtc_entry->value[0] == 't' || rtc_entry->value[0] == 'T') ? 1 : 0);
    }

    ConfigEntry *ntp_host_entry = find_entry(PARAM_RTC_NTP_SERVER_HOST);
    if (ntp_host_entry != NULL && strlen(ntp_host_entry->value) > 0)
    {
        strncpy(out->ntp_server, ntp_host_entry->value, sizeof(out->ntp_server) - 1);
    }

    ConfigEntry *utc_entry = find_entry(PARAM_RTC_UTC_OFFSET);
    if (utc_entry != NULL && strlen(utc_entry->value) > 0)
    {
        strncpy(out->utc_offset, utc_entry->value, sizeof(out->utc_offset) - 1);
    }

    sidetnfs_system_config_force_nul_termination(out);
    return true;
}

void sidetnfs_system_config_init(void)
{
    const sidetnfs_system_flash_t *flash_ptr =
        (const sidetnfs_system_flash_t *)(XIP_BASE + SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET);
    static sidetnfs_system_flash_t candidate;
    memcpy(&candidate, flash_ptr, sizeof(candidate));

    bool valid = (candidate.magic == SIDETNFS_SYSTEM_CONFIG_MAGIC);
    if (valid && candidate.version != SIDETNFS_SYSTEM_CONFIG_FLASH_VERSION)
    {
        valid = false;
    }
    if (valid)
    {
        uint32_t computed = sidetnfs_system_crc32((const uint8_t *)&candidate, offsetof(sidetnfs_system_flash_t, crc32));
        valid = (computed == candidate.crc32);
    }
    if (valid)
    {
        valid = sidetnfs_system_config_validate_structure(&candidate.settings);
    }

    if (valid)
    {
        g_settings = candidate.settings;
        sidetnfs_system_config_force_nul_termination(&g_settings);
        g_loaded_from_flash = true;
        g_migrated_from_legacy = false;
        g_ready = true;
        return;
    }

    // Never a partial recovery -- either a full legacy migration
    // (RAM only) or full factory defaults, never a mix.
    if (sidetnfs_system_config_migrate_from_legacy(&g_settings))
    {
        g_loaded_from_flash = false;
        g_migrated_from_legacy = true;
    }
    else
    {
        g_settings = SIDETNFS_SYSTEM_DEFAULT_SETTINGS;
        g_loaded_from_flash = false;
        g_migrated_from_legacy = false;
    }
    g_ready = true;
}

bool sidetnfs_system_config_loaded_from_flash(void)
{
    return g_loaded_from_flash;
}

bool sidetnfs_system_config_migrated_from_legacy(void)
{
    return g_migrated_from_legacy;
}

void sidetnfs_system_config_get(sidetnfs_system_settings_t *out)
{
    if (!g_ready)
    {
        *out = SIDETNFS_SYSTEM_DEFAULT_SETTINGS;
        return;
    }
    *out = g_settings;
}

sidetnfs_system_config_status_t sidetnfs_system_config_set(const sidetnfs_system_settings_t *in)
{
    sidetnfs_system_settings_t candidate = *in;
    sidetnfs_system_config_force_nul_termination(&candidate);

    if (!sidetnfs_system_config_validate_structure(&candidate))
    {
        return SIDETNFS_SYSCONFIG_STATUS_INVALID_STRUCTURE;
    }

    g_settings = candidate;
    g_ready = true;
    return SIDETNFS_SYSCONFIG_STATUS_OK;
}

#define SIDETNFS_SYSTEM_CONFIG_PROGRAM_SIZE \
    (((sizeof(sidetnfs_system_flash_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)
_Static_assert(SIDETNFS_SYSTEM_CONFIG_PROGRAM_SIZE <= SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE,
               "SIDETNFS_SYSTEM_CONFIG_PROGRAM_SIZE no longer fits in one SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE sector");

sidetnfs_system_config_status_t sidetnfs_system_config_save(void)
{
    if (!sidetnfs_system_config_validate_structure(&g_settings))
    {
        return SIDETNFS_SYSCONFIG_STATUS_INVALID_STRUCTURE;
    }

    static sidetnfs_system_flash_t clean;
    memset(&clean, 0, sizeof(clean));
    clean.magic = SIDETNFS_SYSTEM_CONFIG_MAGIC;
    clean.version = SIDETNFS_SYSTEM_CONFIG_FLASH_VERSION;
    clean.settings = g_settings;
    sidetnfs_system_config_force_nul_termination(&clean.settings);
    clean.crc32 = sidetnfs_system_crc32((const uint8_t *)&clean, offsetof(sidetnfs_system_flash_t, crc32));

    static uint8_t program_buf[SIDETNFS_SYSTEM_CONFIG_PROGRAM_SIZE];
    memset(program_buf, 0, sizeof(program_buf));
    memcpy(program_buf, &clean, sizeof(clean));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET, SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE);
    flash_range_program(SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET, program_buf, sizeof(program_buf));
    restore_interrupts(ints);

    const sidetnfs_system_flash_t *flash_ptr =
        (const sidetnfs_system_flash_t *)(XIP_BASE + SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET);
    static sidetnfs_system_flash_t readback;
    memcpy(&readback, flash_ptr, sizeof(readback));

    if (readback.magic != SIDETNFS_SYSTEM_CONFIG_MAGIC || readback.version != SIDETNFS_SYSTEM_CONFIG_FLASH_VERSION)
    {
        return SIDETNFS_SYSCONFIG_STATUS_FLASH_WRITE_FAILED;
    }
    uint32_t computed = sidetnfs_system_crc32((const uint8_t *)&readback, offsetof(sidetnfs_system_flash_t, crc32));
    if (computed != readback.crc32)
    {
        return SIDETNFS_SYSCONFIG_STATUS_CRC_MISMATCH;
    }

    g_settings = readback.settings;
    g_loaded_from_flash = true;
    g_migrated_from_legacy = false;
    g_ready = true;
    return SIDETNFS_SYSCONFIG_STATUS_OK;
}
