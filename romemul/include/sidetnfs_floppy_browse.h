/**
 * File: sidetnfs_floppy_browse.h
 * Description: FLOPPY.PRG's real LFN directory browser backend (Step 2 of
 * the SideTNFS-Floppy-emulation project). Browses ONE active profile's
 * real TNFS/SD source directly -- no GEMDOS drive/letter, no
 * Fsfirst/Fsnext, no 8.3 conversion/aliasing/case conversion. Names come
 * straight from TNFS's own READDIRX response / FatFS's own FILINFO.fname.
 *
 * Exactly one browse session is active at a time (a single static
 * floppy_browse_state_t inside sidetnfs_floppy_browse.c) -- FLOPPY.PRG
 * itself is a single Atari-side client, same assumption the rest of this
 * protocol already makes. Opening a new profile, or changing directory,
 * replaces the previous CWD outright and bumps a generation counter;
 * GET_*_PAGE callers must echo back the generation they last received, so
 * a request that targets a directory the browser has since left (a stale
 * page from before a CHANGE_DIR, a reboot, ...) is rejected instead of
 * silently answering with the wrong directory's contents.
 *
 * Paging re-walks the backend directory from the start up to the
 * requested page_index on every genuinely NEW request, rather than
 * keeping a live cursor across DIFFERENT requests. This gives a strong
 * correctness property: FIRST/NEXT/PREVIOUS/arbitrary-page and a TNFS
 * timeout+reconnect mid-browse are ALL just "the caller asked for a
 * different page_index"; there is no server-side per-direction cursor
 * that can go stale or point at the wrong place after a reconnect.
 *
 * A single walk, however, is NOT carried to completion inside one
 * blocking Pico-side call if that would take more than a handful of real
 * TNFS round trips: GET_*_PAGE's dispatch handler runs on the same core
 * that must keep answering the Atari's time-critical bus reads, so it
 * must always return quickly, however deep the page or however large the
 * directory. sidetnfs_floppy_browse_get_page() instead does a small,
 * bounded amount of work per call and returns
 * FLOPPY_BROWSE_STATUS_IN_PROGRESS (not an error) when more remains,
 * parking its progress (including any open TNFS dir handle) in the same
 * static session state; the Atari-side client re-issues the IDENTICAL
 * request to resume it, invisibly to FLOPTEST.PRG/the future Step 3
 * browser -- see that function's own comment for the full contract. SD
 * reads have no unbounded network wait to chunk around and always finish
 * within one call.
 *
 * Directories and files are always listed as two independent result sets
 * (see sidetnfs_floppy_browse_get_page's `want_dirs`) -- a page never
 * mixes the two, so no per-entry type field is needed on the wire (see
 * GEMDRVEMUL_FLOPPY_PAGE in gemdrvemul.h).
 *
 * Backend abstraction: TNFS reuses sidetnfs_probe.c's existing per-slot
 * MOUNT/session machinery (slots SIDETNFS_PROBE_FLOPPY_SLOT_BASE.. --
 * see sidetnfs_probe.h) and its existing fslisting_send_opendirx/
 * readdirx/closedir()+fslisting_wait_for() wire functions; SD uses FatFS's
 * f_findfirst()/f_findnext() directly, exactly like the existing SD
 * Fsfirst/Fsnext code (gemdrvemul.c) already does, just without that
 * code's 8.3 filtering/dotfile-vs-real-dotfile distinction. Path safety
 * (root-boundary enforcement, "."/".." handling, separator
 * normalization) is a single shared routine used by both backends,
 * mirroring gemdrvemul.c's own normalize_gemdos_path()/
 * sidetnfs_sd_build_fatfs_path() contract ("normalize, then always
 * concatenate AFTER the profile's own root, never splice in the middle" --
 * makes "above root" unrepresentable without a separate escape check).
 */
#ifndef SIDETNFS_FLOPPY_BROWSE_H
#define SIDETNFS_FLOPPY_BROWSE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FLOPPY_BROWSE_CWD_LEN 256   // matches SIDETNFS_FLOPPY_LASTDIR_LEN / the Step 2 spec's "CWD max 256 bytes incl. terminator"
#define FLOPPY_BROWSE_NAME_LEN 256  // matches the Step 2 spec's "each name max 256 bytes incl. terminator"
#define FLOPPY_BROWSE_PAGE_ENTRIES 25 // matches RESEARCH-STEP0.md's page size and the Step 2 spec

typedef enum
{
    FLOPPY_BROWSE_OK = 0,
    FLOPPY_BROWSE_ERR_INVALID_PROFILE = 1,          // index out of range, or the slot is EMPTY
    FLOPPY_BROWSE_ERR_SOURCE_NOT_CONFIGURED = 2,     // backend's required field (host/sd_path) is blank
    FLOPPY_BROWSE_ERR_TNFS_NOT_CONNECTED = 3,        // no WiFi, or host never resolved
    FLOPPY_BROWSE_ERR_TNFS_HOST_UNREACHABLE = 4,     // MOUNT sent, no response within the bounded wait, or mount rc != OK
    FLOPPY_BROWSE_ERR_SD_NOT_PRESENT = 5,            // SD service hasn't run yet, or global status isn't READY
    FLOPPY_BROWSE_ERR_DIR_NOT_FOUND = 6,             // CWD/target subdir does not exist on the backend
    FLOPPY_BROWSE_ERR_ACCESS_DENIED = 7,             // backend reported a permission error
    FLOPPY_BROWSE_ERR_PATH_TOO_LONG = 8,             // normalized CWD+name would exceed FLOPPY_BROWSE_CWD_LEN or the backend's own path buffer
    FLOPPY_BROWSE_ERR_NAME_TOO_LONG = 9,             // one backend entry's own name exceeds FLOPPY_BROWSE_NAME_LEN -- that entry is skipped, never truncated into a different name
    FLOPPY_BROWSE_ERR_INVALID_PAGE_REQUEST = 10,     // page_index would need to be negative (never actually representable, kept for protocol completeness) or browse session not open
    FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY = 11,      // page_index is past the last real page -- NOT a hard error, response is still well-formed (count=0)
    FLOPPY_BROWSE_ERR_STALE_GENERATION = 12,         // caller's generation no longer matches the active browse session
    FLOPPY_BROWSE_ERR_BACKEND_ERROR = 13,            // generic TNFS/SD I/O error mid-listing (timeout after the session was already established, unexpected FRESULT, malformed response, ...)
    FLOPPY_BROWSE_ERR_NOT_OPEN = 14,                 // CHANGE_DIR/GET_*_PAGE called before a successful BROWSE_OPEN
    FLOPPY_BROWSE_STATUS_IN_PROGRESS = 15             // NOT an error: GET_*_PAGE's walk isn't finished yet -- caller must re-issue the IDENTICAL request (same generation/want_dirs/page_index) to resume it. See sidetnfs_floppy_browse_get_page()'s own comment.
} sidetnfs_floppy_browse_status_t;

// Opens `profile_index` for browsing: resolves the backend, establishes
// (or reuses) a TNFS session for TNFS profiles, sets the CWD to the
// profile's own stored last_directory (or root "/" if empty or no longer
// valid on the backend), and starts a new generation. Any previously
// active browse session (a different profile, or a stale CWD) is replaced
// outright. *out_cwd is always left NUL-terminated and valid (root "/" on
// any failure that leaves no better answer); *out_generation is always
// written. out_cwd_size must be >= FLOPPY_BROWSE_CWD_LEN.
// `network_ok` mirrors gemdrvemul.c's own boot-time-latched
// sidetnfs_network_ok local (WiFi confirmed up at boot) -- the same value
// already threaded into sidetnfs_probe_classify_slot_error() elsewhere in
// that file. Only consulted for a TNFS profile, to tell "no WiFi at all"
// apart from "WiFi is up but this host never resolved/responded".
sidetnfs_floppy_browse_status_t sidetnfs_floppy_browse_open(uint8_t profile_index, bool network_ok,
                                                              uint32_t *out_generation, char *out_cwd,
                                                              size_t out_cwd_size);

// Changes the active CWD. go_up=true moves to the parent -- a no-op at the
// profile's own configured root, never an error (mirrors
// normalize_gemdos_path()'s own "at root, '..' is a no-op" contract).
// go_up=false descends into `name`, which must be one of the CURRENT CWD's
// own subdirectory names (as most recently returned by a dir-kind
// GET_*_PAGE call) -- not re-validated against a specific earlier page,
// only against the backend at the moment of the call. `generation` must
// match the browse session's current generation
// (FLOPPY_BROWSE_ERR_STALE_GENERATION otherwise, e.g. the caller is
// acting on a directory listing it has since left). On success the CWD
// and generation are updated and both are written to *out_cwd/
// *out_generation; on failure the browse session's CWD/generation are
// left completely unchanged (also still echoed back), so a failed
// CHANGE_DIR never leaves a stale-looking CWD.
sidetnfs_floppy_browse_status_t sidetnfs_floppy_browse_change_dir(uint32_t generation, bool go_up, const char *name,
                                                                    uint32_t *out_generation, char *out_cwd,
                                                                    size_t out_cwd_size);

typedef struct
{
    sidetnfs_floppy_browse_status_t status;
    uint32_t generation;
    uint32_t page_index;
    uint16_t count;
    bool has_prev;
    bool has_next;
} floppy_browse_page_result_t;

// Fetches page `page_index` (0-based) of the active CWD's subdirectory
// names (want_dirs=true) or file names (want_dirs=false). Each matched
// entry name is written directly into the ROM3 shared-memory window at
// memory_shared_address+entries_offset+ (slot * FLOPPY_BROWSE_NAME_LEN),
// up to FLOPPY_BROWSE_PAGE_ENTRIES slots, using the same byte-copy +
// CHANGE_ENDIANESS_BLOCK16 convention every other Pico->Atari string field
// in this protocol already uses -- no separate RAM page buffer is ever
// allocated (see this project's own RAM-discipline history).
//
// Re-walks the backend directory from the start every time a NEW request
// arrives (different generation/want_dirs/page_index than whatever was
// last in progress), counting matching (dir-or-file, per want_dirs)
// entries, skipping page_index*FLOPPY_BROWSE_PAGE_ENTRIES of them, then
// collecting up to FLOPPY_BROWSE_PAGE_ENTRIES more -- so
// FIRST/NEXT/PREVIOUS/arbitrary-page and a TNFS timeout+reconnect
// mid-browse are all simply "the caller asked for a different
// page_index"; there is no server-side per-direction cursor to invalidate
// or get out of sync. A page_index past the last real page returns
// FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY (count=0, has_next=false,
// has_prev=(page_index>0)) -- a well-formed, valid-empty page, not a hard
// error.
//
// IMPORTANT -- this call can return FLOPPY_BROWSE_STATUS_IN_PROGRESS: a
// TNFS walk is never carried to completion inside one call if that would
// take more than a handful of real network round trips (each dispatch
// call runs on the same core that must keep answering the time-critical
// Atari bus, so it must always return quickly). When this happens, no
// entries have been published for the caller to see yet -- the caller
// (the Atari-side client) must re-issue the IDENTICAL request (same
// generation/want_dirs/page_index) to resume exactly where this call left
// off, repeating until a terminal status (OK/END_OF_DIRECTORY/an error)
// comes back. SD walks have no unbounded network wait to chunk around, so
// they always finish within one call in practice.
floppy_browse_page_result_t sidetnfs_floppy_browse_get_page(uint32_t generation, bool want_dirs, uint32_t page_index,
                                                              uint32_t memory_shared_address,
                                                              uint32_t entries_offset);

#endif // SIDETNFS_FLOPPY_BROWSE_H
