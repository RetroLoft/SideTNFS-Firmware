/**
 * File: sidetnfs_floppy_config.h
 * Description: -- persistent FLOPPY.PRG server-profile flash config.
 *
 * Entirely independent from sidetnfs_config.h's GEMDOS drive-list sector:
 * own flash sector, own magic/version/CRC32, own struct, no drive letters,
 * no GEMDOS drive slot state. FLOPPY.PRG's profiles are TNFS *sources* it
 * browses for floppy images -- they never become GEMDOS drives.
 *
 * Same design philosophy as sidetnfs_config.h/sidetnfs_system_config.h:
 * magic -> version -> CRC32 -> structural validation, wholesale fallback to
 * built-in defaults on any failure (never a partial/salvaged record).
 * SAVE_PROFILES is the only command that ever touches flash -- GET/SET/
 * DELETE_PROFILE and SET_ACTIVE_PROFILE only ever change the RAM copy. See
 * the SideTNFS-Floppy-emulation project's RESEARCH-STEP0.md section 8 for
 * the full design rationale.
 *
 * Deliberately smaller than sidetnfs_config.h: no v(N-1) migration path
 * (this is a new store, starting at version 1), no "pending"/reinit
 * tracking (no active runtime session depends on these profiles the way
 * the GEMDOS drive list feeds the active TNFS connection), no factory-reset
 * command, no UART diagnostic dump. Add these only if a real need appears.
 */
#ifndef SIDETNFS_FLOPPY_CONFIG_H
#define SIDETNFS_FLOPPY_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// Next free 4KB-aligned flash sector after SIDETNFS_SYSTEM_CONFIG_FLASH
// (0x101000, 4KB, see sidetnfs_system_config.h), i.e. immediately after
// that sector ends at 0x102000. Re-proven free the same way that sector's
// own offset was: nothing in this codebase writes flash at or above
// 0x102000 (config.c/sidetnfs_config.c/sidetnfs_system_config.c all target
// strictly lower, or the two sectors immediately below this one).
// Reserved as an actual MEMORY region in memmap_romemul.ld (not just this
// comment) so a future flash-layout change gets a linker-enforced
// collision check, not only a hand-verified one.
#define SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET 0x102000u
#define SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE 4096u

#define SIDETNFS_FLOPPY_MAX_PROFILES 8
#define SIDETNFS_FLOPPY_NICKNAME_LEN 24
#define SIDETNFS_FLOPPY_HOST_LEN 64
#define SIDETNFS_FLOPPY_MOUNTPATH_LEN 32
// Matches the LFN browser's proposed CWD size (RESEARCH-STEP0.md section
// 6) so a future browser can persist "last directory" without a record
// shape change. Not directly editable via the profile editor UI -- see
// sidetnfs_floppy_profile_config_t's own field comment below.
#define SIDETNFS_FLOPPY_LASTDIR_LEN 256

// Mirrors sidetnfs_drive_slot_state_t's three-state convention (EMPTY/
// DISABLED/ENABLED) for the same reason: DISABLED lets a profile be saved
// without being the active one, distinct from never having been configured
// at all.
typedef enum
{
    SIDETNFS_FLOPPY_PROFILE_EMPTY = 0,
    SIDETNFS_FLOPPY_PROFILE_DISABLED = 1,
    SIDETNFS_FLOPPY_PROFILE_ENABLED = 2
} sidetnfs_floppy_profile_state_t;

#define SIDETNFS_FLOPPY_CONFIG_MAGIC 0x464C5053u // "FLPS" -- Floppy Profile Store
#define SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION 1u

// One TNFS floppy-image source. No drive_letter, no type/transport choice
// (TNFS/UDP only, same restriction the GEMDOS drive list currently
// enforces for TNFS drives), no sd_path -- this store only ever describes
// a TNFS host to browse, never a GEMDOS drive.
//
// last_directory is included in the flash layout now so a future browser
// (see RESEARCH-STEP0.md) can persist "last shown directory" without a
// record/flash-version change later, but it is application state, not a
// user-edited field -- the Step 1 profile editor does not expose it.
// Because writing flash on every directory navigation would wear the
// sector for no real benefit, only an explicit SET_PROFILE + SAVE_PROFILES
// (the same two-step RAM-then-flash flow every other field already uses)
// ever persists a changed last_directory -- see sidetnfs_floppy_config.c's
// own file header for the exact policy this phase implements.
typedef struct
{
    uint8_t state; // sidetnfs_floppy_profile_state_t
    uint8_t reserved0[3];
    char nickname[SIDETNFS_FLOPPY_NICKNAME_LEN];
    char host[SIDETNFS_FLOPPY_HOST_LEN];
    uint16_t port;
    uint8_t reserved1[2];
    char mount_path[SIDETNFS_FLOPPY_MOUNTPATH_LEN];
    char last_directory[SIDETNFS_FLOPPY_LASTDIR_LEN];
} sidetnfs_floppy_profile_config_t;

static inline bool sidetnfs_floppy_profile_is_empty(const sidetnfs_floppy_profile_config_t *p)
{
    return p->state == SIDETNFS_FLOPPY_PROFILE_EMPTY;
}

static inline bool sidetnfs_floppy_profile_is_configured(const sidetnfs_floppy_profile_config_t *p)
{
    return p->state == SIDETNFS_FLOPPY_PROFILE_DISABLED || p->state == SIDETNFS_FLOPPY_PROFILE_ENABLED;
}

static inline bool sidetnfs_floppy_profile_is_enabled(const sidetnfs_floppy_profile_config_t *p)
{
    return p->state == SIDETNFS_FLOPPY_PROFILE_ENABLED;
}

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t active_profile_index; // 0..SIDETNFS_FLOPPY_MAX_PROFILES-1; meaningless (0) if no profile is configured
    uint8_t profile_count;        // configured (DISABLED+ENABLED) count, excludes EMPTY
    uint8_t reserved[2];
    sidetnfs_floppy_profile_config_t profiles[SIDETNFS_FLOPPY_MAX_PROFILES];
    uint32_t crc32;
} sidetnfs_floppy_flash_t;

// Compile-time proof of the exact documented sizes -- 384 bytes/record (8+
// 24+64+2+2+32+256), 3088 bytes total (12 + 8*384 + 4), comfortably within
// one 4KB sector.
_Static_assert(sizeof(sidetnfs_floppy_profile_config_t) == 384, "sidetnfs_floppy_profile_config_t size drifted from the documented wire/flash layout");
_Static_assert(sizeof(sidetnfs_floppy_flash_t) == 3088, "sidetnfs_floppy_flash_t size drifted from the documented flash layout");
_Static_assert(sizeof(sidetnfs_floppy_flash_t) <= SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE, "sidetnfs_floppy_flash_t no longer fits in one SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE sector");

typedef enum
{
    SIDETNFS_FLOPPY_STATUS_OK = 0,
    SIDETNFS_FLOPPY_STATUS_INVALID_INDEX = 1,
    SIDETNFS_FLOPPY_STATUS_EMPTY_SLOT = 2,
    SIDETNFS_FLOPPY_STATUS_INVALID_NICKNAME = 3,
    SIDETNFS_FLOPPY_STATUS_INVALID_HOST = 4,
    SIDETNFS_FLOPPY_STATUS_INVALID_PORT = 5,
    SIDETNFS_FLOPPY_STATUS_INVALID_PROFILE_STATE = 6,
    SIDETNFS_FLOPPY_STATUS_FLASH_WRITE_FAILED = 7,
    SIDETNFS_FLOPPY_STATUS_CRC_MISMATCH = 8,
    SIDETNFS_FLOPPY_STATUS_UNSUPPORTED_VERSION = 9
} sidetnfs_floppy_config_status_t;

// Load and validate the flash config block exactly once at boot (magic,
// flash-format version, CRC32 over the whole block, then
// active_profile_index range and every configured record's own fields).
// Falls back to the built-in default (all eight slots EMPTY, active index
// 0) on blank/erased flash or ANY validation failure -- never attempts to
// salvage individual records out of a block that failed any check. Never
// writes to flash. Must be called before GEMDRVEMUL can process any
// GEMDRVEMUL_FLOPPY_* command (see main.c).
void sidetnfs_floppy_config_init(void);

// True iff the currently-active config came from a validated flash block
// this boot -- false means sidetnfs_floppy_config_init() fell back to the
// built-in (all-empty) default.
bool sidetnfs_floppy_config_loaded_from_flash(void);

uint32_t sidetnfs_floppy_config_get_max_profiles(void);

// Number of CONFIGURED slots (DISABLED + ENABLED), excludes EMPTY.
uint8_t sidetnfs_floppy_config_get_profile_count(void);

uint8_t sidetnfs_floppy_config_get_active_profile_index(void);

// GET_PROFILE: fills *out with profiles[index] (zeroed first, so a non-OK
// result always leaves *out fully zeroed). INVALID_INDEX if index >=
// SIDETNFS_FLOPPY_MAX_PROFILES; otherwise always OK, for EMPTY as much as
// for DISABLED/ENABLED. All strings are guaranteed NUL-terminated.
sidetnfs_floppy_config_status_t sidetnfs_floppy_config_get_profile(uint8_t index, sidetnfs_floppy_profile_config_t *out);

// SET_PROFILE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_FLOPPY_MAX_PROFILES. Dispatches on in->state:
//   EMPTY:              the slot is fully cleared (equivalent to
//                        sidetnfs_floppy_config_delete_profile()) -- no
//                        field validation applies.
//   DISABLED / ENABLED:  nickname/host/port are validated (mount_path and
//                        last_directory may be empty -- "server root" /
//                        "not yet browsed") before being written.
//   anything else:       SIDETNFS_FLOPPY_STATUS_INVALID_PROFILE_STATE,
//                        nothing is written.
// profile_count is recomputed after every call.
sidetnfs_floppy_config_status_t sidetnfs_floppy_config_set_profile(uint8_t index, const sidetnfs_floppy_profile_config_t *in);

// DELETE_PROFILE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_FLOPPY_MAX_PROFILES, EMPTY_SLOT if already EMPTY. Fully zeroes
// the record and recomputes profile_count. Clears active_profile_index to
// 0 if it pointed at the deleted slot.
sidetnfs_floppy_config_status_t sidetnfs_floppy_config_delete_profile(uint8_t index);

// SET_ACTIVE_PROFILE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_FLOPPY_MAX_PROFILES. EMPTY_SLOT if that slot is not configured
// -- an EMPTY slot can never become the active profile.
sidetnfs_floppy_config_status_t sidetnfs_floppy_config_set_active_profile(uint8_t index);

// SAVE_PROFILES: validates the full RAM config, builds a clean flash image
// (reserved bytes zeroed, EMPTY records fully zeroed, profile_count
// recomputed), erases exactly one SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE
// sector, programs only the page-aligned bytes actually needed (interrupts
// disabled only around the erase+program pair), then reads back via XIP
// and re-validates magic/version/CRC before reporting success. Returns
// FLASH_WRITE_FAILED if the readback magic/version don't match,
// CRC_MISMATCH if the CRC doesn't match. Never retries and never reboots.
// This is the ONLY function in this module that ever touches flash --
// selecting a different active profile or editing a profile in
// FLOPPY.PRG's UI does not, by itself, write flash; the user must reach an
// explicit "Save" action (mirrors SAVE_CONFIG's contract exactly, see
// sidetnfs_config.h).
sidetnfs_floppy_config_status_t sidetnfs_floppy_config_save(void);

#endif // SIDETNFS_FLOPPY_CONFIG_H
