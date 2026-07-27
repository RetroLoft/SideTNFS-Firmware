/**
 * File: sidetnfs_sd_service.h
 * A single, one-shot, non-blocking
 * boot-time SD card/mount check plus a per-ENABLED-SD-drive directory
 * check -- never the full FatFS/SD GEMDOS file backend (that is ).
 * Mirrors sidetnfs_probe.c's own TNFS multi-slot pattern
 * (sidetnfs_probe_set_slot_context()/sidetnfs_probe_mount_runtime_slots()/
 * sidetnfs_probe_get_slot_context()), applied to SD instead of TNFS. Keeps
 * raw FatFS/FRESULT/FATFS/FILINFO details entirely inside
 * sidetnfs_sd_service.c -- this header only exposes plain enums/structs,
 * same layering discipline sidetnfs_probe.h already uses for TNFS.
 */
#ifndef SIDETNFS_SD_SERVICE_H
#define SIDETNFS_SD_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sidetnfs_config.h" // SIDETNFS_SDPATH_LEN
#include "sidetnfs_probe.h"  // SidetnfsDirSearchResult/SidetnfsAtariDirEntry -- shared listing
                              // result types (SETTINGS' own config-drive backend already
                              // reuses these too); no TNFS session/state coupling implied.

// Mirrors gemdrvemul.c's GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES (9),
// duplicated as a literal rather than included -- same reasoning as
// sidetnfs_probe.h's own SIDETNFS_PROBE_MAX_RUNTIME_SLOTS (this header must
// not depend on gemdrvemul.h; the dependency already goes the other way).
#define SIDETNFS_SD_SERVICE_MAX_SLOTS 9

// Minimum required status set. READY covers both "the physical
// card/mount is fine" (global) and "this slot's own sd_path exists and is
// a directory" (per-slot) -- see sidetnfs_sd_get_drive_status()'s own
// comment for how the two combine.
typedef enum
{
    SIDETNFS_SD_STATUS_READY = 0,
    SIDETNFS_SD_STATUS_ABSENT,
    SIDETNFS_SD_STATUS_INIT_FAILED,
    SIDETNFS_SD_STATUS_MOUNT_FAILED,
    SIDETNFS_SD_STATUS_FILESYSTEM_ERROR,
    SIDETNFS_SD_STATUS_DIRECTORY_NOT_FOUND,
    SIDETNFS_SD_STATUS_NOT_A_DIRECTORY,
} sidetnfs_sd_status_t;

typedef struct
{
    bool valid;                        // true once sidetnfs_sd_service_set_slot_path() has populated this slot
    int runtime_slot;
    char driveletter;                  // uppercase ASCII
    char sd_path[SIDETNFS_SDPATH_LEN];  // as configured, e.g. "/hd"
    sidetnfs_sd_status_t status;        // this slot's own final status after sidetnfs_sd_service_run()
    uint8_t fresult_raw;                // the raw FatFS FRESULT behind `status` (0xFF if none applies, e.g. INIT_FAILED)
} sidetnfs_sd_drive_status_t;

// Registers slot `slot`'s driveletter + configured sd_path -- pure
// bookkeeping, no SD/FatFS access. Call once per ENABLED SD-backend slot
// from sidetnfs_runtime_drives_init(), same point sd_path is already
// known from the slot's own persisted sidetnfs_drive_config_t. Must be
// called for every SD slot BEFORE sidetnfs_sd_service_run(). No-op if
// slot is out of range.
void sidetnfs_sd_service_set_slot_path(int slot, char driveletter, const char *sd_path);

// The one boot-time SD detection/mount/per-slot directory check --
// see gemdrvemul.c's own call-site comment for exactly where this runs.
// Never retries, never loops waiting for a card, never blocks WiFi/TNFS/
// SETTINGS/the GEMDRIVE command loop. A second call this boot is a no-op
// (see sidetnfs_sd_service_has_run()) -- this function does real SD/FatFS
// I/O exactly once per boot.
void sidetnfs_sd_service_run(void);

// True once sidetnfs_sd_service_run() has completed (whatever the
// outcome) this boot.
bool sidetnfs_sd_service_has_run(void);

// The shared physical card/mount status. SIDETNFS_SD_STATUS_READY here
// only means "card present, driver initialized, FatFS mounted on the SD
// volume" -- an individual slot can still be DIRECTORY_NOT_FOUND/
// NOT_A_DIRECTORY on top of a globally READY card (see
// sidetnfs_sd_get_drive_status()). Meaningless before
// sidetnfs_sd_service_has_run() is true (reads as SIDETNFS_SD_STATUS_ABSENT).
sidetnfs_sd_status_t sidetnfs_sd_global_status(void);

// The raw FatFS FRESULT behind sidetnfs_sd_global_status() (0xFF if the
// failure happened before f_mount() was ever reached, e.g. INIT_FAILED).
uint8_t sidetnfs_sd_global_fresult(void);

// Fills *out with slot `slot`'s full SD status -- combines the global
// card/mount status with this slot's own sd_path directory check: if the
// card/mount itself isn't READY, every registered slot mirrors that same
// global status (a card problem is never slot-specific); if the card/mount
// IS READY, each slot's status reflects only its OWN sd_path check
// (DIRECTORY_NOT_FOUND/NOT_A_DIRECTORY/READY), completely independent of
// any other slot's sd_path. Returns false (out untouched) if the slot was
// never registered via sidetnfs_sd_service_set_slot_path().
bool sidetnfs_sd_get_drive_status(int slot, sidetnfs_sd_drive_status_t *out);

// Short, fixed, human-readable description of a status (e.g. "No SD card
// inserted.") -- never NULL, never allocates.
const char *sidetnfs_sd_status_text(sidetnfs_sd_status_t status);

// Short recovery hint for a status (e.g. "Insert an SD card.") -- never
// NULL, never allocates.
const char *sidetnfs_sd_status_hint(sidetnfs_sd_status_t status);

// Exact filename shown for an ENABLED SD drive whose backend
// isn't READY -- Fsfirst/Fsnext ever produce exactly this one entry for
// such a drive, nothing else (no "."/"..", never NO_NETW.TXT/NET_ERR.TXT
// -- SD is never routed through the TNFS fake-listing path).
#define SIDETNFS_SD_ERROR_NAME "SD_ERROR.TXT"
// Generous fixed upper bound for the generated SD_ERROR.TXT body.
#define SIDETNFS_SD_ERROR_TEXT_MAX 320

// Builds SD_ERROR.TXT's body text for runtime slot `slot`. Always
// NUL-terminates and never writes past out_size. Returns the text length
// actually written (excluding the NUL) -- also the exact file size
// Fsfirst/Fopen must report (test H) -- the two must never disagree.
size_t sidetnfs_build_sd_error_text(int slot, char driveletter, char *out, size_t out_size);

// A small, dedicated, memory-only listing for exactly one
// synthetic entry (SIDETNFS_SD_ERROR_NAME) -- the SD equivalent of
// sidetnfs_probe.c's fake/NET_ERR listings, but entirely separate state
// (own search-slot table, own file) so SD is never routed through the
// TNFS fake-listing/NET_ERR mechanism ("Routeer SD nooit via de
// TNFS-foutbackend"). path == "/" yields exactly one entry; any other
// path yields an immediately-empty listing. Registers under ndta, same
// "one slot per concurrently-active search" shape as the TNFS-side
// tables -- sidetnfs_sd_error_search_next()/is_active()/close() work
// identically regardless of which drive/slot started the search.
SidetnfsDirSearchResult sidetnfs_sd_error_search_start(uint32_t ndta, int slot, char driveletter, const char *path,
                                                         const char *pattern, uint8_t attribs,
                                                         SidetnfsAtariDirEntry *out_entry);
SidetnfsDirSearchResult sidetnfs_sd_error_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry);
bool sidetnfs_sd_error_search_is_active(uint32_t ndta);
void sidetnfs_sd_error_search_close(uint32_t ndta);
void sidetnfs_sd_error_search_close_all(void);
uint16_t sidetnfs_sd_error_search_count_active(void);

#endif // SIDETNFS_SD_SERVICE_H
