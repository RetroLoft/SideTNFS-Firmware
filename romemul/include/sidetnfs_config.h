/**
 * File: sidetnfs_config.h
 * Description: Fase 9C -- persistent SideTNFS drive-list flash config.
 * Replaces the Fase 9B2 max-8-servers model (never committed) with one
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

// Fase 9B2/9C: dedicated 4KB flash sector for the SideTNFS config, re-proven
// free against the current linker script (romemul/memmap_romemul.ld) --
// ROM_FLASH ends at exactly XIP_BASE + 0x100000 (0x100E0000, length 128k),
// and CONFIG_FLASH/ROM_FLASH are the only other reserved flash regions,
// both < 0x100000. See docs/sidetnfs-config-protocol.md for the full
// evidence trail (unchanged since Fase 9B2).
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

typedef enum
{
    SIDETNFS_TRANSPORT_UDP = 0,
    SIDETNFS_TRANSPORT_TCP = 1 // stored for future use; the UI/validation
                               // this phase only ever accepts UDP for a new
                               // TNFS drive, and no TCP socket code exists
                               // anywhere in this module or its callers yet.
} sidetnfs_transport_t;

#define SIDETNFS_CONFIG_MAGIC 0x53544446u // "STDF"
#define SIDETNFS_CONFIG_FLASH_VERSION 2u

// Pointer-free, fixed-layout record -- no union: SD drives use
// nickname/drive_letter/sd_path, TNFS drives use
// nickname/drive_letter/transport/host/port/mount_path; the fields that
// don't apply to a given type are always zeroed (enforced by
// sidetnfs_config_set_drive() and sidetnfs_config_save()).
typedef struct
{
    uint8_t used;
    uint8_t drive_letter; // uppercase ASCII, e.g. 'N'
    uint8_t type;         // sidetnfs_drive_type_t
    uint8_t transport;    // sidetnfs_transport_t (TNFS only)
    uint16_t port;        // TNFS only
    uint8_t reserved0[2];

    char nickname[SIDETNFS_NICKNAME_LEN];
    char host[SIDETNFS_HOST_LEN];             // TNFS only
    char mount_path[SIDETNFS_MOUNTPATH_LEN];  // TNFS only
    char sd_path[SIDETNFS_SDPATH_LEN];        // SD only
} sidetnfs_drive_config_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t config_drive_letter; // e.g. 'S'; never A/B, never equal to any drives[] letter
    uint8_t drive_count;         // number of used records in drives[] -- excludes the config drive
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
    SIDETNFS_CONFIG_STATUS_UNSUPPORTED_VERSION = 14
} sidetnfs_config_status_t;

// Fase 9C: load and validate the flash config block exactly once at boot
// (magic, flash-format version, CRC32 over the whole block, then
// config_drive_letter/drive_count range checks, then every used record's
// own fields and letter-uniqueness against every other used record AND
// against config_drive_letter). Falls back to the built-in default (config
// drive 'S', one TNFS drive 'N'/RetroLoft/192.168.178.10:16384/Atari.ST --
// see sidetnfs_config.c) on blank/erased flash or ANY validation failure;
// never attempts to salvage individual records out of a block that failed
// any check. Never writes to flash. Must be called before GEMDRVEMUL can
// process any of the SIDETNFS config commands (see main.c).
void sidetnfs_config_init(void);

uint32_t sidetnfs_config_get_max_drives(void);
uint8_t sidetnfs_config_get_drive_count(void);
uint8_t sidetnfs_config_get_config_drive_letter(void);

// GET_DRIVE: fills *out with drives[index] (zeroed first, so a non-OK
// result always leaves *out fully zeroed). INVALID_INDEX if index >=
// SIDETNFS_MAX_DRIVES, EMPTY_SLOT if the slot's used flag is 0, otherwise
// OK with *out filled in and all strings guaranteed NUL-terminated.
sidetnfs_config_status_t sidetnfs_config_get_drive(uint8_t index, sidetnfs_drive_config_t *out);

// SET_DRIVE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_MAX_DRIVES. If in->used == 0, the slot is cleared (equivalent
// to sidetnfs_config_delete_drive()) without any type-specific field
// validation. If in->used != 0, the record is fully validated (drive
// letter, type, and type-specific fields) and checked for letter
// uniqueness against every other used drive and against the config drive
// letter before being written; irrelevant fields for the record's type
// are zeroed before storing (SD zeroes transport/host/mount_path, TNFS
// zeroes sd_path). drive_count is recomputed after every call.
sidetnfs_config_status_t sidetnfs_config_set_drive(uint8_t index, const sidetnfs_drive_config_t *in);

// DELETE_DRIVE: RAM only, no flash write. INVALID_INDEX if index >=
// SIDETNFS_MAX_DRIVES, EMPTY_SLOT if already unused. Fully zeroes the
// record and recomputes drive_count. Can never touch the config drive --
// there is no config-drive index in this array.
sidetnfs_config_status_t sidetnfs_config_delete_drive(uint8_t index);

// SET_CONFIG_DRIVE: RAM only, no flash write. Rejects A/B and any letter
// already used by a used ordinary drive.
sidetnfs_config_status_t sidetnfs_config_set_config_drive_letter(uint8_t new_letter);

// SAVE_CONFIG: validates the full RAM config (config drive letter, every
// used record, letter uniqueness), builds a clean flash image (reserved
// bytes zeroed, unused records fully zeroed, drive_count recomputed from
// the used bitmap), erases exactly one SIDETNFS_CONFIG_FLASH_SIZE sector,
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
// RESET does not reboot the Pico (see report, Fase 9E).
sidetnfs_config_status_t sidetnfs_config_save(void);

// Fase 9E: true from the moment a SAVE_CONFIG succeeds until the newly
// saved configuration has actually been adopted by the running GEMDRIVE
// session (see sidetnfs_probe_reinit_active_server() in sidetnfs_probe.c,
// invoked from the proven Atari-reset boundary in gemdrvemul.c --
// GEMDRVEMUL_PING, but only the first one after the very first PING this
// Pico boot). Always false before the first SAVE_CONFIG this boot.
bool sidetnfs_config_is_pending(void);

// Fase 9E: clear the pending flag. Must only be called once the new
// configuration has been safely read back into the active server/drive
// state -- never on a failed or skipped reinit attempt, so a later Atari
// reset still retries.
void sidetnfs_config_clear_pending(void);

#endif // SIDETNFS_CONFIG_H
