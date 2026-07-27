/**
 * File: sidetnfs_netconfig.c
 * Description: -- see sidetnfs_netconfig.h. storage
 * backend switched from the old ConfigEntry store (romemul/config.c) to
 * the independent sidetnfs_system_config module -- this file no longer
 * includes config.h or calls find_entry()/put_string()/put_bool()/
 * put_integer()/write_all_entries() at all. Wire-protocol struct,
 * validation rules, and RAM staging are all unchanged from before.
 */
#include "include/sidetnfs_netconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "include/sidetnfs_system_config.h"
#include "lwip/ip_addr.h"

static sidetnfs_network_config_t g_staging;
static bool g_staged = false;

void sidetnfs_netconfig_get(sidetnfs_network_config_t *out)
{
    memset(out, 0, sizeof(*out));

    sidetnfs_system_settings_t sys;
    sidetnfs_system_config_get(&sys);

    out->auth_mode = sys.auth_mode;
    out->use_dhcp = sys.use_dhcp;
    strncpy(out->ssid, sys.ssid, sizeof(out->ssid) - 1);
    strncpy(out->password, sys.password, sizeof(out->password) - 1);

    // An empty stored country means "never configured" --
    // normalized to "XX" for display/editing (sidetnfs_system_config's
    // own factory defaults already store "XX" for this same reason, so
    // this only matters for a legacy-migrated value that happened to be
    // empty).
    if (strlen(sys.country) > 0)
    {
        strncpy(out->country, sys.country, sizeof(out->country) - 1);
    }
    else
    {
        strncpy(out->country, "XX", sizeof(out->country) - 1);
    }

    strncpy(out->ip_address, sys.ip_address, sizeof(out->ip_address) - 1);
    strncpy(out->netmask, sys.netmask, sizeof(out->netmask) - 1);
    strncpy(out->gateway, sys.gateway, sizeof(out->gateway) - 1);
    strncpy(out->primary_dns, sys.primary_dns, sizeof(out->primary_dns) - 1);
}

// True if the field's NUL terminator was found strictly within its buffer
// (strnlen() < buffer size) -- the "aantoonbaar NUL-getermineerd" (provably
// NUL-terminated) requirement, checked the same way for every string field.
static bool is_nul_terminated(const char *field, size_t field_size)
{
    return strnlen(field, field_size) < field_size;
}

static bool is_valid_ipv4(const char *field, size_t field_size)
{
    if (!is_nul_terminated(field, field_size))
    {
        return false;
    }
    ip_addr_t tmp;
    return ipaddr_aton(field, &tmp) != 0;
}

sidetnfs_netconfig_status_t sidetnfs_netconfig_validate(const sidetnfs_network_config_t *cfg)
{
    if (!is_nul_terminated(cfg->ssid, sizeof(cfg->ssid)) || strlen(cfg->ssid) > 32)
    {
        return SIDETNFS_NETCONFIG_STATUS_INVALID_SSID;
    }
    if (!is_nul_terminated(cfg->password, sizeof(cfg->password)) || strlen(cfg->password) > 64)
    {
        return SIDETNFS_NETCONFIG_STATUS_INVALID_PASSWORD;
    }
    if (cfg->auth_mode > 8)
    {
        return SIDETNFS_NETCONFIG_STATUS_INVALID_AUTH_MODE;
    }

    // Country: exactly two letters, case-insensitive input, must be one of
    // the codes get_country_code() actually accepts (including "XX") --
    // reuses that function instead of duplicating its allow-list. A
    // request must always supply a real code (never empty) -- GET's own
    // empty->"XX" normalization is a display convenience, not something
    // SET/SAVE requests may rely on.
    if (!is_nul_terminated(cfg->country, sizeof(cfg->country)) || strlen(cfg->country) != 2)
    {
        return SIDETNFS_NETCONFIG_STATUS_INVALID_COUNTRY;
    }
    char upper_country[3] = {(char)toupper((unsigned char)cfg->country[0]),
                              (char)toupper((unsigned char)cfg->country[1]), '\0'};
    char *valid_country_str = NULL;
    get_country_code(upper_country, &valid_country_str);
    if (valid_country_str == NULL || strcmp(upper_country, valid_country_str) != 0)
    {
        return SIDETNFS_NETCONFIG_STATUS_INVALID_COUNTRY;
    }

    if (cfg->use_dhcp != 0 && cfg->use_dhcp != 1)
    {
        return SIDETNFS_NETCONFIG_STATUS_INVALID_DHCP;
    }

    // With DHCP on, the four static-network fields are simply
    // unused -- never re-validated (may be empty, stale, or anything
    // else). With DHCP off, every one of the four must be a real,
    // NUL-terminated IPv4 dotted-quad (ipaddr_aton(), the same lwIP
    // function already proven throughout sidetnfs_probe.c).
    if (cfg->use_dhcp == 0)
    {
        if (!is_valid_ipv4(cfg->ip_address, sizeof(cfg->ip_address)))
        {
            return SIDETNFS_NETCONFIG_STATUS_INVALID_IP;
        }
        if (!is_valid_ipv4(cfg->netmask, sizeof(cfg->netmask)))
        {
            return SIDETNFS_NETCONFIG_STATUS_INVALID_NETMASK;
        }
        if (!is_valid_ipv4(cfg->gateway, sizeof(cfg->gateway)))
        {
            return SIDETNFS_NETCONFIG_STATUS_INVALID_GATEWAY;
        }
        if (!is_valid_ipv4(cfg->primary_dns, sizeof(cfg->primary_dns)))
        {
            return SIDETNFS_NETCONFIG_STATUS_INVALID_DNS;
        }
    }

    return SIDETNFS_NETCONFIG_STATUS_OK;
}

sidetnfs_netconfig_status_t sidetnfs_netconfig_stage(const sidetnfs_network_config_t *cfg)
{
    sidetnfs_netconfig_status_t result = sidetnfs_netconfig_validate(cfg);
    if (result != SIDETNFS_NETCONFIG_STATUS_OK)
    {
        // G_staging/g_staged are untouched on any failure -- the
        // previous staging copy (if any) survives exactly as it was.
        return result;
    }
    g_staging = *cfg;
    g_staged = true;
    return SIDETNFS_NETCONFIG_STATUS_OK;
}

bool sidetnfs_netconfig_is_staged(void)
{
    return g_staged;
}

sidetnfs_netconfig_status_t sidetnfs_netconfig_save(void)
{
    if (!g_staged)
    {
        return SIDETNFS_NETCONFIG_STATUS_NOT_STAGED;
    }

    // Re-validate the staged copy in full before touching
    // sidetnfs_system_config at all -- defense in depth, since stage()
    // already validated it once, but nothing else in this module can
    // have mutated g_staging in between.
    sidetnfs_netconfig_status_t result = sidetnfs_netconfig_validate(&g_staging);
    if (result != SIDETNFS_NETCONFIG_STATUS_OK)
    {
        return result;
    }

    // Clean local copy: guarantees NUL-termination and uppercases country,
    // independent of what was already true of the (already-validated)
    // staging copy.
    sidetnfs_network_config_t clean = g_staging;
    clean.ssid[sizeof(clean.ssid) - 1] = '\0';
    clean.password[sizeof(clean.password) - 1] = '\0';
    clean.country[sizeof(clean.country) - 1] = '\0';
    clean.country[0] = (char)toupper((unsigned char)clean.country[0]);
    clean.country[1] = (char)toupper((unsigned char)clean.country[1]);
    clean.ip_address[sizeof(clean.ip_address) - 1] = '\0';
    clean.netmask[sizeof(clean.netmask) - 1] = '\0';
    clean.gateway[sizeof(clean.gateway) - 1] = '\0';
    clean.primary_dns[sizeof(clean.primary_dns) - 1] = '\0';

    // Read the current full system settings first, so the RTC
    // fields (owned by sidetnfs_rtcconfig.c, sharing the same underlying
    // sidetnfs_system_flash_t) are preserved exactly as they were --
    // this SAVE only ever touches the WiFi/Network fields.
    sidetnfs_system_settings_t sys;
    sidetnfs_system_config_get(&sys);

    sys.auth_mode = clean.auth_mode;
    sys.use_dhcp = clean.use_dhcp;
    strncpy(sys.ssid, clean.ssid, sizeof(sys.ssid) - 1);
    sys.ssid[sizeof(sys.ssid) - 1] = '\0';
    strncpy(sys.password, clean.password, sizeof(sys.password) - 1);
    sys.password[sizeof(sys.password) - 1] = '\0';
    strncpy(sys.country, clean.country, sizeof(sys.country) - 1);
    sys.country[sizeof(sys.country) - 1] = '\0';
    strncpy(sys.ip_address, clean.ip_address, sizeof(sys.ip_address) - 1);
    sys.ip_address[sizeof(sys.ip_address) - 1] = '\0';
    strncpy(sys.netmask, clean.netmask, sizeof(sys.netmask) - 1);
    sys.netmask[sizeof(sys.netmask) - 1] = '\0';
    strncpy(sys.gateway, clean.gateway, sizeof(sys.gateway) - 1);
    sys.gateway[sizeof(sys.gateway) - 1] = '\0';
    strncpy(sys.primary_dns, clean.primary_dns, sizeof(sys.primary_dns) - 1);
    sys.primary_dns[sizeof(sys.primary_dns) - 1] = '\0';

    sidetnfs_system_config_status_t set_result = sidetnfs_system_config_set(&sys);
    if (set_result != SIDETNFS_SYSCONFIG_STATUS_OK)
    {
        // Structure validation inside sidetnfs_system_config_set() is
        // deliberately light (see its own doc comment) -- this module
        // already validated everything semantically above, so reaching
        // this branch would indicate an internal inconsistency, not a
        // user input error. Reported as a write failure rather than
        // silently ignored.
        return SIDETNFS_NETCONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    sidetnfs_system_config_status_t save_result = sidetnfs_system_config_save();
    if (save_result == SIDETNFS_SYSCONFIG_STATUS_CRC_MISMATCH)
    {
        return SIDETNFS_NETCONFIG_STATUS_FLASH_VERIFY_FAILED;
    }
    if (save_result != SIDETNFS_SYSCONFIG_STATUS_OK)
    {
        return SIDETNFS_NETCONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    // Deliberately does NOT touch the active WiFi connection --
    // network_terminate()/network_init() are never called here. The new
    // configuration only takes effect on a later, separate apply/reinit
    // path. See docs/sidetnfs-config-protocol.md.
    return SIDETNFS_NETCONFIG_STATUS_OK;
}
