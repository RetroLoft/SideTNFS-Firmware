/**
 * File: sidetnfs_floppy_config.h
 * Description: -- persistent FLOPPY.PRG server-profile flash config.
 *
 * Entirely independent from sidetnfs_config.h's GEMDOS drive-list sector:
 * own flash sector, own magic/version/CRC32, own struct, no drive letters,
 * no GEMDOS drive slot state. FLOPPY.PRG's profiles are floppy-image
 * *sources* it browses -- TNFS network shares or local SD-card folders --
 * they never become GEMDOS drives.
 *
 * Same design philosophy as sidetnfs_config.h/sidetnfs_system_config.h:
 * magic -> version -> CRC32 -> structural validation, wholesale fallback to
 * built-in defaults on any failure (never a partial/salvaged record).
 * SAVE_PROFILES is the only command that ever touches flash -- GET/SET/
 * DELETE_PROFILE and SET_ACTIVE_PROFILE only ever change the RAM copy. See
 * the SideTNFS-Floppy-emulation project's RESEARCH-STEP0.md section 8 for
 * the full design rationale.
 *
 * RAM DISCIPLINE (read before touching this file): version 1 of this
 * module (TNFS-only, no `backend` field) briefly shipped with FOUR static
 * whole-image copies (g_config + one local static each in init()/save()'s
 * clean-image-build/save()'s readback -- the same multiple-static-copy
 * pattern sidetnfs_config.c itself uses) and a last_directory[256] field,
 * costing ~15.3KB of static RAM on a target where the free margin is
 * measured in single-digit-to-low-double-digit KB (see RESEARCH-STEP0.md's
 * own RAM-budget findings, and the real regression this caused before it
 * was reverted). This version instead uses only TWO static buffers total
 * (g_config, persistent, and one page-rounded program_buf local to
 * save()) by validating directly against the XIP-mapped flash pointer
 * during init() instead of copying to a scratch buffer first, and by
 * reading back via the XIP pointer for save()'s own verification instead
 * of a second RAM copy -- see sidetnfs_floppy_config.c's own comments.
 * Backend-specific fields (TNFS host/mount_path vs. SD sd_path) are also
 * stored in a union, not as separate always-present fields, since a given
 * profile is never both at once. Any future field added here should be
 * sized and reasoned about with this same discipline -- check the
 * resulting sizeof() against SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE AND
 * actually rebuild+measure __HeapLimit's distance from ROM_IN_RAM's start
 * (0x20020000) before assuming it is safe.
 *
 * Deliberately smaller than sidetnfs_config.h in other respects: no
 * "pending"/reinit tracking (no active runtime session depends on these
 * profiles the way the GEMDOS drive list feeds the active TNFS
 * connection), no factory-reset command, no UART diagnostic dump. Add
 * these only if a real need appears.
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
//
// SIZE grew from 4KB to 8KB when the SD backend was added (the v2 record,
// with its 256-byte sd_path, no longer fits 8 records in 4KB) -- the extra
// 4KB (0x103000-0x104000) was, and remains, confirmed free flash by the
// same evidence as the original 4KB reservation (~880KB free from this
// point to the end of the FLASH region, per RESEARCH-STEP0.md section 8).
#define SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET 0x102000u
#define SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE 8192u

#define SIDETNFS_FLOPPY_MAX_PROFILES 8
#define SIDETNFS_FLOPPY_NICKNAME_LEN 24
#define SIDETNFS_FLOPPY_HOST_LEN 64
#define SIDETNFS_FLOPPY_MOUNTPATH_LEN 32
// SD root-folder path and last-browsed-directory length -- matches the LFN
// browser's proposed CWD size (RESEARCH-STEP0.md section 6) and the
// project brief's explicit "max 256 bytes, TNFS or SD" requirement, so a
// future browser can persist "last shown directory" (any backend) and an
// SD profile can name its root folder without a record-shape change
// later. last_directory is application state, not a user-edited field --
// the profile editor does not expose it directly.
#define SIDETNFS_FLOPPY_SDPATH_LEN 256
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

// Explicit backend selector -- never inferred from an empty hostname or
// any other implicit signal. A profile is either a TNFS network share or a
// local SD-card folder, chosen once by the user in the profile editor.
typedef enum
{
    SIDETNFS_FLOPPY_BACKEND_TNFS = 1,
    SIDETNFS_FLOPPY_BACKEND_SD = 2
} sidetnfs_floppy_backend_t;

#define SIDETNFS_FLOPPY_CONFIG_MAGIC 0x464C5053u // "FLPS" -- Floppy Profile Store
#define SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION 2u
// The original (TNFS-only, no `backend` field) layout this module shipped
// with -- see sidetnfs_floppy_profile_config_v1_t below. Migrated forward
// on read, same pattern sidetnfs_config.c's own
// sidetnfs_config_migrate_v2_to_v3() uses for its analogous case.
#define SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION_V1 1u

// TNFS-only fields: same restriction the GEMDOS drive list currently
// enforces for TNFS drives (UDP only, no transport choice).
typedef struct
{
    char host[SIDETNFS_FLOPPY_HOST_LEN];
    char mount_path[SIDETNFS_FLOPPY_MOUNTPATH_LEN];
} sidetnfs_floppy_tnfs_fields_t; // 96 bytes

// SD-only fields: a single root-folder path the future browser will open
// directly on the local SD card -- no GEMDOS drive letter, no intermediate
// FatFS drive mapping (see this project's own report on why not).
typedef struct
{
    char sd_path[SIDETNFS_FLOPPY_SDPATH_LEN];
} sidetnfs_floppy_sd_fields_t; // 256 bytes

// A profile's backend-specific fields, unioned rather than kept as
// separate always-present fields -- see this header's own top-of-file RAM
// discipline note. Which member is meaningful is entirely determined by
// the record's own `backend` field; the union's raw bytes are never
// selectively cleared/patched in place (see sidetnfs_floppy_config.c's own
// comment on why), only ever fully overwritten by SET_PROFILE.
typedef union
{
    sidetnfs_floppy_tnfs_fields_t tnfs;
    sidetnfs_floppy_sd_fields_t sd;
} sidetnfs_floppy_backend_fields_t; // 256 bytes (max of the two members)

// One floppy-image source: a TNFS network share or a local SD-card
// folder, chosen explicitly via `backend`. No drive_letter -- this store
// only ever describes a place to browse, never a GEMDOS drive.
typedef struct
{
    uint8_t state;   // sidetnfs_floppy_profile_state_t
    uint8_t backend; // sidetnfs_floppy_backend_t -- meaningless when state == EMPTY
    uint16_t port;   // TNFS only; meaningless (0) for SD
    char nickname[SIDETNFS_FLOPPY_NICKNAME_LEN];
    char last_directory[SIDETNFS_FLOPPY_LASTDIR_LEN];
    sidetnfs_floppy_backend_fields_t fields;
} sidetnfs_floppy_profile_config_t; // 1+1+2+24+256+256 = 540 bytes

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

// Compile-time proof of the exact documented sizes -- 540 bytes/record (4 +
// 24 + 256 + 256), 4336 bytes total (12 + 8*540 + 4), within one 8KB
// sector with room to spare for future fields.
_Static_assert(sizeof(sidetnfs_floppy_profile_config_t) == 540, "sidetnfs_floppy_profile_config_t size drifted from the documented wire/flash layout");
_Static_assert(sizeof(sidetnfs_floppy_flash_t) == 4336, "sidetnfs_floppy_flash_t size drifted from the documented flash layout");
_Static_assert(sizeof(sidetnfs_floppy_flash_t) <= SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE, "sidetnfs_floppy_flash_t no longer fits in one SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE sector");

// ------------------------------------------------------------------ *
// Version 1 (TNFS-only, pre-SD-backend) on-flash layout, kept ONLY so
// sidetnfs_floppy_config_init() can migrate an already-saved v1 block
// forward. Never used for anything else -- new code should never construct
// one of these. Byte-identical to this module's original
// sidetnfs_floppy_profile_config_t/sidetnfs_floppy_flash_t, before the
// `backend` field and the TNFS/SD union existed.
// ------------------------------------------------------------------ */
typedef struct
{
    uint8_t state;
    uint8_t reserved0[3];
    char nickname[SIDETNFS_FLOPPY_NICKNAME_LEN];
    char host[SIDETNFS_FLOPPY_HOST_LEN];
    uint16_t port;
    uint8_t reserved1[2];
    char mount_path[SIDETNFS_FLOPPY_MOUNTPATH_LEN];
    char last_directory[SIDETNFS_FLOPPY_LASTDIR_LEN];
} sidetnfs_floppy_profile_config_v1_t; // 384 bytes -- do not change

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t active_profile_index;
    uint8_t profile_count;
    uint8_t reserved[2];
    sidetnfs_floppy_profile_config_v1_t profiles[SIDETNFS_FLOPPY_MAX_PROFILES];
    uint32_t crc32;
} sidetnfs_floppy_flash_v1_t; // 3088 bytes -- do not change

_Static_assert(sizeof(sidetnfs_floppy_profile_config_v1_t) == 384, "sidetnfs_floppy_profile_config_v1_t must stay byte-identical to the original shipped v1 layout for migration to work");
_Static_assert(sizeof(sidetnfs_floppy_flash_v1_t) == 3088, "sidetnfs_floppy_flash_v1_t must stay byte-identical to the original shipped v1 layout for migration to work");
_Static_assert(sizeof(sidetnfs_floppy_flash_v1_t) <= SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE, "sidetnfs_floppy_flash_v1_t must still fit in the (now larger) flash sector so migration can read it in place");

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
    SIDETNFS_FLOPPY_STATUS_UNSUPPORTED_VERSION = 9,
    SIDETNFS_FLOPPY_STATUS_INVALID_BACKEND = 10,   // backend byte outside 1/2 (TNFS/SD)
    SIDETNFS_FLOPPY_STATUS_INVALID_SD_PATH = 11    // SD backend, sd_path empty
} sidetnfs_floppy_config_status_t;

// Load and validate the flash config block exactly once at boot: magic,
// then flash-format version (current, or the one older migratable
// version -- see sidetnfs_floppy_profile_config_v1_t above), then CRC32
// over the whole (current-version) block, then active_profile_index range
// and every configured record's own fields (including `backend` and its
// backend-specific fields). Falls back to the built-in default (all eight
// slots EMPTY, active index 0) on blank/erased flash or ANY validation
// failure -- never attempts to salvage individual records out of a block
// that failed any check. A v1 block that fails its OWN CRC check is
// likewise treated as a fallback case, not partially migrated. Never
// writes to flash. Must be called before GEMDRVEMUL can process any
// GEMDRVEMUL_FLOPPY_* command (see main.c).
void sidetnfs_floppy_config_init(void);

// True iff the currently-active config came from a validated flash block
// this boot (native v2 OR successfully migrated from v1) -- false means
// sidetnfs_floppy_config_init() fell back to the built-in (all-empty)
// default.
bool sidetnfs_floppy_config_loaded_from_flash(void);

// True iff this boot's active config was migrated (RAM only, never
// written back to flash by init() itself) from a v1 block. Only
// meaningful when sidetnfs_floppy_config_loaded_from_flash() is true.
bool sidetnfs_floppy_config_migrated_from_v1(void);

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
//   DISABLED / ENABLED:  nickname is always required; backend must be
//                        TNFS or SD (INVALID_BACKEND otherwise); then,
//                        depending on backend:
//                          TNFS: host and port required (INVALID_HOST/
//                                INVALID_PORT), mount_path may be empty
//                                ("server root").
//                          SD:   sd_path required (INVALID_SD_PATH) --
//                                host/port are never validated or stored
//                                for an SD profile.
//                        last_directory may always be empty ("not yet
//                        browsed").
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
// disabled only around the erase+program pair), then reads back directly
// from the XIP-mapped flash pointer (no extra RAM copy -- see this
// header's own RAM discipline note) and re-validates magic/version/CRC
// before reporting success. Returns FLASH_WRITE_FAILED if the readback
// magic/version don't match, CRC_MISMATCH if the CRC doesn't match. Never
// retries and never reboots. This is the ONLY function in this module
// that ever touches flash -- selecting a different active profile or
// editing a profile in FLOPPY.PRG's UI does not, by itself, write flash;
// the user must reach an explicit "Save" action (mirrors SAVE_CONFIG's
// contract exactly, see sidetnfs_config.h). Always writes the CURRENT
// (v2) format -- a migrated-from-v1 config's first Save upgrades it on
// flash permanently.
sidetnfs_floppy_config_status_t sidetnfs_floppy_config_save(void);

#endif // SIDETNFS_FLOPPY_CONFIG_H
