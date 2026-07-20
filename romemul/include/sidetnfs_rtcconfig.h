/**
 * File: sidetnfs_rtcconfig.h
 * Description: Fase 12A -- minimal (KISS) "Set Atari clock using NTP"
 * configuration accessible while GEMDRIVE is running
 * (GEMDRVEMUL_SIDETNFS_GET/SET/SAVE_RTC_CONFIG). Reuses the existing
 * GEMDRIVE_RTC/RTC_NTP_SERVER_HOST/RTC_UTC_OFFSET configData entries and
 * the existing 8KB CONFIG_FLASH sector (romemul/config.c) -- no new
 * PARAM_* entries, MAX_ENTRIES unchanged. RTC_NTP_SERVER_PORT stays an
 * internal-only default (123, config.c); this protocol never reads or
 * writes it. Mirrors the shape (validate -> RAM stage -> flash +
 * readback) already proven by sidetnfs_netconfig.c, against the same
 * configData/CONFIG_FLASH storage, but a completely separate command-ID
 * space (0x16-0x18).
 *
 * Explicitly out of scope for this phase (see report): timezone names,
 * IANA database, automatic DST, POSIX TZ, NTP port configuration, live
 * resync, decoupling network/NTP startup, timeout changes, runtime
 * status beyond the STATUS field below, multiple drives.
 */
#ifndef SIDETNFS_RTCCONFIG_H
#define SIDETNFS_RTCCONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Fase 12A: NTP hostname buffer -- matches PARAM_RTC_NTP_SERVER_HOST's
// existing MAX_STRING_VALUE_LENGTH (64, config.h) exactly, so any value
// already stored in configData round-trips without truncation.
#define SIDETNFS_RTC_NTP_SERVER_LEN 64

// Fase 12A: UTC offset text buffer -- whole hours only, range -12..+14
// (see sidetnfs_rtcconfig_validate()), canonical form "0"/"+N"/"-N".
// 4 bytes is exactly enough for the longest values ("+14"/"-12", 3 chars)
// plus a NUL terminator -- no slack for anything longer.
#define SIDETNFS_RTC_UTC_OFFSET_LEN 4

// Fase 12A: one fixed wire record, every field either uint16_t or a
// char[] of even length -- no field ever needs compiler-inserted
// padding, and this struct is never memcpy'd as a whole onto the ROM3
// shared-memory window (see GEMDRVEMUL_SIDETNFS_RTC_* explicit offsets
// in gemdrvemul.h) -- it is only ever a RAM-side value (GET result, SET
// request, or the staging copy), field-by-field into/out of shared
// memory.
typedef struct
{
    uint16_t enabled;                          // 0 or 1 -- mirrors GEMDRIVE_RTC's true/false
    char ntp_server[SIDETNFS_RTC_NTP_SERVER_LEN];   // 64, required when enabled == 1
    char utc_offset[SIDETNFS_RTC_UTC_OFFSET_LEN];   // 4, whole hours, "-12".."+14"
} sidetnfs_rtc_config_t;

// Fase 12A: exact size guarded at compile time -- 2+64+4 = 70. If this
// ever fails, GEMDRVEMUL_SIDETNFS_RTC_* offsets in gemdrvemul.h (which
// are computed independently, field-length-by-field-length, not from
// sizeof()) and this struct have silently drifted apart.
_Static_assert(sizeof(sidetnfs_rtc_config_t) == 70, "sidetnfs_rtc_config_t must stay exactly 70 bytes");

typedef enum
{
    SIDETNFS_RTCCONFIG_STATUS_OK = 0,
    SIDETNFS_RTCCONFIG_STATUS_INVALID_ENABLED = 1,
    SIDETNFS_RTCCONFIG_STATUS_INVALID_NTP_SERVER = 2,
    SIDETNFS_RTCCONFIG_STATUS_INVALID_UTC_OFFSET = 3,
    SIDETNFS_RTCCONFIG_STATUS_NOT_STAGED = 4,
    SIDETNFS_RTCCONFIG_STATUS_FLASH_WRITE_FAILED = 5,
    SIDETNFS_RTCCONFIG_STATUS_FLASH_VERIFY_FAILED = 6
} sidetnfs_rtcconfig_status_t;

// GET_RTC_CONFIG: fills *out from the three existing GEMDRIVE_RTC/
// RTC_NTP_SERVER_HOST/RTC_UTC_OFFSET config entries. No SD/WiFi/NTP/
// flash I/O -- find_entry() is a pure RAM lookup. Missing entries (never
// expected in practice -- all three are seeded in config.c's
// defaultEntries[] -- but handled defensively) fall back to the same
// defaults config.c itself uses: enabled=1, ntp_server="pool.ntp.org",
// utc_offset="+1".
void sidetnfs_rtcconfig_get(sidetnfs_rtc_config_t *out);

// Pure validation, no side effects whatsoever -- never touches
// configData, flash, or the staging copy, and never normalizes
// utc_offset (see sidetnfs_rtcconfig_stage()/_save() for that). Checked
// in this order: enabled (0 or 1 only), ntp_server (NUL-terminated
// within its buffer, no spaces, required only when enabled == 1 -- when
// enabled == 0 an existing/unchanged server value is accepted as-is, is
// never force-cleared by this protocol), utc_offset (NUL-terminated
// within its buffer, optional leading '+'/'-', 1-2 digits, nothing else,
// parsed value in -12..+14).
sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_validate(const sidetnfs_rtc_config_t *cfg);

// SET_RTC_CONFIG: validates first; only on SIDETNFS_RTCCONFIG_STATUS_OK
// does it overwrite the RAM-only staging copy (utc_offset normalized to
// "0"/"+N"/"-N" in the staging copy at this point). On any validation
// failure, the previous staging copy (if any) is left completely
// untouched. Never touches configData, flash, WiFi, or NTP.
sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_stage(const sidetnfs_rtc_config_t *cfg);

// SAVE_RTC_CONFIG: SIDETNFS_RTCCONFIG_STATUS_NOT_STAGED if
// sidetnfs_rtcconfig_stage() was never called successfully (or only ever
// failed) since boot. Otherwise re-validates the staged copy (defense in
// depth), builds a clean local copy (NUL-termination + utc_offset
// re-normalized), updates the three existing GEMDRIVE_RTC/
// RTC_NTP_SERVER_HOST/RTC_UTC_OFFSET configData entries via put_bool()/
// put_string() (same as sidetnfs_netconfig_save() -- note these mutate
// the live in-RAM configData immediately, before write_all_entries() is
// even attempted; see report for the known implication if the
// subsequent flash write/readback then fails), calls write_all_entries()
// (romemul/config.c), reads the same three entries back via a fresh XIP
// re-read, and only reports SIDETNFS_RTCCONFIG_STATUS_OK if all three
// fields byte-for-byte round-trip. Never touches WiFi/NTP/reboots -- the
// new configuration only takes effect on the next normal Pico boot.
sidetnfs_rtcconfig_status_t sidetnfs_rtcconfig_save(void);

// Test/introspection helper: true once sidetnfs_rtcconfig_stage() has
// succeeded at least once (and stays true across a later failed
// stage/save -- only ever cleared by another successful stage
// overwriting it, never automatically). Mirrors
// sidetnfs_netconfig_is_staged()'s same "never silently reset" contract.
bool sidetnfs_rtcconfig_is_staged(void);

#endif // SIDETNFS_RTCCONFIG_H
