#ifndef SIDETNFS_CONFIG_DRIVE_BACKEND_H
#define SIDETNFS_CONFIG_DRIVE_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include "sidetnfs_probe.h" // SidetnfsAtariDirEntry, SidetnfsDirSearchResult

// Fase 10B: read-only, root-only virtual drive backend serving the two
// Fase 10A flash-embedded files (SIDETNFS.PRG, README.TXT) directly from
// their existing const arrays (romemul/sidetnfs_config_drive.c) -- no RAM
// copy, no SD/WiFi/TNFS access. Only reachable when SIDETNFS_CONFIG_DRIVE_ONLY
// (see sidetnfs_probe.h, default 0) selects it as the sole GEMDRIVE backend
// for a temporary test build. Mirrors the shape (not the storage) of
// sidetnfs_probe.c's TNFS DTA registry / fake no-network search table, so
// gemdrvemul.c's existing Fsfirst/Fsnext call sites need no new pattern.

// Number of fixed files this drive ever serves.
#define SIDETNFS_CONFIG_DRIVE_FILE_COUNT 2

// Fase 10B: fixed placeholder date/time (2026-07-20 12:00:00, DOS/GEMDOS
// packed format) for both files -- the Fase 10A generator does not record
// source mtimes, and these are build-time-embedded flash contents with no
// single meaningful "file time" of their own. A valid, non-zero,
// non-corrupt fixed value (rather than 0/0, which some GEMDOS software
// treats as an invalid/corrupt entry -- see gemdrvemul.c's own
// unix_epoch_to_dos_datetime() 1980-01-01 fallback for the same reasoning)
// is all "correct" requires here. Shared with gemdrvemul.c's
// GEMDRVEMUL_FDATETIME_CALL inquire handling for GEMDRIVE_FILE_BACKEND_CONFIG_FLASH.
#define SIDETNFS_CONFIG_DRIVE_DATE 23796u // ((2026-1980)<<9)|(7<<5)|20
#define SIDETNFS_CONFIG_DRIVE_TIME 24576u // (12<<11)|(0<<5)|(0/2)

// Read-only + archive -- these files can never be written, and ARCH is the
// conventional "ordinary file" bit (matches sidetnfs_gemdos_attr_match()'s
// own "attr==0 counts as ARCH" fallback used elsewhere, made explicit here
// instead of relying on that fallback). Shared with gemdrvemul.c's
// GEMDRVEMUL_FATTRIB_CALL inquire handling for GEMDRIVE_FILE_BACKEND_CONFIG_FLASH.
#define SIDETNFS_CONFIG_DRIVE_ATTR (FS_ST_READONLY | FS_ST_ARCH)

// True if `name83` (an already drive-letter/path-stripped, single path
// component, e.g. "SIDETNFS.PRG") matches one of the two files.
// Case-insensitive. On true, *out_data/*out_size point directly at the
// existing flash array -- valid for the lifetime of the firmware, never
// freed, never copied.
bool sidetnfs_config_drive_lookup(const char *name83, const uint8_t **out_data, uint32_t *out_size);

// Fsfirst-equivalent: start a new root-directory search for `ndta`,
// matching `pattern`/`attribs` against the two fixed entries in a fixed,
// deterministic order (SIDETNFS.PRG then README.TXT) -- every matching
// entry is considered, including the first; nothing here ever
// synthesizes or skips a "."/".." entry, since there is no such entry to
// begin with. Returns SIDETNFS_DIR_SEARCH_FOUND with *out_entry filled
// in, SIDETNFS_DIR_SEARCH_NOT_FOUND if neither file matches, or (Fase
// 10B2) SIDETNFS_DIR_SEARCH_ERROR if every search slot is already active
// for other ndta's -- never evicts another ndta's in-progress search.
SidetnfsDirSearchResult sidetnfs_config_drive_search_start(uint32_t ndta, const char *pattern, uint8_t attribs,
                                                             SidetnfsAtariDirEntry *out_entry);

// Fsnext-equivalent: continue ndta's search. SIDETNFS_DIR_SEARCH_NOT_FOUND
// if ndta has no active search (never started via
// sidetnfs_config_drive_search_start(), or already exhausted).
SidetnfsDirSearchResult sidetnfs_config_drive_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry);

// Release ndta's search slot, if any (no-op otherwise). Mirrors
// sidetnfs_tnfs_dta_release()/sidetnfs_fake_search_close(); pure RAM, no I/O.
void sidetnfs_config_drive_search_close(uint32_t ndta);

// True if ndta currently has an active config-drive search slot. Mirrors
// sidetnfs_tnfs_dta_is_active()/sidetnfs_fake_search_is_active() -- used by
// GEMDRVEMUL_DTA_EXIST_CALL so the 68k side's pre-Fsnext existence check
// recognizes a config-drive search too.
bool sidetnfs_config_drive_search_is_active(uint32_t ndta);

// Number of currently active config-drive search slots. Mirrors
// sidetnfs_tnfs_dta_count_active()/sidetnfs_fake_search_count_active() --
// used as (part of) the GEMDRVEMUL_DTA_RELEASE_CALL return value.
uint32_t sidetnfs_config_drive_search_count_active(void);

// Close every active config-drive search slot, regardless of ndta. Mirrors
// sidetnfs_tnfs_dta_release_all()/sidetnfs_fake_search_close_all() -- called
// at every config-drive reset moment (first PING, Fase 9E reinit after a
// saved config change, and any other full DTA/file-handle cleanup) so a
// search slot from a previous Atari session can never survive into a new
// one.
void sidetnfs_config_drive_search_close_all(void);

// Dfree-equivalent: fixed 256 KiB virtual capacity, reported entirely
// full (0 free clusters) -- a read-only drive has nothing to write into.
// bytes_per_sector * sectors_per_cluster * total_clusters == 256 KiB.
void sidetnfs_config_drive_get_disk_info(uint32_t *out_total_clusters, uint32_t *out_free_clusters,
                                          uint32_t *out_bytes_per_sector, uint32_t *out_sectors_per_cluster);

#endif // SIDETNFS_CONFIG_DRIVE_BACKEND_H
