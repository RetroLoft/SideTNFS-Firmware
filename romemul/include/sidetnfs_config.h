/**
 * File: sidetnfs_config.h
 * Description: -- persistent SideTNFS drive-list flash config.
 * Replaces the max-8-servers model (never committed) with one
 * mandatory config drive (letter only, kept outside this array) plus up to
 * SIDETNFS_MAX_DRIVES ordinary SD/TNFS drives. Read/write from the Atari
 * side via GET_CONFIG_INFO/GET_DRIVE/SET_DRIVE/DELETE_DRIVE/
 * SET_CONFIG_DRIVE/SAVE_CONFIG (gemdrvemul.c). SAVE_CONFIG is the only
 * command in this phase that ever touches flash -- GET/SET/DELETE/
 * SET_CONFIG_DRIVE only ever change the RAM copy. The active GEMDRIVE
 * runtime (sidetnfs_probe.c) is NOT changed by this module -- a reboot is
 * required before any future code can act on a saved list. See
 * docs/sidetnfs-config-protocol.md for the full contract.
 */
#ifndef SIDETNFS_CONFIG_H
#define SIDETNFS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// Dedicated 4KB flash sector for the SideTNFS config, re-proven
// free against the current linker script (romemul/memmap_romemul.ld) --
// ROM_FLASH ends at exactly XIP_BASE + 0x100000 (0x100E0000, length 128k),
// and CONFIG_FLASH/ROM_FLASH are the only other reserved flash regions,
// both < 0x100000. See docs/sidetnfs-config-protocol.md for the full
// evidence trail (unchanged.
#define SIDETNFS_CONFIG_FLASH_OFFSET 0x100000u
#define SIDETNFS_CONFIG_FLASH_SIZE 4096u

// Ordinary (non-config) drives only -- the mandatory config drive is a
// separate field in sidetnfs_drive_flash_t and never counts against this
// limit or against drive_count.
#define SIDETNFS_MAX_DRIVES 8
#define SIDETNFS_NICKNAME_LEN 24
#define SIDETNFS_HOST_LEN 64
#define SIDETNFS_MOUNTPATH_LEN 32
#define SIDETNFS_SDPATH_LEN 64

typedef enum
{
    SIDETNFS_DRIVE_SD = 1,
    SIDETNFS_DRIVE_TNFS = 2
} sidetnfs_drive_type_t;

// The three-state model for each of the SIDETNFS_MAX_DRIVES
// ordinary slots -- replaces the ..12A binary `used` flag. EMPTY
// means the slot holds no stored configuration at all (all other fields
// are meaningless/zeroed); DISABLED means a fully valid, stored
// configuration that is deliberately not published as a GEMDOS drive;
// ENABLED means a fully valid, stored configuration that is (eventually)
// published. DISABLED and ENABLED are both "configured" -- see
// sidetnfs_drive_slot_is_configured() below -- and both count for
// duplicate-drive-letter validation and drive_count (see
// sidetnfs_config_validate_structure()'s own comment). The SETTINGS disk
// is not part of this model at all -- it has no index in drives[] and
// therefore no EMPTY/DISABLED state, only its separate
// config_drive_letter field (see sidetnfs_drive_flash_t below).
typedef enum
{
    SIDETNFS_DRIVE_SLOT_EMPTY = 0,
    SIDETNFS_DRIVE_SLOT_DISABLED = 1,
    SIDETNFS_DRIVE_SLOT_ENABLED = 2
} sidetnfs_drive_slot_state_t;

typedef enum
{
    SIDETNFS_TRANSPORT_UDP = 0,
    SIDETNFS_TRANSPORT_TCP = 1 // stored for future use; the UI/validation
                               // this phase only ever accepts UDP for a new
                               // TNFS drive, and no TCP socket code exists
                               // anywhere in this module or its callers yet.
} sidetnfs_transport_t;

#define SIDETNFS_CONFIG_MAGIC 0x53544446u // "STDF"
#define SIDETNFS_CONFIG_FLASH_VERSION 3u

// The record layout itself did NOT change size or field offsets
// from flash format version 2 -- only the first field's name and value
// range changed (used: uint8_t, 0/1 -> state: uint8_t, 0/1/2, see
// sidetnfs_drive_slot_state_t above). sidetnfs_config.c's migration path
// (sidetnfs_config_migrate_v2_to_v3()) relies on this byte-for-byte
// compatibility -- see its own comment and the
// sidetnfs_drive_config_v2_t/sidetnfs_drive_flash_v2_t types local to
// that file.
//
// Pointer-free, fixed-layout record -- no union: SD drives use
// nickname/drive_letter/sd_path, TNFS drives use
// nickname/drive_letter/transport/host/port/mount_path; the fields that
// don't apply to a given type are always zeroed (enforced by
// sidetnfs_config_set_drive() and sidetnfs_config_save()).
typedef struct
{
    uint8_t state;         // sidetnfs_drive_slot_state_t -- EMPTY/DISABLED/ENABLED
    uint8_t drive_letter;  // uppercase ASCII, e.g. 'N' -- meaningless when state == EMPTY
    uint8_t type;          // sidetnfs_drive_type_t
    uint8_t transport;     // sidetnfs_transport_t (TNFS only)
    uint16_t port;         // TNFS only
    uint8_t reserved0[2];

    char nickname[SIDETNFS_NICKNAME_LEN];
    char host[SIDETNFS_HOST_LEN];             // TNFS only
    char mount_path[SIDETNFS_MOUNTPATH_LEN];  // TNFS only
    char sd_path[SIDETNFS_SDPATH_LEN];        // SD only
} sidetnfs_drive_config_t;

// Single source of truth for "does this slot hold a stored
// configuration" / "will this slot be published as a GEMDOS drive" --
// never check `state != SIDETNFS_DRIVE_SLOT_EMPTY` or similar ad hoc
// comparisons elsewhere; use these so the three-state semantics stay
// consistent everywhere they're read.
static inline bool sidetnfs_drive_slot_is_empty(const sidetnfs_drive_config_t *drive)
{
    return drive->state == SIDETNFS_DRIVE_SLOT_EMPTY;
}

static inline bool sidetnfs_drive_slot_is_configured(const sidetnfs_drive_config_t *drive)
{
    return drive->state == SIDETNFS_DRIVE_SLOT_DISABLED || drive->state == SIDETNFS_DRIVE_SLOT_ENABLED;
}

static inline bool sidetnfs_drive_slot_is_enabled(const sidetnfs_drive_config_t *drive)
{
    return drive->state == SIDETNFS_DRIVE_SLOT_ENABLED;
}

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t config_drive_letter; // e.g. 'S'; never A/B, never equal to any DISABLED/ENABLED drives[] letter
    uint8_t drive_count;         // number of CONFIGURED records in drives[] -- DISABLED + ENABLED, excludes EMPTY and excludes the config drive. See sidetnfs_config_get_enabled_drive_count() for ENABLED-only.
    uint8_t reserved[2];
    sidetnfs_drive_config_t drives[SIDETNFS_MAX_DRIVES];
    uint32_t crc32;
} sidetnfs_drive_flash_t;

// Compile-time proof of the exact documented sizes -- 192 bytes/record,
// 1552 bytes total (12 + 8*192 + 4), within one 4KB sector.
_Static_assert(sizeof(sidetnfs_drive_config_t) == 192, "sidetnfs_drive_config_t size drifted from the documented wire/flash layout");
_Static_assert(sizeof(sidetnfs_drive_flash_t) == 1552, "sidetnfs_drive_flash_t size drifted from the documented flash layout");
_Static_assert(sizeof(sidetnfs_drive_flash_t) <= SIDETNFS_CONFIG_FLASH_SIZE, "sidetnfs_drive_flash_t no longer fits in one SIDETNFS_CONFIG_FLASH_SIZE sector");

typedef enum
{
    SIDETNFS_CONFIG_STATUS_OK = 0,
    SIDETNFS_CONFIG_STATUS_INVALID_INDEX = 1,
    SIDETNFS_CONFIG_STATUS_EMPTY_SLOT = 2,
    SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_LETTER = 3,
    SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER = 4,
    SIDETNFS_CONFIG_STATUS_INVALID_TYPE = 5,
    SIDETNFS_CONFIG_STATUS_INVALID_TRANSPORT = 6,
    SIDETNFS_CONFIG_STATUS_INVALID_PORT = 7,
    SIDETNFS_CONFIG_STATUS_INVALID_HOST = 8,
    SIDETNFS_CONFIG_STATUS_INVALID_MOUNT_PATH = 9,
    SIDETNFS_CONFIG_STATUS_INVALID_SD_PATH = 10,
    SIDETNFS_CONFIG_STATUS_TOO_MANY_DRIVES = 11, // structurally unreachable via SET_DRIVE today (index is bounded 0..SIDETNFS_MAX_DRIVES-1 by the fixed array), kept for the documented minimum status set and any future bulk-set path
    SIDETNFS_CONFIG_STATUS_FLASH_WRITE_FAILED = 12,
    SIDETNFS_CONFIG_STATUS_CRC_MISMATCH = 13,
    SIDETNFS_CONFIG_STATUS_UNSUPPORTED_VERSION = 14,
    // Appended (never renumbered an existing value -- the
    // protocol layer mirrors these numerically on the Atari side too).
    // A drives[] record's `state` byte was outside
    // sidetnfs_drive_slot_state_t's 0/1/2 range.
    SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE = 15
} sidetnfs_config_status_t;

// Which check (if any) made sidetnfs_config_init fall back to
// the built-in factory default instead of trusting the flash block.
// Deliberately coarse-grained at the top level (magic/version/CRC/
// structure) -- when the reason is STRUCTURE_INVALID, the specific
// validation failure is the sidetnfs_config_status_t from
// sidetnfs_config_get_fallback_structure_status() below (e.g.
// INVALID_DRIVE_LETTER, DUPLICATE_DRIVE_LETTER, an individual drive
// record's own INVALID_* code). No firmware/UF2 build hash is ever
// checked here or anywhere else in this module -- see report ("why no
// firmware hash").
//
// UNSUPPORTED_VERSION is now only reached for a version this
// firmware cannot even migrate (i.e. neither the current
// SIDETNFS_CONFIG_FLASH_VERSION nor the one older version this firmware
// knows how to migrate from -- see sidetnfs_config_migrated_from_version()
// below). A recognized older version that migrates successfully is NOT a
// fallback at all (sidetnfs_config_loaded_from_flash() is still true).
typedef enum
{
    SIDETNFS_CONFIG_FALLBACK_NONE = 0,       // flash config loaded and trusted -- no fallback happened
    SIDETNFS_CONFIG_FALLBACK_BAD_MAGIC = 1,  // blank/erased flash, or a foreign or legacy block
    SIDETNFS_CONFIG_FALLBACK_UNSUPPORTED_VERSION = 2,
    SIDETNFS_CONFIG_FALLBACK_CRC_MISMATCH = 3,
    SIDETNFS_CONFIG_FALLBACK_STRUCTURE_INVALID = 4
} sidetnfs_config_fallback_reason_t;

// Load and validate the flash config block exactly once at boot
// (magic, flash-format version, CRC32 over the whole block, then
// config_drive_letter/drive_count range checks, then every configured
// record's own fields and letter-uniqueness against every other
// DISABLED/ENABLED record AND against config_drive_letter). Falls back to
// the built-in default (config drive 'S', one TNFS drive
// 'N'/RetroLoft/192.168.178.10:16384/Atari.ST, ENABLED -- see
// sidetnfs_config.c) on blank/erased flash or ANY validation failure;
// never attempts to salvage individual records out of a block that failed
// any check. Never writes to flash. Must be called before GEMDRVEMUL can
// process any of the SIDETNFS config commands (see main.c).
//
// A flash block whose version is the previous flash format
// (see sidetnfs_config_migrated_from_version() below) is migrated to the
// current layout in RAM first (used=0/1 -> state EMPTY/ENABLED), then
// validated exactly like a current-version block -- a migrated block that
// fails validation still falls back to defaults, same as any other
// invalid block. A successful migration is never written back to flash by
// this function; only an explicit SAVE_CONFIG persists it.
void sidetnfs_config_init(void);

// The flash format version sidetnfs_config_init actually
// migrated FROM this boot, or 0 if the active config either came straight
// from a current-version flash block or is the built-in factory default
// (no migration happened either way). Only meaningful together with
// sidetnfs_config_loaded_from_flash() == true.
uint32_t sidetnfs_config_migrated_from_version(void);

// True iff the currently-active config (see the getters above)
// came from a validated flash block -- false means sidetnfs_config_init()
// fell back to the built-in factory default (see
// sidetnfs_config_get_fallback_reason() for why). Always call this (or
// check the reason below) before assuming "SAVE_CONFIG will overwrite a
// real prior configuration" in any future diagnostic/UI code -- a
// fallback config was never written to flash, so a save from here is the
// user's first real configuration, not an overwrite.
bool sidetnfs_config_loaded_from_flash(void);

// See sidetnfs_config_fallback_reason_t above. Always
// SIDETNFS_CONFIG_FALLBACK_NONE when sidetnfs_config_loaded_from_flash()
// is true. Persists only for this boot -- sidetnfs_config_init() is the
// only writer, and it runs exactly once (see its own doc comment).
sidetnfs_config_fallback_reason_t sidetnfs_config_get_fallback_reason(void);

// Meaningful only when sidetnfs_config_get_fallback_reason ==
// SIDETNFS_CONFIG_FALLBACK_STRUCTURE_INVALID -- the exact
// sidetnfs_config_validate_structure() failure code (e.g.
// SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_LETTER,
// SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER, or one of the
// individual per-record INVALID_* codes) that caused the fallback.
// SIDETNFS_CONFIG_STATUS_OK otherwise.
sidetnfs_config_status_t sidetnfs_config_get_fallback_structure_status(void);

uint32_t sidetnfs_config_get_max_drives(void);

// Number of CONFIGURED ordinary slots -- DISABLED + ENABLED,
// excludes EMPTY. This is the same value persisted as
// sidetnfs_drive_flash_t.drive_count and transported as
// GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT. For ENABLED-only, see
// sidetnfs_config_get_enabled_drive_count() below.
uint8_t sidetnfs_config_get_drive_count(void);

// Number of ENABLED ordinary slots only (a subset of
// sidetnfs_config_get_drive_count() above). Not persisted -- always
// recomputed from the live drives[] array; add a stored field only if a
// future caller needs this without an O(SIDETNFS_MAX_DRIVES) scan.
uint8_t sidetnfs_config_get_enabled_drive_count(void);

uint8_t sidetnfs_config_get_config_drive_letter(void);

// GET_DRIVE: fills *out with drives[index] (zeroed first, so a non-OK
// result always leaves *out fully zeroed). INVALID_INDEX if index >=
// SIDETNFS_MAX_DRIVES; otherwise always OK, for EMPTY as much as for
// DISABLED/ENABLED (EMPTY is a normal, valid record state, not
// an error -- *out->state tells the caller which of the three it is).
// All strings are guaranteed NUL-terminated.
sidetnfs_config_status_t sidetnfs_config_get_drive(uint8_t index, sidetnfs_drive_config_t *out);

// SET_DRIVE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_MAX_DRIVES. , dispatches on in->state:
//   EMPTY:              the slot is fully cleared (equivalent to
//                        sidetnfs_config_delete_drive()) -- no
//                        type-specific field validation applies.
//   DISABLED / ENABLED:  the record is fully validated (drive letter,
//                        type, and type-specific fields) and checked for
//                        letter uniqueness against every other
//                        DISABLED/ENABLED drive and against the config
//                        drive letter before being written; irrelevant
//                        fields for the record's type are zeroed before
//                        storing (SD zeroes transport/host/mount_path,
//                        TNFS zeroes sd_path). A DISABLED<->ENABLED
//                        transition never drops any other field.
//   anything else:      SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE,
//                        nothing is written.
// drive_count (DISABLED + ENABLED) is recomputed after every call.
sidetnfs_config_status_t sidetnfs_config_set_drive(uint8_t index, const sidetnfs_drive_config_t *in);

// Change only a slot's state, preserving every other stored
// field for a DISABLED<->ENABLED transition. Internally delegates to
// sidetnfs_config_set_drive() -- never duplicates its validation.
//   -> EMPTY:              clears the slot completely (same as SET_DRIVE
//                          with state == EMPTY).
//   -> DISABLED/ENABLED:   requires the slot to already be configured
//                          (DISABLED or ENABLED) -- EMPTY_SLOT if it is
//                          currently EMPTY, since there is no stored
//                          record to preserve; use sidetnfs_config_set_drive()
//                          with a full record to configure an EMPTY slot.
//   anything else:         SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE.
sidetnfs_config_status_t sidetnfs_config_set_drive_state(uint8_t index, sidetnfs_drive_slot_state_t new_state);

// DELETE_DRIVE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_MAX_DRIVES, EMPTY_SLOT if already EMPTY (works on a DISABLED
// slot too -- deletion doesn't care whether it was enabled). Fully zeroes
// the record (state becomes EMPTY) and recomputes drive_count. Can never
// touch the config drive -- there is no config-drive index in this array.
sidetnfs_config_status_t sidetnfs_config_delete_drive(uint8_t index);

// SET_CONFIG_DRIVE: RAM only, no flash write. Rejects A/B and any letter
// already used by a DISABLED or ENABLED ordinary drive (a
// disabled drive still reserves its letter.
sidetnfs_config_status_t sidetnfs_config_set_config_drive_letter(uint8_t new_letter);

// SAVE_CONFIG: validates the full RAM config (config drive letter, every
// DISABLED/ENABLED record, letter uniqueness), builds a clean flash image
// (reserved bytes zeroed, EMPTY records fully zeroed, DISABLED/ENABLED
// records preserved with their actual state, drive_count recomputed),
// erases exactly one SIDETNFS_CONFIG_FLASH_SIZE sector,
// programs only the page-aligned bytes actually needed (interrupts
// disabled only around the erase+program pair, per the same pattern
// romemul/config.c already uses for its own flash config), then reads
// back via XIP and re-validates magic/version/CRC before reporting
// success. Returns FLASH_WRITE_FAILED if the readback magic/version don't
// match, CRC_MISMATCH if the CRC doesn't match, or the validation failure
// code if validation itself failed (nothing is erased/programmed in that
// case). Never retries and never reboots. On success, also marks the
// config "pending" (see sidetnfs_config_is_pending() below) -- writing
// flash alone never changes the currently active TNFS session/drive
// letter/open handles/DTAs; a reboot is required for that, and an Atari
// RESET does not reboot the Pico.
sidetnfs_config_status_t sidetnfs_config_save(void);

// True from the moment a SAVE_CONFIG succeeds until the newly
// saved configuration has actually been adopted by the running GEMDRIVE
// session (see sidetnfs_probe_reinit_active_server() in sidetnfs_probe.c,
// invoked from the proven Atari-reset boundary in gemdrvemul.c --
// GEMDRVEMUL_PING, but only the first one after the very first PING this
// Pico boot). Always false before the first SAVE_CONFIG this boot.
bool sidetnfs_config_is_pending(void);

// Clear the pending flag. Must only be called once the new
// configuration has been safely read back into the active server/drive
// state -- never on a failed or skipped reinit attempt, so a later Atari
// reset still retries.
void sidetnfs_config_clear_pending(void);

// Loads the built-in
// factory defaults (same content sidetnfs_config_init() falls back to on
// a bad/corrupt flash block -- config drive 'S', one TNFS drive
// 'N'/RetroLoft/.../ENABLED, all other ordinary slots EMPTY) into the RAM
// config and persists them via the existing, already-safe
// sidetnfs_config_save() path (validate -> build clean image -> erase
// exactly one sector -> program -> XIP readback -> magic/version/CRC
// re-check). Returns whatever sidetnfs_config_save() returns; on anything
// other than SIDETNFS_CONFIG_STATUS_OK, flash was NOT successfully
// rewritten (sidetnfs_config_save() never partially erases/programs) and
// the caller must not treat the reset as having taken effect. Can be
// called at any point after sidetnfs_config_init() would normally run --
// does not itself require sidetnfs_config_init() to have run first, and
// does not depend on WiFi/TNFS/GEMDOS in any way.
sidetnfs_config_status_t sidetnfs_config_factory_reset(void);

//
// overwrites the RAM config with the built-in factory defaults, exactly
// like sidetnfs_config_factory_reset() above, but NEVER touches flash --
// whatever is actually stored on flash is left completely untouched, so
// the board can boot safely without destroying evidence needed to
// diagnose it. Call after sidetnfs_config_init() (and after
// sidetnfs_config_dump_uart(), if used, so the dump still reflects the
// real flash content) and before sidetnfs_probe_load_active_server().
void sidetnfs_config_force_factory_ram(void);

// True iff the raw flash
// block, read directly via XIP (never via sidetnfs_config_init()'s
// migration/fallback path -- this check never invokes migration at
// all), is magic/version/CRC-valid AND byte-for-byte matches exactly
// what sidetnfs_config_load_defaults() would produce (settings 'S',
// slot 0 the factory N:/RetroLoft/.../ENABLED drive, slots 1-7 fully
// zeroed/EMPTY, drive_count 1). Read-only -- never mutates flash or
// g_config. Used both to decide whether a recovery write is needed at
// all (skip it if this is already true, so a repeated boot of a
// recovery build never rewrites/resets in a loop) and, after a write,
// to verify it actually took effect.
bool sidetnfs_config_flash_matches_factory_defaults(void);

#if SIDETNFS_ENABLE_DIAG_UART
// Read-only boot-time diagnostic dump over UART (printf) of
// the raw flash header (magic/version/stored+recomputed CRC/drive_count/
// settings letter), the load result (native v3 / migrated from v2 /
// factory fallback + reason), and all eight raw + interpreted drive
// records. Never mutates g_config or flash. Call once, synchronously,
// after sidetnfs_config_init() and before any TNFS/runtime-drive code
// runs (see main.c). Compiled out entirely (zero cost) unless
// SIDETNFS_ENABLE_DIAG_UART is set -- see CMakeLists.txt.
void sidetnfs_config_dump_uart(void);
#endif

#endif // SIDETNFS_CONFIG_H
