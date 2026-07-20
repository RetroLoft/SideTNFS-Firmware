/**
 * File: sidetnfs_rtcconfig.c
 * Description: Fase 12A -- see sidetnfs_rtcconfig.h. Reuses configData
 * (romemul/config.c) and the existing GEMDRIVE_RTC/RTC_NTP_SERVER_HOST/
 * RTC_UTC_OFFSET entries/8KB CONFIG_FLASH sector; never a second flash
 * sector, never a new PARAM_* key. GET/validate never touch flash; SAVE
 * is the only function here that does (via write_all_entries()).
 */
#include "include/sidetnfs_rtcconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/config.h"

static sidetnfs_rtc_config_t g_staging;
static bool g_staged = false;

static bool is_nul_terminated(const char *field, size_t field_size)
{
    return strnlen(field, field_size) < field_size;
}

// Strict "-12..+14, whole hours" parser: optional leading '+'/'-', 1-2
// digits, nothing else. Rejects empty strings, a bare sign, extra
// characters, and anything outside the valid range. Shared by validate()
// (format/range check only) and normalize_utc_offset() (reformat into
// the canonical "0"/"+N"/"-N" form after a field already known to be
// valid).
static bool parse_utc_offset(const char *field, size_t field_size, int *out_value)
{
    if (!is_nul_terminated(field, field_size))
    {
        return false;
    }

    const char *p = field;
    int sign = 1;
    if (*p == '+')
    {
        p++;
    }
    else if (*p == '-')
    {
        sign = -1;
        p++;
    }

    if (*p == '\0')
    {
        // Empty, or just a sign with no digits.
        return false;
    }

    int value = 0;
    int digit_count = 0;
    while (*p != '\0')
    {
        if (!isdigit((unsigned char)*p))
        {
            return false;
        }
        value = value * 10 + (*p - '0');
        digit_count++;
        if (digit_count > 2)
        {
            // -12..+14 never needs more than 2 digits; also guards
            // against overflow on a longer run of digits.
            return false;
        }
        p++;
    }

    value *= sign;
    if (value < -12 || value > 14)
    {
        return false;
    }

    *out_value = value;
    return true;
}

// Reformats an already-valid utc_offset field in place to the canonical
// "0" / "+N" / "-N" form (no leading zeros, no "+0"). Only ever called
// on a field that just passed parse_utc_offset() -- the parse here is
// expected to always succeed.
static void normalize_utc_offset(char *field, size_t field_size)
{
    int value = 0;
    if (!parse_utc_offset(field, field_size, &value))
    {
        // Unreachable in practice (see callers), but never leave the
        // field in a half-written state if it somehow is.
        return;
    }

    if (value == 0)
    {
        snprintf(field, field_size, "0");
    }
    else if (value > 0)
    {
        snprintf(field, field_size, "+%d", value);
    }
    else
    {
        snprintf(field, field_size, "%d", value); // "%d" already emits the leading '-'
    }
}

void sidetnfs_rtcconfig_get(sidetnfs_rtc_config_t *out)
{
    memset(out, 0, sizeof(*out));

    // Fase 12A: mirrors gemdrvemul.c's own gemdrive_rtc_enabled parsing
    // exactly (init_gemdrvemul(), PARAM_GEMDRIVE_RTC) -- an entry that
    // exists but isn't "true"/"True" is 0, not defaulted to 1. The 1
    // default only applies when the entry is missing entirely (never
    // expected -- config.c's defaultEntries[] always seeds it).
    ConfigEntry *rtc_entry = find_entry(PARAM_GEMDRIVE_RTC);
    if (rtc_entry != NULL)
    {
        out->enabled = (uint16_t)((rtc_entry->value[0] == 't' || rtc_entry->value[0] == 'T') ? 1 : 0);
    }
    else
    {
        out->enabled = 1;
    }

    ConfigEntry *host_entry = find_entry(PARAM_RTC_NTP_SERVER_HOST);
    if (host_entry != NULL && strlen(host_entry->value) > 0)
    {
        strncpy(out->ntp_server, host_entry->value, sizeof(out->ntp_server) - 1);
    }
    else
    {
        strncpy(out->ntp_server, "pool.ntp.org", sizeof(out->ntp_server) - 1);
    }
    out->ntp_server[sizeof(out->ntp_server) - 1] = '\0';

    ConfigEntry *offset_entry = find_entry(PARAM_RTC_UTC_OFFSET);
    if (offset_entry != NULL && strlen(offset_entry->value) > 0)
    {
        strncpy(out->utc_offset, offset_entry->value, sizeof(out->utc_offset) - 1);
    }
    else
    {
        strncpy(out->utc_offset, "+1", sizeof(out->utc_offset) - 1);
    }
    out->utc_offset[sizeof(out->utc_offset) - 1] = '\0';
}

sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_validate(const sidetnfs_rtc_config_t *cfg)
{
    if (cfg->enabled != 0 && cfg->enabled != 1)
    {
        return SIDETNFS_RTCCONFIG_STATUS_INVALID_ENABLED;
    }

    if (!is_nul_terminated(cfg->ntp_server, sizeof(cfg->ntp_server)))
    {
        return SIDETNFS_RTCCONFIG_STATUS_INVALID_NTP_SERVER;
    }
    size_t ntp_len = strlen(cfg->ntp_server);
    if (cfg->enabled == 1 && ntp_len == 0)
    {
        // Required only when enabled -- with enabled == 0, an empty (or
        // any other already-NUL-terminated, space-free) value is
        // accepted as-is; this protocol never force-clears the server.
        return SIDETNFS_RTCCONFIG_STATUS_INVALID_NTP_SERVER;
    }
    for (size_t i = 0; i < ntp_len; i++)
    {
        if (cfg->ntp_server[i] == ' ')
        {
            return SIDETNFS_RTCCONFIG_STATUS_INVALID_NTP_SERVER;
        }
    }

    int offset_value = 0;
    if (!parse_utc_offset(cfg->utc_offset, sizeof(cfg->utc_offset), &offset_value))
    {
        return SIDETNFS_RTCCONFIG_STATUS_INVALID_UTC_OFFSET;
    }

    return SIDETNFS_RTCCONFIG_STATUS_OK;
}

sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_stage(const sidetnfs_rtc_config_t *cfg)
{
    sidetnfs_rtcconfig_status_t result = sidetnfs_rtcconfig_validate(cfg);
    if (result != SIDETNFS_RTCCONFIG_STATUS_OK)
    {
        // Fase 12A: g_staging/g_staged are untouched on any failure --
        // the previous staging copy (if any) survives exactly as it was.
        return result;
    }
    g_staging = *cfg;
    normalize_utc_offset(g_staging.utc_offset, sizeof(g_staging.utc_offset));
    g_staged = true;
    return SIDETNFS_RTCCONFIG_STATUS_OK;
}

bool sidetnfs_rtcconfig_is_staged(void)
{
    return g_staged;
}

// Linear lookup against an arbitrary ConfigData block (e.g. a flash
// readback), not the live global configData -- find_entry() in config.c
// only ever searches the live global, so SAVE's readback verification
// needs its own copy of the same simple scan. Same pattern as
// sidetnfs_netconfig.c's own find_entry_in()/readback_field_matches().
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

sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_save(void)
{
    if (!g_staged)
    {
        return SIDETNFS_RTCCONFIG_STATUS_NOT_STAGED;
    }

    // Fase 12A: re-validate the staged copy in full before touching
    // configData/flash at all -- defense in depth, since stage() already
    // validated it once, but nothing else in this module can have
    // mutated g_staging in between.
    sidetnfs_rtcconfig_status_t result = sidetnfs_rtcconfig_validate(&g_staging);
    if (result != SIDETNFS_RTCCONFIG_STATUS_OK)
    {
        return result;
    }

    // Clean local copy: guarantees NUL-termination and canonical
    // utc_offset form, independent of what was already true of the
    // (already-validated, already-normalized-by-stage()) staging copy --
    // same "build a clean temp before writing anything real" shape
    // sidetnfs_netconfig_save() uses for its own clean/candidate struct.
    sidetnfs_rtc_config_t clean = g_staging;
    clean.ntp_server[sizeof(clean.ntp_server) - 1] = '\0';
    clean.utc_offset[sizeof(clean.utc_offset) - 1] = '\0';
    normalize_utc_offset(clean.utc_offset, sizeof(clean.utc_offset));

    const char *enabled_str = clean.enabled ? "true" : "false";

    // Fase 12A: put_bool()/put_string() mutate the live in-RAM
    // configData immediately (romemul/config.c's add_entry()) -- the
    // same behavior sidetnfs_netconfig_save() already relies on and
    // ships with. If write_all_entries() or the readback below then
    // fails, configData has already been updated in RAM even though
    // flash was not (successfully) changed; the next SAVE (or reboot,
    // which reloads from flash) is required to reconcile it. This is a
    // pre-existing, accepted characteristic of this save pattern, not
    // something introduced here -- see report.
    //
    // put_bool()/put_string() declare their key parameter as
    // `const char key[MAX_KEY_LENGTH]` (20 bytes). PARAM_GEMDRIVE_RTC
    // ("GEMDRIVE_RTC", 13 bytes incl. NUL) and PARAM_RTC_UTC_OFFSET
    // ("RTC_UTC_OFFSET", 15 bytes incl. NUL) are both shorter string
    // literals than that, which -Wstringop-overread flags as a
    // (harmless, since put_bool()/put_string() never actually read past
    // the NUL) size mismatch. Copied into a correctly-sized local
    // MAX_KEY_LENGTH buffer first -- same fix shape without touching the
    // shared put_bool()/put_string() signature. PARAM_RTC_NTP_SERVER_HOST
    // is exactly 20 bytes incl. NUL, so it never triggered this warning.
    char gemdrive_rtc_key[MAX_KEY_LENGTH];
    memset(gemdrive_rtc_key, 0, sizeof(gemdrive_rtc_key));
    strncpy(gemdrive_rtc_key, PARAM_GEMDRIVE_RTC, sizeof(gemdrive_rtc_key) - 1);

    char rtc_utc_offset_key[MAX_KEY_LENGTH];
    memset(rtc_utc_offset_key, 0, sizeof(rtc_utc_offset_key));
    strncpy(rtc_utc_offset_key, PARAM_RTC_UTC_OFFSET, sizeof(rtc_utc_offset_key) - 1);

    put_bool(gemdrive_rtc_key, clean.enabled != 0);
    put_string(PARAM_RTC_NTP_SERVER_HOST, clean.ntp_server);
    put_string(rtc_utc_offset_key, clean.utc_offset);

    if (write_all_entries() != 0)
    {
        return SIDETNFS_RTCCONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    // Real XIP readback -- re-read the bytes actually committed to flash
    // (not the live in-RAM configData, which trivially already matches
    // what was just written) -- same proof-of-write
    // sidetnfs_netconfig_save() performs. static: sizeof(ConfigData) is
    // far too large for the stack.
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    static ConfigData readback;
    memcpy(&readback, flash_ptr, sizeof(readback));

    bool ok = true;
    ok = ok && readback_field_matches(&readback, PARAM_GEMDRIVE_RTC, enabled_str);
    ok = ok && readback_field_matches(&readback, PARAM_RTC_NTP_SERVER_HOST, clean.ntp_server);
    ok = ok && readback_field_matches(&readback, PARAM_RTC_UTC_OFFSET, clean.utc_offset);

    if (!ok)
    {
        return SIDETNFS_RTCCONFIG_STATUS_FLASH_VERIFY_FAILED;
    }

    // Fase 12A: deliberately does NOT touch WiFi/NTP or reboot -- the new
    // configuration only takes effect on the next normal Pico boot.
    return SIDETNFS_RTCCONFIG_STATUS_OK;
}
