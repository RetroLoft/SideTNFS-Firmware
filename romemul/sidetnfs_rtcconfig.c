/**
 * File: sidetnfs_rtcconfig.c
 * Description: -- see sidetnfs_rtcconfig.h. storage
 * backend switched from the old ConfigEntry store (romemul/config.c) to
 * the independent sidetnfs_system_config module -- this file no longer
 * includes config.h or calls find_entry()/put_bool()/put_string()/
 * write_all_entries() at all. Wire-protocol struct, validation rules,
 * and RAM staging are all unchanged from before.
 */
#include "include/sidetnfs_rtcconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/sidetnfs_system_config.h"

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

    sidetnfs_system_settings_t sys;
    sidetnfs_system_config_get(&sys);

    out->enabled = sys.rtc_enabled;
    strncpy(out->ntp_server, sys.ntp_server, sizeof(out->ntp_server) - 1);
    out->ntp_server[sizeof(out->ntp_server) - 1] = '\0';
    strncpy(out->utc_offset, sys.utc_offset, sizeof(out->utc_offset) - 1);
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
        // G_staging/g_staged are untouched on any failure --
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

sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_save(void)
{
    if (!g_staged)
    {
        return SIDETNFS_RTCCONFIG_STATUS_NOT_STAGED;
    }

    // Re-validate the staged copy in full before touching
    // sidetnfs_system_config at all -- defense in depth, since stage()
    // already validated it once, but nothing else in this module can
    // have mutated g_staging in between.
    sidetnfs_rtcconfig_status_t result = sidetnfs_rtcconfig_validate(&g_staging);
    if (result != SIDETNFS_RTCCONFIG_STATUS_OK)
    {
        return result;
    }

    // Clean local copy: guarantees NUL-termination and canonical
    // utc_offset form, independent of what was already true of the
    // (already-validated, already-normalized-by-stage()) staging copy.
    sidetnfs_rtc_config_t clean = g_staging;
    clean.ntp_server[sizeof(clean.ntp_server) - 1] = '\0';
    clean.utc_offset[sizeof(clean.utc_offset) - 1] = '\0';
    normalize_utc_offset(clean.utc_offset, sizeof(clean.utc_offset));

    // Read the current full system settings first, so the
    // WiFi/Network fields (owned by sidetnfs_netconfig.c, sharing the
    // same underlying sidetnfs_system_flash_t) are preserved exactly as
    // they were -- this SAVE only ever touches the RTC fields.
    sidetnfs_system_settings_t sys;
    sidetnfs_system_config_get(&sys);

    sys.rtc_enabled = clean.enabled;
    strncpy(sys.ntp_server, clean.ntp_server, sizeof(sys.ntp_server) - 1);
    sys.ntp_server[sizeof(sys.ntp_server) - 1] = '\0';
    strncpy(sys.utc_offset, clean.utc_offset, sizeof(sys.utc_offset) - 1);
    sys.utc_offset[sizeof(sys.utc_offset) - 1] = '\0';

    sidetnfs_system_config_status_t set_result = sidetnfs_system_config_set(&sys);
    if (set_result != SIDETNFS_SYSCONFIG_STATUS_OK)
    {
        // See sidetnfs_netconfig_save()'s identical comment -- this
        // module already validated everything semantically above.
        return SIDETNFS_RTCCONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    sidetnfs_system_config_status_t save_result = sidetnfs_system_config_save();
    if (save_result == SIDETNFS_SYSCONFIG_STATUS_CRC_MISMATCH)
    {
        return SIDETNFS_RTCCONFIG_STATUS_FLASH_VERIFY_FAILED;
    }
    if (save_result != SIDETNFS_SYSCONFIG_STATUS_OK)
    {
        return SIDETNFS_RTCCONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    // Deliberately does NOT touch WiFi/NTP or reboot -- the new
    // configuration only takes effect on the next normal Pico boot.
    return SIDETNFS_RTCCONFIG_STATUS_OK;
}
