/**
 * File: sidetnfs_system_config.h
 * Description: Fase 2 -- independent, persistent SideTNFS system
 * configuration (WiFi + Network + RTC/NTP + reserved general settings),
 * in its own flash sector (SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET), with its
 * own magic/format-version/CRC32/structure validation -- same design
 * philosophy as sidetnfs_config.h's drive-list block, entirely
 * independent from it (this is not the drive config, and not the old
 * Sidecart ConfigEntry store in romemul/config.c).
 *
 * sidetnfs_netconfig.c and sidetnfs_rtcconfig.c are the only intended
 * callers of the _get/_set/_save functions below -- they keep their own
 * existing wire-protocol structs/validation/staging exactly as before,
 * only swapping their storage backend from ConfigEntry/config.c to this
 * module. Nothing else should call sidetnfs_system_config_set()/_save()
 * directly.
 */
#ifndef SIDETNFS_SYSTEM_CONFIG_H
#define SIDETNFS_SYSTEM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// Fase 2: placed in the next free 4KB-aligned sector right after the
// existing SIDETNFS drive-config sector (SIDETNFS_CONFIG_FLASH_OFFSET =
// 0x100000, size 4096, see sidetnfs_config.h) -- confirmed free against
// the linker script (romemul/memmap_romemul.ld): FLASH spans
// 0x10000000..0x101DE000 (1912K), ROM_FLASH is a separate reservation at
// 0x100E0000 (128K), and the old ConfigEntry store's own CONFIG_FLASH_OFFSET
// (romemul/constants.c, = FLASH_ROM_LOAD_OFFSET - 8192) is a different,
// much lower region entirely. 0x101000 is between the drive-config
// sector and ROM_FLASH, touching neither.
#define SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET 0x101000u
#define SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE 4096u

#define SIDETNFS_SYSTEM_CONFIG_MAGIC 0x53595343u // "SYSC"
#define SIDETNFS_SYSTEM_CONFIG_FLASH_VERSION 1u

// Field sizes match the existing wire-protocol structs exactly
// (sidetnfs_netconfig.h/sidetnfs_rtcconfig.h) so no value ever needs
// truncation moving between the two.
#define SIDETNFS_SYS_SSID_LEN        36
#define SIDETNFS_SYS_PASSWORD_LEN    68
#define SIDETNFS_SYS_COUNTRY_LEN     4
#define SIDETNFS_SYS_IPV4_LEN        16
#define SIDETNFS_SYS_NTP_SERVER_LEN  64
#define SIDETNFS_SYS_UTC_OFFSET_LEN  4

// One persistent record for everything sidetnfs_netconfig.c and
// sidetnfs_rtcconfig.c together used to keep in ConfigEntry. Fase 2
// deliberately does NOT split this into three separate flash sectors --
// one sector, one magic/version/CRC, matching "eventuele toekomstige
// algemene instellingen" (the trailing reserved bytes) living alongside
// WiFi/Network/RTC in a single system-config concept.
typedef struct
{
    // --- WiFi ---
    uint16_t auth_mode;                         // 0-8
    uint16_t use_dhcp;                           // 0 or 1
    char ssid[SIDETNFS_SYS_SSID_LEN];
    char password[SIDETNFS_SYS_PASSWORD_LEN];
    char country[SIDETNFS_SYS_COUNTRY_LEN];      // 2 letters + NUL + pad, "XX" = unset

    // --- Network (static IP, meaningful only when use_dhcp == 0) ---
    char ip_address[SIDETNFS_SYS_IPV4_LEN];
    char netmask[SIDETNFS_SYS_IPV4_LEN];
    char gateway[SIDETNFS_SYS_IPV4_LEN];
    char primary_dns[SIDETNFS_SYS_IPV4_LEN];

    // --- RTC / NTP ---
    uint16_t rtc_enabled;                        // 0 or 1
    char ntp_server[SIDETNFS_SYS_NTP_SERVER_LEN];
    char utc_offset[SIDETNFS_SYS_UTC_OFFSET_LEN]; // canonical "0"/"+N"/"-N"

    // Reserved for future general settings -- always zeroed for now,
    // covered by structure validation/CRC like every other byte.
    uint8_t reserved[16];
} sidetnfs_system_settings_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    sidetnfs_system_settings_t settings;
    uint32_t crc32;
} sidetnfs_system_flash_t;

_Static_assert(sizeof(sidetnfs_system_flash_t) <= SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE,
               "sidetnfs_system_flash_t no longer fits in one SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE sector");

typedef enum
{
    SIDETNFS_SYSCONFIG_STATUS_OK = 0,
    SIDETNFS_SYSCONFIG_STATUS_INVALID_STRUCTURE = 1,
    SIDETNFS_SYSCONFIG_STATUS_FLASH_WRITE_FAILED = 2,
    SIDETNFS_SYSCONFIG_STATUS_CRC_MISMATCH = 3,
    SIDETNFS_SYSCONFIG_STATUS_UNSUPPORTED_VERSION = 4
} sidetnfs_system_config_status_t;

// Fase 2: same fallback design as sidetnfs_config_init() -- checks magic,
// then format version, then CRC32, then structure (NUL-termination/basic
// range sanity only; semantic validation, e.g. country-code allow-lists
// or IPv4 parsing, stays the responsibility of sidetnfs_netconfig.c/
// sidetnfs_rtcconfig.c's own existing validate() functions, which run
// before any value ever reaches sidetnfs_system_config_set()). On any
// failure: if the OLD ConfigEntry store (romemul/config.c) already holds
// a loaded, in-RAM configuration, migrate its PARAM_WIFI_*/
// PARAM_GEMDRIVE_RTC/PARAM_RTC_* values into this module's RAM copy
// (never written to flash by this function -- only an explicit SAVE from
// SIDETNFS.PRG persists it). If the old store isn't usable either, falls
// back to the same built-in factory defaults romemul/config.c's own
// defaultEntries[] table already used. No firmware/UF2 build hash is
// ever checked, same policy as sidetnfs_config_init().
void sidetnfs_system_config_init(void);

// True iff the currently-active settings came from a validated flash
// block this boot (false covers both "migrated from legacy ConfigEntry"
// and "factory default" -- see sidetnfs_system_config_migrated_from_legacy()
// to tell those two apart).
bool sidetnfs_system_config_loaded_from_flash(void);

// Fase 2B: true iff this boot's active settings were migrated (RAM only)
// from GENUINE, real legacy ConfigEntry flash data (config.c's own
// config_loaded_from_real_flash() must also be true) because no valid
// sidetnfs_system_config flash block existed yet. False in every other
// case -- including when the legacy store itself was never configured
// and only held its own built-in defaults, which is reported as a plain
// factory-default fallback instead (see
// sidetnfs_system_config_loaded_from_flash() being false with this also
// false). Always false once a SAVE has actually persisted a real flash
// block on some earlier boot.
bool sidetnfs_system_config_migrated_from_legacy(void);

// Fills *out with the currently active settings (flash-loaded, migrated,
// or factory-default -- whichever this boot actually has). Read-only, no
// flash access.
void sidetnfs_system_config_get(sidetnfs_system_settings_t *out);

// Overwrites the active RAM copy with *in -- structure-validates first
// (NUL-termination/basic range sanity, see sidetnfs_system_config_init()'s
// own doc comment on the validation split); on failure the previous RAM
// copy is left completely untouched. RAM only, never touches flash.
sidetnfs_system_config_status_t sidetnfs_system_config_set(const sidetnfs_system_settings_t *in);

// Persists the current active RAM copy to flash: validates, builds a
// clean image (reserved bytes zeroed, strings NUL-terminated), erases
// exactly one SIDETNFS_SYSTEM_CONFIG_FLASH_SIZE sector, programs it,
// reads it back via XIP, and re-validates magic/version/CRC before
// reporting success -- same erase/program/readback contract
// sidetnfs_config_save() already uses for its own, separate sector.
sidetnfs_system_config_status_t sidetnfs_system_config_save(void);

#endif // SIDETNFS_SYSTEM_CONFIG_H
