/**
 * File: sidetnfs_netconfig.c
 * Description: Fase 11A -- see sidetnfs_netconfig.h. Reuses configData
 * (romemul/config.c) and the existing PARAM_WIFI_* entries/8KB CONFIG_FLASH
 * sector; never a second flash sector. GET/validate never touch flash;
 * SAVE is the only function here that does (via write_all_entries(),
 * itself fixed in this same phase for its own 256-byte program-length
 * requirement -- see config.c).
 */
#include "include/sidetnfs_netconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "include/config.h"
#include "lwip/ip_addr.h"

static sidetnfs_network_config_t g_staging;
static bool g_staged = false;

static void get_string_field(const char *key, char *out, size_t out_size)
{
    ConfigEntry *e = find_entry(key);
    if (e != NULL)
    {
        strncpy(out, e->value, out_size - 1);
    }
    out[out_size - 1] = '\0';
}

void sidetnfs_netconfig_get(sidetnfs_network_config_t *out)
{
    memset(out, 0, sizeof(*out));

    ConfigEntry *auth_entry = find_entry(PARAM_WIFI_AUTH);
    out->auth_mode = (uint16_t)((auth_entry != NULL && strlen(auth_entry->value) > 0) ? atoi(auth_entry->value) : 0);

    ConfigEntry *dhcp_entry = find_entry(PARAM_WIFI_DHCP);
    out->use_dhcp = (uint16_t)((dhcp_entry != NULL && (dhcp_entry->value[0] == 't' || dhcp_entry->value[0] == 'T')) ? 1 : 0);

    get_string_field(PARAM_WIFI_SSID, out->ssid, sizeof(out->ssid));
    get_string_field(PARAM_WIFI_PASSWORD, out->password, sizeof(out->password));

    ConfigEntry *country_entry = find_entry(PARAM_WIFI_COUNTRY);
    if (country_entry != NULL && strlen(country_entry->value) > 0)
    {
        strncpy(out->country, country_entry->value, sizeof(out->country) - 1);
        out->country[sizeof(out->country) - 1] = '\0';
    }
    else
    {
        // Fase 11A: an empty stored country means "never configured" --
        // normalized to "XX" for display/editing, same worldwide default
        // get_country_code() itself falls back to. The flash entry itself
        // is left untouched (this is a read-only command).
        strncpy(out->country, "XX", sizeof(out->country) - 1);
    }

    get_string_field(PARAM_WIFI_IP, out->ip_address, sizeof(out->ip_address));
    get_string_field(PARAM_WIFI_NETMASK, out->netmask, sizeof(out->netmask));
    get_string_field(PARAM_WIFI_GATEWAY, out->gateway, sizeof(out->gateway));
    get_string_field(PARAM_WIFI_DNS, out->primary_dns, sizeof(out->primary_dns));
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

    // Fase 11A: with DHCP on, the four static-network fields are simply
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
        // Fase 11A: g_staging/g_staged are untouched on any failure -- the
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

// Linear lookup against an arbitrary ConfigData block (e.g. a flash
// readback), not the live global configData -- find_entry() in config.c
// only ever searches the live global, so SAVE's readback verification
// needs its own copy of the same simple scan.
static ConfigEntry *find_entry_in(ConfigData *data, const char *key)
{
    for (size_t i = 0; i < data->count; i++)
    {
        if (strncmp(data->entries[i].key, key, MAX_KEY_LENGTH) == 0)
        {
            return &data->entries[i];
        }
    }
    return NULL;
}

static bool readback_field_matches(ConfigData *readback, const char *key, const char *expected)
{
    ConfigEntry *entry = find_entry_in(readback, key);
    return entry != NULL && strcmp(entry->value, expected) == 0;
}

sidetnfs_netconfig_status_t sidetnfs_netconfig_save(void)
{
    if (!g_staged)
    {
        return SIDETNFS_NETCONFIG_STATUS_NOT_STAGED;
    }

    // Fase 11A: re-validate the staged copy in full before touching
    // configData/flash at all -- defense in depth, since stage() already
    // validated it once, but nothing else in this module can have
    // mutated g_staging in between.
    sidetnfs_netconfig_status_t result = sidetnfs_netconfig_validate(&g_staging);
    if (result != SIDETNFS_NETCONFIG_STATUS_OK)
    {
        return result;
    }

    // Clean local copy: guarantees NUL-termination and uppercases country,
    // independent of what was already true of the (already-validated)
    // staging copy -- same "build a clean temp before writing anything
    // real" shape sidetnfs_config_save() uses for its own clean/candidate
    // struct.
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

    char auth_str[8];
    snprintf(auth_str, sizeof(auth_str), "%d", (int)clean.auth_mode);
    const char *dhcp_str = clean.use_dhcp ? "true" : "false";

    put_integer(PARAM_WIFI_AUTH, (int)clean.auth_mode);
    put_bool(PARAM_WIFI_DHCP, clean.use_dhcp != 0);
    put_string(PARAM_WIFI_SSID, clean.ssid);
    put_string(PARAM_WIFI_PASSWORD, clean.password);
    put_string(PARAM_WIFI_COUNTRY, clean.country);
    put_string(PARAM_WIFI_IP, clean.ip_address);
    put_string(PARAM_WIFI_NETMASK, clean.netmask);
    put_string(PARAM_WIFI_GATEWAY, clean.gateway);
    put_string(PARAM_WIFI_DNS, clean.primary_dns);

    if (write_all_entries() != 0)
    {
        return SIDETNFS_NETCONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    // Real XIP readback -- re-read the bytes actually committed to flash
    // (not the live in-RAM configData, which trivially already matches
    // what was just written) -- same proof-of-write sidetnfs_config_save()
    // performs for its own, separate flash sector. static: sizeof(ConfigData)
    // is far too large for the 4KB stack (see config.c's write_all_entries()
    // report note for the same reasoning).
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    static ConfigData readback;
    memcpy(&readback, flash_ptr, sizeof(readback));

    bool ok = true;
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_AUTH, auth_str);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_DHCP, dhcp_str);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_SSID, clean.ssid);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_PASSWORD, clean.password);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_COUNTRY, clean.country);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_IP, clean.ip_address);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_NETMASK, clean.netmask);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_GATEWAY, clean.gateway);
    ok = ok && readback_field_matches(&readback, PARAM_WIFI_DNS, clean.primary_dns);

    if (!ok)
    {
        return SIDETNFS_NETCONFIG_STATUS_FLASH_VERIFY_FAILED;
    }

    // Fase 11A: deliberately does NOT touch the active WiFi connection --
    // network_terminate()/network_init() are never called here. The new
    // configuration only takes effect on a later, separate apply/reinit
    // path (not part of this phase). See docs/sidetnfs-config-protocol.md.
    return SIDETNFS_NETCONFIG_STATUS_OK;
}
