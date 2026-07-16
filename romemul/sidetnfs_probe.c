/**
 * File: sidetnfs_probe.c
 * Description: One-shot, fire-and-forget UDP reachability probe toward the
 * TNFS server, plus a small dirty-flag-driven DEBUG.TXT status file. See
 * include/sidetnfs_probe.h.
 */
#include "include/sidetnfs_probe.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h> // gmtime() -- already used elsewhere (rtcemul.c), safe/available here too
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "f_util.h"
#include "include/filesys.h"  // FS_ST_* Atari attribute bits
#include "include/commands.h" // GEMDRVEMUL_*_CALL ids -- for COMMAND_ENTER name decode only

#define SIDETNFS_SERVER_IP "192.168.178.10"
#define SIDETNFS_SERVER_PORT 16384
#define SIDETNFS_MOUNT_NAME "Atari.ST"

#define TNFS_CMD_MOUNT 0x00u
#define TNFS_CMD_OPENDIRX 0x17u
#define TNFS_CMD_READDIRX 0x18u
// Fase 5AA: standard TNFS CLOSEDIR opcode -- shared by both the classic
// OPENDIR (0x10) and extended OPENDIRX (0x17) handle namespace in the
// published TNFS protocol (the same protocol family this project's
// MOUNT/OPENDIRX/READDIRX opcodes above already follow). Not previously
// used anywhere in this codebase, and not independently verifiable against
// local server source (none present in this repo) -- if this value turns
// out to be wrong, fslisting_wait_for()'s bounded timeout below still
// guarantees CLOSEDIR failure never blocks or crashes, only skips the
// server-side cleanup (see report).
#define TNFS_CMD_CLOSEDIR 0x12u
#define TNFS_PROTO_VER_MINOR 0x02
#define TNFS_PROTO_VER_MAJOR 0x01

#define TNFS_OK 0x00u
#define TNFS_EOF 0x21u

// READDIRX entry flags (bit 0 of the per-entry flags byte)
#define TNFS_DIRENTRY_DIR 0x01u
#define TNFS_DIRENTRY_HIDDEN 0x02u
#define TNFS_DIRENTRY_SPECIAL 0x04u

// Set to 1 to append a raw hex dump of the last response to DEBUG.TXT.
// Off by default -- Fase 5G/5I want short, human-readable status lines only.
#ifndef SIDETNFS_DEBUG_SHOW_RAW
#define SIDETNFS_DEBUG_SHOW_RAW 0
#endif

#define SIDETNFS_DEBUG_RAW_SIZE 16
#define SIDETNFS_DEBUG_WRITE_MIN_INTERVAL_MS 250

// Response parse buffer: large enough for a READDIRX batch (a few entries
// with short 8.3-style Atari names). MOUNT/OPENDIRX responses are much
// smaller and always fit comfortably too.
#define SIDETNFS_RX_BUF_SIZE 256

// Entries requested per READDIRX round-trip, and a hard cap on the number
// of round-trips so a pathological/misbehaving server can never turn this
// into an unbounded (if non-blocking) request loop.
#define SIDETNFS_READDIRX_BATCH_SIZE 4u
#define SIDETNFS_READDIRX_MAX_ROUNDS 32u

// Fase 5L: small, fixed test set of GEMDOS-style patterns counted against
// each successfully normalized READDIRX entry during the existing root
// scan. Kept intentionally short (<=5) per the debug-line budget.
#define SIDETNFS_MATCH_PATTERN_COUNT 5
static const char *const SIDETNFS_MATCH_PATTERNS[SIDETNFS_MATCH_PATTERN_COUNT] = {
    "*.*", "*", "*.PRG", "*.ACC", "CONFIG.*"};

// Fase 5O: entries fetched per READDIRX round while (re)building a
// directory-cache slot.
#define SIDETNFS_FS_BATCH_SIZE 8u

// Fase 5O: total entries one directory-cache slot can hold. Root only has
// ~13 today; sized generously for a somewhat larger directory. Extra
// entries beyond this are dropped (slot.truncated = true) rather than
// overflowing -- no malloc, fixed storage only.
#define SIDETNFS_DIR_CACHE_MAX_ENTRIES 64u

// Fase 5Q: number of directories that can be cached (and searched)
// simultaneously. Root-cause analysis of "root always truncated to 1
// entry" (see report) found that GEM/TOS issues more than one concurrent
// Fsfirst/Fsnext sequence around a single folder-window refresh (e.g. an
// icon-layout .INF lookup interleaved with the real listing) -- a single
// shared cache/search slot let one clobber the other. 4 slots comfortably
// covers root + a couple of subdirectory/auxiliary lookups at once.
#define SIDETNFS_DIR_CACHE_SLOTS 4u
#define SIDETNFS_SEARCH_SLOTS 4u

// Fase 5R (diagnostic): bounded wait for a single OPENDIRX/READDIRX
// round-trip in the direct (uncached) scan path. Worst case per round:
// SIDETNFS_FS_WAIT_MAX_ITER * SIDETNFS_FS_WAIT_STEP_US = 200ms. Only used
// when SIDETNFS_DIR_CACHE_ENABLED == 0.
#define SIDETNFS_FS_WAIT_MAX_ITER 200
#define SIDETNFS_FS_WAIT_STEP_US 1000

// Fase 5O/5P: hard wall-clock budget for GEMDRVEMUL_FSFIRST_CALL to wait
// for a cache-miss refresh to finish (see sidetnfs_dir_cache_wait_ready()).
// This is the ONLY bounded wait left anywhere in the TNFS-listing path --
// Fsnext never waits, and this budget covers the *whole* refresh (however
// many READDIRX rounds that takes), not a single round-trip like Fase 5N.
// Raised from 200ms to 500ms in Fase 5P: a directory needing several
// READDIRX rounds (e.g. root's ~13 entries at 8/round) should comfortably
// fit, but 200ms left little margin once real network jitter is added.
#define SIDETNFS_DIR_CACHE_FSFIRST_WAIT_MS 500u
#define SIDETNFS_DIR_CACHE_WAIT_STEP_US 1000

static const char SIDETNFS_PROBE_PAYLOAD[] = "SIDETNFS_PROBE";

// Fase 5F/5G/5I: small, fixed-size debug state. Filled in by the UDP
// callback (RAM only, never touches FatFS) and consumed by
// sidetnfs_debug_file_service(), which is the only place that ever writes
// to SD. No malloc, no dynamic strings.
typedef struct
{
    bool debug_dirty;
    uint32_t debug_write_count;

    bool network_skipped;

    bool mount_probe_sent;
    bool mount_response_received;
    uint16_t sid;
    uint8_t mount_rc;

    // "opendir" here means OPENDIRX (0x17) -- READDIRX needs a handle from
    // the extended variant, not from a basic OPENDIR (0x10).
    bool opendir_sent;
    bool opendir_response_received;
    uint8_t opendir_handle;
    uint8_t opendir_rc;

    bool readdirx_started;
    bool readdirx_waiting_response;
    bool readdirx_done;
    uint8_t readdirx_last_rc;
    uint16_t readdirx_count_dirs;
    uint16_t readdirx_count_files;
    uint16_t readdirx_count_total;
    uint16_t readdirx_rounds;

    // Fase 5K: how many raw READDIRX entries were successfully normalized
    // into Atari/GEMDOS 8.3 form vs. intentionally skipped (unsupported
    // name, or TNFS "special" flag).
    uint16_t translate_ok_count;
    uint16_t translate_skipped_count;

    // Fase 5L: how many normalized entries matched each pattern in
    // SIDETNFS_MATCH_PATTERNS (same index).
    uint16_t match_counts[SIDETNFS_MATCH_PATTERN_COUNT];

    // Fase 5M: how many normalized entries matched each GEMDOS-style
    // attribute test (see sidetnfs_gemdos_attr_match()).
    uint16_t attr_normal_count;
    uint16_t attr_folder_count;
    uint16_t attr_hidden_count;

    bool sd_scan_done;
    uint16_t sd_scan_count_dirs;
    uint16_t sd_scan_count_files;

    // Fase 5N (experimental): how many TNFS-backed Fsfirst/Fsnext calls
    // succeeded vs. hit a bounded-wait timeout/protocol error.
    uint16_t fs_listing_hits;
    uint16_t fs_listing_errors;

#if SIDETNFS_DEBUG_SHOW_RAW
    uint16_t last_response_len;
    uint8_t last_raw[SIDETNFS_DEBUG_RAW_SIZE];
#endif
} SidetnfsDebugState;

static SidetnfsDebugState s_state = {0};
static uint8_t s_readdirx_seq = 2; // MOUNT uses 0, OPENDIRX uses 1

// Fase 5O: single-slot RAM directory cache, entirely separate from the
// FatFS DTA hash table in gemdrvemul.c -- that table is never touched by
// the TNFS-listing path (see report for why: insertDTA()/lookupDTA()
// dereference dj/fno unconditionally, and a TNFS-backed ndta never has
// real FatFS DIR/FILINFO objects). SIDETNFS_DIR_CACHE_SLOTS directories can
// be cached simultaneously (Fase 5Q, see report) -- requesting a path not
// already held in a slot claims an empty slot, or evicts the
// least-recently-used slot not currently mid-network-build.
typedef enum
{
    SIDETNFS_CACHE_EMPTY = 0,
    SIDETNFS_CACHE_LOADING,
    SIDETNFS_CACHE_READY,
    SIDETNFS_CACHE_ERROR,
} SidetnfsCacheState;

typedef struct
{
    SidetnfsCacheState state;
    char path[MAX_FOLDER_LENGTH];
    SidetnfsAtariDirEntry entries[SIDETNFS_DIR_CACHE_MAX_ENTRIES];
    uint16_t count;
    uint16_t skipped;
    uint16_t dirs;
    uint16_t files;
    uint8_t error_rc;
    bool truncated;
    uint32_t last_used_seq; // for least-recently-used eviction
} SidetnfsDirCacheSlot;

static SidetnfsDirCacheSlot s_dir_cache_slots[SIDETNFS_DIR_CACHE_SLOTS] = {0};
static uint32_t s_dir_cache_use_counter = 0;

// Fase 5Q: the network channel handles ONE OPENDIRX/READDIRX conversation
// at a time, regardless of how many cache slots exist -- concurrent
// conversations sharing one TNFS session is what caused entries to be
// lost in earlier phases (see report). s_building_slot is the index into
// s_dir_cache_slots[] currently being (re)built over the network, or -1
// if the channel is idle. The build-in-progress fields below are only
// meaningful while s_building_slot >= 0.
static int s_building_slot = -1;
static bool s_building_opendirx_sent = false;
static bool s_building_opendirx_done = false;
static uint8_t s_building_opendirx_seq = 0;
static uint8_t s_building_dir_handle = 0;
static bool s_building_readdirx_pending = false;
static uint8_t s_building_readdirx_seq = 0;

// Fase 5Q: fixed-size search-slot table, used by
// GEMDRVEMUL_FSFIRST_CALL/FSNEXT_CALL, one slot per concurrently-active
// ndta (see report -- this is what actually fixed "root always truncated
// to 1 entry": GEM/TOS runs more than one Fsfirst/Fsnext sequence around a
// single folder refresh). "fake" marks a memory-only no-network search
// (see sidetnfs_fake_search_start()) that never touches a cache slot at
// all -- cache_search_advance() special-cases it inline.
typedef struct
{
    bool active;
    bool fake;
    uint32_t ndta;
    char path[MAX_FOLDER_LENGTH];
    char pattern[13];
    uint8_t attribs;
    uint16_t next_index;
} SidetnfsCacheSearchSlot;

static SidetnfsCacheSearchSlot s_cache_searches[SIDETNFS_SEARCH_SLOTS] = {0};

#if !SIDETNFS_DIR_CACHE_ENABLED
// Fase 5Y: TNFS DTA-registry entry -- one open dir_handle plus
// path/pattern/attribs/eof, NO entry cache of any kind. This is the direct
// TNFS-side analogue of a FatFS DTANode (see insertDTA()/lookupDTA() in
// gemdrvemul.c): keyed by ndta, inserted on Fsfirst, looked up on Fsnext.
// The SD-baseline hardware test (Fase 5X) proved real GEMDRVEMUL_FSNEXT_CALL
// dispatch works correctly once state is registered this way -- the earlier
// Fase 5W "live search" mechanism was functionally identical but not framed
// around ndta as an explicit registry key the way SD's DTA table is (see
// report). Up to SIDETNFS_TNFS_DTA_SLOTS concurrent searches (one per ndta).
typedef struct
{
    bool active;
    uint32_t ndta;
    char path[MAX_FOLDER_LENGTH];
    char pattern[13];
    uint8_t attribs;
    uint8_t dir_handle;
    bool eof;
    // Fase 5AA: true from a successful OPENDIRX until CLOSEDIR has been
    // sent for dir_handle (successfully or not) -- the single guard against
    // ever sending CLOSEDIR twice for the same handle. Always set/cleared
    // together with `active` (see insertTnfsDTA()/releaseTnfsDTA()).
    bool handle_valid;
} SidetnfsTnfsDtaSearch;

static SidetnfsTnfsDtaSearch s_tnfs_dta_searches[SIDETNFS_TNFS_DTA_SLOTS] = {0};
#endif // !SIDETNFS_DIR_CACHE_ENABLED

// Fase 5S: RAM-only diagnostic eventlog. Stops recording once full (see
// sidetnfs_diag_log() doc comment) -- never a ring buffer, so the earliest
// events (boot/cold-start/first Fsfirst) are never overwritten by later
// noise.
static SidetnfsDiagEvent s_diag_events[SIDETNFS_DIAG_MAX_EVENTS];
static uint16_t s_diag_event_count = 0;

void sidetnfs_diag_log(SidetnfsDiagEventType event, uint32_t ndta, const char *path,
                        const char *pattern, const char *name, uint16_t index,
                        uint16_t count, uint8_t result, uint8_t attr)
{
#if SIDETNFS_FS_DIAG_ENABLED
    if (s_diag_event_count >= SIDETNFS_DIAG_MAX_EVENTS)
    {
        return; // full -- stop recording, keep the earliest events
    }
    SidetnfsDiagEvent *e = &s_diag_events[s_diag_event_count];
    e->seq = s_diag_event_count;
    e->event = (uint16_t)event;
    e->ndta = ndta;
    e->index = index;
    e->count = count;
    e->result = result;
    e->attr = attr;
    e->path[0] = '\0';
    if (path != NULL)
    {
        strncpy(e->path, path, sizeof(e->path) - 1);
    }
    e->pattern[0] = '\0';
    if (pattern != NULL)
    {
        strncpy(e->pattern, pattern, sizeof(e->pattern) - 1);
    }
    e->name[0] = '\0';
    if (name != NULL)
    {
        strncpy(e->name, name, sizeof(e->name) - 1);
    }
    s_diag_event_count++;
#else
    (void)event;
    (void)ndta;
    (void)path;
    (void)pattern;
    (void)name;
    (void)index;
    (void)count;
    (void)result;
    (void)attr;
#endif
}

#if SIDETNFS_DEBUG_DUMP_ON_SELECT
static const char *diag_event_name(SidetnfsDiagEventType event)
{
    switch (event)
    {
    case SIDETNFS_DIAG_FSFIRST_ENTER:
        return "FSFIRST_ENTER";
    case SIDETNFS_DIAG_FSFIRST_ATTR_PREP:
        return "FSFIRST_ATTR_PREP";
    case SIDETNFS_DIAG_FSFIRST_CACHE_HIT:
        return "FSFIRST_CACHE_HIT";
    case SIDETNFS_DIAG_FSFIRST_CACHE_MISS:
        return "FSFIRST_CACHE_MISS";
    case SIDETNFS_DIAG_FSFIRST_CACHE_READY:
        return "FSFIRST_CACHE_READY";
    case SIDETNFS_DIAG_FSFIRST_CACHE_ERROR:
        return "FSFIRST_CACHE_ERROR";
    case SIDETNFS_DIAG_FSFIRST_FOUND:
        return "FSFIRST_FOUND";
    case SIDETNFS_DIAG_FSFIRST_NOT_FOUND:
        return "FSFIRST_NOT_FOUND";
    case SIDETNFS_DIAG_FSFIRST_RETURN:
        return "FSFIRST_RETURN";
    case SIDETNFS_DIAG_FSNEXT_ENTER:
        return "FSNEXT_ENTER";
    case SIDETNFS_DIAG_FSNEXT_SEARCH_ACTIVE:
        return "FSNEXT_SEARCH_ACTIVE";
    case SIDETNFS_DIAG_FSNEXT_SEARCH_MISSING:
        return "FSNEXT_SEARCH_MISSING";
    case SIDETNFS_DIAG_FSNEXT_FOUND:
        return "FSNEXT_FOUND";
    case SIDETNFS_DIAG_FSNEXT_END:
        return "FSNEXT_END";
    case SIDETNFS_DIAG_FSNEXT_RETURN:
        return "FSNEXT_RETURN";
    case SIDETNFS_DIAG_SEARCH_OVERWRITE:
        return "SEARCH_OVERWRITE";
    case SIDETNFS_DIAG_CACHE_REPLACE:
        return "CACHE_REPLACE";
    case SIDETNFS_DIAG_CACHE_BUILD_START:
        return "CACHE_BUILD_START";
    case SIDETNFS_DIAG_CACHE_OPENDIRX_OK:
        return "CACHE_OPENDIRX_OK";
    case SIDETNFS_DIAG_CACHE_READDIRX_BATCH:
        return "CACHE_READDIRX_BATCH";
    case SIDETNFS_DIAG_CACHE_BUILD_READY:
        return "CACHE_BUILD_READY";
    case SIDETNFS_DIAG_CACHE_BUILD_ERROR:
        return "CACHE_BUILD_ERROR";
    case SIDETNFS_DIAG_CACHE_BUILD_TIMEOUT:
        return "CACHE_BUILD_TIMEOUT";
    case SIDETNFS_DIAG_FAKE_SEARCH_START:
        return "FAKE_SEARCH_START";
    case SIDETNFS_DIAG_FAKE_FOUND:
        return "FAKE_FOUND";
    case SIDETNFS_DIAG_FAKE_NOT_FOUND:
        return "FAKE_NOT_FOUND";
    case SIDETNFS_DIAG_FSFIRST_REPEAT_CONTINUE:
        return "FSFIRST_REPEAT_CONTINUE";
    case SIDETNFS_DIAG_FSFIRST_REPEAT_RESTART:
        return "FSFIRST_REPEAT_RESTART";
    case SIDETNFS_DIAG_FSNEXT_CASE_REACHED:
        return "FSNEXT_CASE_REACHED";
    case SIDETNFS_DIAG_COMMAND_ENTER:
        return "COMMAND_ENTER";
    case SIDETNFS_DIAG_TNFS_OPENDIRX:
        return "TNFS_OPENDIRX";
    case SIDETNFS_DIAG_TNFS_OPENDIRX_OK:
        return "TNFS_OPENDIRX_OK";
    case SIDETNFS_DIAG_TNFS_OPENDIRX_ERROR:
        return "TNFS_OPENDIRX_ERROR";
    case SIDETNFS_DIAG_TNFS_READDIRX_ONE:
        return "TNFS_READDIRX_ONE";
    case SIDETNFS_DIAG_TNFS_READDIRX_ENTRY:
        return "TNFS_READDIRX_ENTRY";
    case SIDETNFS_DIAG_TNFS_READDIRX_SKIP:
        return "TNFS_READDIRX_SKIP";
    case SIDETNFS_DIAG_TNFS_READDIRX_MATCH:
        return "TNFS_READDIRX_MATCH";
    case SIDETNFS_DIAG_TNFS_READDIRX_EOF:
        return "TNFS_READDIRX_EOF";
    case SIDETNFS_DIAG_DTA_INSERT:
        return "DTA_INSERT";
    case SIDETNFS_DIAG_DTA_LOOKUP_OK:
        return "DTA_LOOKUP_OK";
    case SIDETNFS_DIAG_DTA_LOOKUP_FAIL:
        return "DTA_LOOKUP_FAIL";
    case SIDETNFS_DIAG_SD_FIND_FIRST:
        return "SD_FIND_FIRST";
    case SIDETNFS_DIAG_SD_FIND_NEXT:
        return "SD_FIND_NEXT";
    case SIDETNFS_DIAG_SD_DTA_INSERT:
        return "SD_DTA_INSERT";
    case SIDETNFS_DIAG_SD_DTA_LOOKUP_OK:
        return "SD_DTA_LOOKUP_OK";
    case SIDETNFS_DIAG_SD_DTA_LOOKUP_FAIL:
        return "SD_DTA_LOOKUP_FAIL";
    case SIDETNFS_DIAG_TNFS_DTA_INSERT:
        return "TNFS_DTA_INSERT";
    case SIDETNFS_DIAG_TNFS_DTA_LOOKUP_OK:
        return "TNFS_DTA_LOOKUP_OK";
    case SIDETNFS_DIAG_TNFS_DTA_LOOKUP_FAIL:
        return "TNFS_DTA_LOOKUP_FAIL";
    case SIDETNFS_DIAG_TNFS_DTA_RELEASE:
        return "TNFS_DTA_RELEASE";
    case SIDETNFS_DIAG_DTA_EXIST_ENTER:
        return "DTA_EXIST_ENTER";
    case SIDETNFS_DIAG_DTA_EXIST_FATFS_OK:
        return "DTA_EXIST_FATFS_OK";
    case SIDETNFS_DIAG_DTA_EXIST_TNFS_OK:
        return "DTA_EXIST_TNFS_OK";
    case SIDETNFS_DIAG_DTA_EXIST_FAKE_OK:
        return "DTA_EXIST_FAKE_OK";
    case SIDETNFS_DIAG_DTA_EXIST_FAIL:
        return "DTA_EXIST_FAIL";
    case SIDETNFS_DIAG_DTA_EXIST_RETURN:
        return "DTA_EXIST_RETURN";
    case SIDETNFS_DIAG_DTA_RELEASE_ENTER:
        return "DTA_RELEASE_ENTER";
    case SIDETNFS_DIAG_DTA_RELEASE_FATFS:
        return "DTA_RELEASE_FATFS";
    case SIDETNFS_DIAG_DTA_RELEASE_TNFS:
        return "DTA_RELEASE_TNFS";
    case SIDETNFS_DIAG_DTA_RELEASE_FAKE:
        return "DTA_RELEASE_FAKE";
    case SIDETNFS_DIAG_DTA_RELEASE_RETURN:
        return "DTA_RELEASE_RETURN";
    case SIDETNFS_DIAG_TNFS_CLOSEDIR:
        return "TNFS_CLOSEDIR";
    case SIDETNFS_DIAG_TNFS_CLOSEDIR_OK:
        return "TNFS_CLOSEDIR_OK";
    case SIDETNFS_DIAG_TNFS_CLOSEDIR_ERROR:
        return "TNFS_CLOSEDIR_ERROR";
    case SIDETNFS_DIAG_TNFS_CLOSEDIR_TIMEOUT:
        return "TNFS_CLOSEDIR_TIMEOUT";
    case SIDETNFS_DIAG_TNFS_HANDLE_RELEASE:
        return "TNFS_HANDLE_RELEASE";
    default:
        return "UNKNOWN";
    }
}

// Fase 5Z: decode a raw GEMDRVEMUL_*_CALL id (as logged in a COMMAND_ENTER
// event's ndta field, see gemdrvemul.c) into a short name, so DEBUG.TXT
// reads as "cmdname=DTA_EXIST" instead of a bare hex id. Only covers the
// handful of commands relevant to the Fsfirst/Fsnext/DTA_EXIST/DTA_RELEASE
// investigation -- not every GEMDRVEMUL_*_CALL in commands.h.
static const char *command_id_name(uint32_t id)
{
    switch (id)
    {
    case GEMDRVEMUL_FSETDTA_CALL:
        return "FSETDTA";
    case GEMDRVEMUL_DTA_EXIST_CALL:
        return "DTA_EXIST";
    case GEMDRVEMUL_DTA_RELEASE_CALL:
        return "DTA_RELEASE";
    case GEMDRVEMUL_FSFIRST_CALL:
        return "FSFIRST";
    case GEMDRVEMUL_FSNEXT_CALL:
        return "FSNEXT";
    case GEMDRVEMUL_FOPEN_CALL:
        return "FOPEN";
    case GEMDRVEMUL_FCLOSE_CALL:
        return "FCLOSE";
    case GEMDRVEMUL_FCREATE_CALL:
        return "FCREATE";
    case GEMDRVEMUL_FDELETE_CALL:
        return "FDELETE";
    case GEMDRVEMUL_FSEEK_CALL:
        return "FSEEK";
    case GEMDRVEMUL_FATTRIB_CALL:
        return "FATTRIB";
    case GEMDRVEMUL_FRENAME_CALL:
        return "FRENAME";
    case GEMDRVEMUL_FDATETIME_CALL:
        return "FDATETIME";
    case GEMDRVEMUL_DGETPATH_CALL:
        return "DGETPATH";
    case GEMDRVEMUL_DSETPATH_CALL:
        return "DSETPATH";
    case GEMDRVEMUL_DCREATE_CALL:
        return "DCREATE";
    case GEMDRVEMUL_DDELETE_CALL:
        return "DDELETE";
    case GEMDRVEMUL_DFREE_CALL:
        return "DFREE";
    case GEMDRVEMUL_DGETDRV_CALL:
        return "DGETDRV";
    default:
        return "";
    }
}
#endif // SIDETNFS_DEBUG_DUMP_ON_SELECT

void sidetnfs_diag_dump_on_select(const char *hd_folder)
{
#if SIDETNFS_DEBUG_DUMP_ON_SELECT
    if (hd_folder == NULL)
    {
        return;
    }
    char path[160];
    int n = snprintf(path, sizeof(path), "%s/DEBUG.TXT", hd_folder);
    if (n <= 0 || (size_t)n >= sizeof(path))
    {
        return;
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        return; // stay silent, no crash
    }

    // Fase 5Z/5AA: 256, not 128 -- the per-event line format below can
    // combine a long event name with a full path/pattern/name and the
    // cmdname suffix, and the per-slot TNFS DTA dump line (Fase 5AA) can
    // combine a full MAX_FOLDER_LENGTH path with the new handle_valid
    // field -- neither reliably fits in 128 or even 224 bytes.
    char line[256];
    UINT written;
    int len = snprintf(line, sizeof(line),
                        "SIDETNFS FS DIAG\r\n"
                        "mode: %s\r\n"
                        "experimental listing: %u\r\n"
                        "events: %u\r\n",
#if SIDETNFS_EXPERIMENTAL_FS_LISTING
                        "TNFS_LIVE_DTA",
#else
                        "SD_BASELINE",
#endif
                        (unsigned)SIDETNFS_EXPERIMENTAL_FS_LISTING, (unsigned)s_diag_event_count);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    // Fase 5W: the single most important diagnostic question -- did a real
    // GEMDRVEMUL_FSNEXT_CALL ever get dispatched this boot, or did GEM/TOS
    // only ever repeat Fsfirst? Computed as a summary so it's visible
    // without having to scan the whole event list by hand.
    bool fsnext_ever_reached = false;
    for (uint16_t i = 0; i < s_diag_event_count; i++)
    {
        if (s_diag_events[i].event == (uint16_t)SIDETNFS_DIAG_FSNEXT_ENTER)
        {
            fsnext_ever_reached = true;
            break;
        }
    }
    len = snprintf(line, sizeof(line),
                    "fsnext case reached: %s\r\n"
                    "tnfs dta slots: %u\r\n"
                    "cache enabled: %s\r\n"
                    "readdirx max entries: %u\r\n"
                    "repeat fsfirst continue: %s\r\n\r\n",
                    fsnext_ever_reached ? "YES" : "NO",
#if SIDETNFS_DIR_CACHE_ENABLED
                    0u,
#else
                    (unsigned)SIDETNFS_TNFS_DTA_SLOTS,
#endif
                    SIDETNFS_DIR_CACHE_ENABLED ? "YES" : "NO", (unsigned)SIDETNFS_READDIRX_MAX_ENTRIES,
                    SIDETNFS_FSFIRST_REPEAT_CONTINUE ? "YES" : "NO");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    // Fase 5AA: how many TNFS DTA-registry slots are active vs. how many
    // still have an unclosed OPENDIRX handle -- if handles-open ever grows
    // without bound across repeated Fsfirst/refresh cycles, that's a
    // leak; it should track active closely (0 or 1 higher, briefly,
    // between OPENDIRX success and the next release).
#if !SIDETNFS_DIR_CACHE_ENABLED
    {
        unsigned tnfs_active = 0;
        unsigned tnfs_handles_open = 0;
        for (unsigned i = 0; i < SIDETNFS_TNFS_DTA_SLOTS; i++)
        {
            if (s_tnfs_dta_searches[i].active)
            {
                tnfs_active++;
            }
            if (s_tnfs_dta_searches[i].handle_valid)
            {
                tnfs_handles_open++;
            }
        }
        len = snprintf(line, sizeof(line),
                        "tnfs dta active: %u\r\n"
                        "tnfs handles open: %u\r\n"
                        "closedir enabled: %s\r\n\r\n",
                        tnfs_active, tnfs_handles_open, SIDETNFS_TNFS_CLOSEDIR_ENABLED ? "YES" : "NO");
        if (len > 0)
        {
            f_write(&file, line, (UINT)len, &written);
        }
    }
#endif

#if SIDETNFS_DIR_CACHE_ENABLED
    for (unsigned i = 0; i < SIDETNFS_DIR_CACHE_SLOTS; i++)
    {
        if (s_dir_cache_slots[i].state == SIDETNFS_CACHE_EMPTY)
        {
            continue;
        }
        const char *state_str = s_dir_cache_slots[i].state == SIDETNFS_CACHE_READY     ? "READY"
                                 : s_dir_cache_slots[i].state == SIDETNFS_CACHE_LOADING ? "LOADING"
                                                                                        : "ERROR";
        len = snprintf(line, sizeof(line), "cache[%u] path=%s state=%s count=%u dirs=%u files=%u skipped=%u\r\n",
                       i, s_dir_cache_slots[i].path, state_str, (unsigned)s_dir_cache_slots[i].count,
                       (unsigned)s_dir_cache_slots[i].dirs, (unsigned)s_dir_cache_slots[i].files,
                       (unsigned)s_dir_cache_slots[i].skipped);
        if (len > 0)
        {
            f_write(&file, line, (UINT)len, &written);
        }
    }
    unsigned active_searches = 0;
    for (unsigned i = 0; i < SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (s_cache_searches[i].active)
        {
            active_searches++;
        }
    }
    len = snprintf(line, sizeof(line), "active cache searches: %u\r\n\r\n", active_searches);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
#else
    for (unsigned i = 0; i < SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (!s_tnfs_dta_searches[i].active)
        {
            continue;
        }
        len = snprintf(line, sizeof(line),
                        "tnfsdta[%u] ndta=%08lx path=%s pat=%s attr=%02x handle=%u handle_valid=%s eof=%s\r\n", i,
                        (unsigned long)s_tnfs_dta_searches[i].ndta, s_tnfs_dta_searches[i].path,
                        s_tnfs_dta_searches[i].pattern, (unsigned)s_tnfs_dta_searches[i].attribs,
                        (unsigned)s_tnfs_dta_searches[i].dir_handle,
                        s_tnfs_dta_searches[i].handle_valid ? "YES" : "NO",
                        s_tnfs_dta_searches[i].eof ? "YES" : "NO");
        if (len > 0)
        {
            // Defensive clamp -- see the per-event dump loop below for why.
            if ((size_t)len >= sizeof(line))
            {
                len = (int)sizeof(line) - 1;
            }
            f_write(&file, line, (UINT)len, &written);
        }
    }
    len = snprintf(line, sizeof(line), "\r\n");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
#endif

    for (uint16_t i = 0; i < s_diag_event_count; i++)
    {
        const SidetnfsDiagEvent *e = &s_diag_events[i];
        // Fase 5Z: for COMMAND_ENTER events, e->ndta actually holds the raw
        // command_id (see gemdrvemul.c) -- decode it so DEBUG.TXT reads
        // "cmdname=DTA_EXIST" instead of a bare hex id.
        const char *cmd_name =
            ((SidetnfsDiagEventType)e->event == SIDETNFS_DIAG_COMMAND_ENTER) ? command_id_name(e->ndta) : "";
        len = snprintf(line, sizeof(line),
                        "%03u %s ndta=%08lx path=%s pat=%s name=%s idx=%u count=%u rc=%u attr=%02x cmdname=%s\r\n",
                        (unsigned)e->seq, diag_event_name((SidetnfsDiagEventType)e->event), (unsigned long)e->ndta,
                        e->path, e->pattern, e->name, (unsigned)e->index, (unsigned)e->count, (unsigned)e->result,
                        (unsigned)e->attr, cmd_name);
        if (len > 0)
        {
            // Defensive clamp: snprintf() reports the length it *would*
            // have written even when truncated -- never let f_write() read
            // past the actual buffer.
            if ((size_t)len >= sizeof(line))
            {
                len = (int)sizeof(line) - 1;
            }
            f_write(&file, line, (UINT)len, &written);
        }
    }

    f_close(&file);
#else
    (void)hd_folder;
#endif
}

// Fase 5N: a dedicated UDP PCB + receive state for the experimental
// Fsfirst/Fsnext path, entirely separate from s_mount_pcb/tnfs_recv_callback
// above. Reusing the probe's own PCB/callback would corrupt its root-scan
// state (same OPENDIRX/READDIRX opcodes, routed purely by opcode with no
// request/response correlation) whenever Fsfirst/Fsnext opens an arbitrary
// (non-root) path -- see report.
//
// Fase 5P: sequence numbers for this channel now reuse the shared
// s_readdirx_seq counter (see report: two independent sequence counters
// sharing one TNFS session was one of the suspected causes of the "root
// truncated to 1 entry" bug) instead of a separate counter.
static struct udp_pcb *s_fslisting_pcb = NULL;

typedef struct
{
    volatile bool response_ready;
    uint8_t cmd;
    uint8_t seq; // Fase 5R: echoed sequence number, for cmd+seq correlation
    uint8_t buf[SIDETNFS_RX_BUF_SIZE];
    uint16_t len;
} SidetnfsFsListingResponse;

static SidetnfsFsListingResponse s_fslisting_resp = {0};

// The MOUNT PCB is intentionally never removed once created: the udp_recv()
// callback must still be able to fire whenever the server's reply happens
// to arrive (for MOUNT, OPENDIRX and READDIRX, all sent over the same PCB),
// and this probe is a one-shot, once-per-boot action, so leaking a single
// PCB for the remaining lifetime of the firmware is the smallest safe
// choice (no risk of removing it out from under an in-flight callback).
static struct udp_pcb *s_mount_pcb = NULL;

bool sidetnfs_udp_connect_test(void)
{
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return false;
    }

    cyw43_arch_lwip_begin();

    struct udp_pcb *pcb = udp_new();
    if (!pcb)
    {
        cyw43_arch_lwip_end();
        return false;
    }

    bool connected = (udp_connect(pcb, &server_ip, SIDETNFS_SERVER_PORT) == ERR_OK);

    udp_remove(pcb);
    cyw43_arch_lwip_end();

    return connected;
}

void sidetnfs_send_udp_probe(void)
{
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    struct udp_pcb *pcb = udp_new();
    if (!pcb)
    {
        cyw43_arch_lwip_end();
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(SIDETNFS_PROBE_PAYLOAD) - 1, PBUF_RAM);
    if (!p)
    {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, SIDETNFS_PROBE_PAYLOAD, sizeof(SIDETNFS_PROBE_PAYLOAD) - 1);
    udp_sendto(pcb, p, &server_ip, SIDETNFS_SERVER_PORT);

    pbuf_free(p);
    udp_remove(pcb);

    cyw43_arch_lwip_end();
}

// Fase 5K: strict, uppercase-only 8.3 name check -- see sidetnfs_probe.h.
bool sidetnfs_is_supported_83_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
    {
        return false;
    }
    if (name[0] == '.')
    {
        // Rejects ".", "..", AppleDouble "._*" and Linux/macOS dotfiles
        // (e.g. ".DS_Store") in one go.
        return false;
    }

    size_t len = strlen(name);
    size_t dot_count = 0;
    size_t dot_pos = 0;
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        if (c == '.')
        {
            dot_count++;
            dot_pos = i;
            continue;
        }
        if ((unsigned char)c < 32)
        {
            return false;
        }
        if (c == ' ' || c == '/' || c == '\\' ||
            c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*')
        {
            return false; // FAT/GEMDOS-invalid characters
        }
        if (c >= 'a' && c <= 'z')
        {
            return false; // lowercase unsupported for now -- see report
        }
    }
    if (dot_count > 1)
    {
        return false; // only one "NAME.EXT"-style dot allowed
    }

    size_t base_len = (dot_count == 1) ? dot_pos : len;
    size_t ext_len = (dot_count == 1) ? (len - dot_pos - 1) : 0;

    if (base_len == 0 || base_len > 8)
    {
        return false;
    }
    if (dot_count == 1 && (ext_len == 0 || ext_len > 3))
    {
        return false; // reject trailing-dot ("NAME.") and overlong extensions
    }

    return true;
}

// Fase 5V: small, safe Unix-epoch -> DOS/GEMDOS packed date/time
// conversion. Reuses gmtime() (already used elsewhere in this codebase,
// e.g. rtcemul.c) instead of hand-rolling calendar math. DOS dates cannot
// represent anything before 1980, and a NULL/implausible gmtime() result
// (e.g. tnfs_mtime == 0) falls back to a fixed placeholder -- but that
// placeholder is 1980-01-01 (a genuinely VALID calendar date), not the old
// Fase 5K placeholder of raw 0/0 (which decodes to month=0/day=0, an
// INVALID DOS date -- suspected of causing GEM/TOS to treat every TNFS
// entry as corrupt, see report).
static void unix_epoch_to_dos_datetime(uint32_t unix_time, uint16_t *out_date, uint16_t *out_time)
{
    time_t t = (time_t)unix_time;
    struct tm *utc = (unix_time != 0) ? gmtime(&t) : NULL;
    if (utc == NULL || (utc->tm_year + 1900) < 1980)
    {
        *out_date = (1 << 5) | 1; // 1980-01-01: year=0, month=1, day=1
        *out_time = 0;
        return;
    }
    uint16_t year = (uint16_t)(utc->tm_year + 1900 - 1980);
    if (year > 127)
    {
        year = 127; // DOS date year field is 7 bits (1980-2107)
    }
    *out_date = (uint16_t)((year << 9) | ((uint16_t)(utc->tm_mon + 1) << 5) | (uint16_t)utc->tm_mday);
    *out_time = (uint16_t)(((uint16_t)utc->tm_hour << 11) | ((uint16_t)utc->tm_min << 5) | ((uint16_t)utc->tm_sec / 2));
}

// Fase 5K: convert one TNFS entry to Atari/GEMDOS form -- see sidetnfs_probe.h.
bool sidetnfs_normalize_dir_entry(const char *tnfs_name, uint8_t tnfs_flags,
                                   uint32_t tnfs_size, uint32_t tnfs_mtime,
                                   SidetnfsAtariDirEntry *out)
{
    memset(out, 0, sizeof(*out));

    if (tnfs_flags & TNFS_DIRENTRY_SPECIAL)
    {
        out->skipped = true;
        return false;
    }
    if (!sidetnfs_is_supported_83_name(tnfs_name))
    {
        out->skipped = true;
        return false;
    }

    strncpy(out->name, tnfs_name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';

    bool is_dir = (tnfs_flags & TNFS_DIRENTRY_DIR) != 0;
    out->attr = 0;
    if (is_dir)
    {
        out->attr |= FS_ST_FOLDER;
    }
    if (tnfs_flags & TNFS_DIRENTRY_HIDDEN)
    {
        out->attr |= FS_ST_HIDDEN;
    }
    // Read-only/system bits are not available from TNFS READDIRX -> left
    // off. Archive bit also left off (documented choice, see report).

    out->size = is_dir ? 0 : tnfs_size;

    // Fase 5V: real conversion (see unix_epoch_to_dos_datetime() doc
    // comment above) -- replaces the Fase 5K fixed 0/0 placeholder.
    unix_epoch_to_dos_datetime(tnfs_mtime, &out->date, &out->time);

    out->valid = true;
    return true;
}

// Fase 5L: strip a trailing ".*" from a pattern, mirroring the existing
// GEMDOS-adjustment already done for the SD/FatFS backend in
// seach_path_2_st() (gemdrvemul.c) -- see the comment there: "Patterns do
// not work with FatFs as Atari ST expects, so we need to adjust them."
// Bounded to a small local buffer since patterns here are always short
// 8.3-style strings; no malloc.
static void normalize_gemdos_pattern(const char *pattern, char *out, size_t out_size)
{
    size_t len = strnlen(pattern, out_size - 1);
    memcpy(out, pattern, len);
    out[len] = '\0';
    if (len >= 2 && out[len - 1] == '*' && out[len - 2] == '.')
    {
        out[len - 2] = '\0';
    }
}

// Classic greedy '*'/'?' glob match, case-insensitive. Iterative (no
// recursion, unlike FatFS's own static pattern_match()) since these
// patterns are always short and single-term.
static bool wildcard_match_upper(const char *pat, const char *str)
{
    const char *s = str;
    const char *p = pat;
    const char *star_p = NULL;
    const char *star_s = NULL;

    while (*s != '\0')
    {
        if (*p == '?' || toupper((unsigned char)*p) == toupper((unsigned char)*s))
        {
            p++;
            s++;
        }
        else if (*p == '*')
        {
            star_p = p++;
            star_s = s;
        }
        else if (star_p != NULL)
        {
            p = star_p + 1;
            s = ++star_s;
        }
        else
        {
            return false;
        }
    }
    while (*p == '*')
    {
        p++;
    }
    return *p == '\0';
}

// Fase 5L: GEMDOS-style pattern match against a normalized 8.3 name -- see
// sidetnfs_probe.h.
bool sidetnfs_gemdos_pattern_match(const char *name83, const char *pattern)
{
    if (name83 == NULL || pattern == NULL)
    {
        return false;
    }
    char norm_pattern[13];
    normalize_gemdos_pattern(pattern, norm_pattern, sizeof(norm_pattern));
    return wildcard_match_upper(norm_pattern, name83);
}

// Fase 5M: GEMDOS-style attribute match against a normalized entry -- see
// sidetnfs_probe.h.
bool sidetnfs_gemdos_attr_match(uint8_t entry_attr, uint8_t search_attr)
{
    uint8_t effective_attr = entry_attr;
    if (entry_attr == 0)
    {
        // TNFS has no real archive bit; a plain, non-folder, non-hidden
        // entry stands in for FS_ST_ARCH so normal-file searches still
        // find it (see the header comment for the rationale).
        effective_attr |= FS_ST_ARCH;
    }
    return (effective_attr & search_attr) != 0;
}

// Parse one READDIRX response's entries (flags(1)+size(4)+mtime(4)+ctime(4)
// +name(null-term) each), counting dirs vs files. Only touches RAM. Mirrors
// the byte layout of a previously hardware-tested standalone implementation.
static void parse_readdirx_entries(const uint8_t *buf, uint16_t n, uint8_t batch)
{
    uint16_t needle = 9;
    for (uint8_t i = 0; i < batch; i++)
    {
        if ((uint32_t)needle + 13 >= n)
        {
            break;
        }
        uint8_t flags = buf[needle];
        uint32_t size = (uint32_t)buf[needle + 1] | ((uint32_t)buf[needle + 2] << 8) |
                        ((uint32_t)buf[needle + 3] << 16) | ((uint32_t)buf[needle + 4] << 24);
        const char *name = (const char *)&buf[needle + 13];
        uint16_t avail = (uint16_t)(n - (needle + 13));
        size_t nlen = strnlen(name, avail);
        if (nlen >= avail)
        {
            break; // name not null-terminated within what we captured
        }
        needle = (uint16_t)(needle + 14 + nlen);

        // Fase 5K: translate every raw entry independently of the raw
        // dir/file counters' own skip rules below, so the ok/skipped
        // counts reflect sidetnfs_normalize_dir_entry()'s own policy.
        SidetnfsAtariDirEntry atari_entry;
        if (sidetnfs_normalize_dir_entry(name, flags, size, 0, &atari_entry))
        {
            s_state.translate_ok_count++;
            for (uint8_t p = 0; p < SIDETNFS_MATCH_PATTERN_COUNT; p++)
            {
                if (sidetnfs_gemdos_pattern_match(atari_entry.name, SIDETNFS_MATCH_PATTERNS[p]))
                {
                    s_state.match_counts[p]++;
                }
            }
            if (sidetnfs_gemdos_attr_match(atari_entry.attr, FS_ST_ARCH))
            {
                s_state.attr_normal_count++;
            }
            if (sidetnfs_gemdos_attr_match(atari_entry.attr, FS_ST_FOLDER))
            {
                s_state.attr_folder_count++;
            }
            if (sidetnfs_gemdos_attr_match(atari_entry.attr, FS_ST_HIDDEN))
            {
                s_state.attr_hidden_count++;
            }
        }
        else if (atari_entry.skipped)
        {
            s_state.translate_skipped_count++;
        }

        if (flags & (TNFS_DIRENTRY_HIDDEN | TNFS_DIRENTRY_SPECIAL))
        {
            continue;
        }
        if (nlen == 0 || (name[0] == '.' && (nlen == 1 || (name[1] == '.' && nlen == 2))))
        {
            continue; // skip empty / "." / ".." entries
        }

        if (flags & TNFS_DIRENTRY_DIR)
        {
            s_state.readdirx_count_dirs++;
        }
        else
        {
            s_state.readdirx_count_files++;
        }
        s_state.readdirx_count_total++;
    }
}

// lwIP receive callback, shared by MOUNT, OPENDIRX and READDIRX responses
// (all use the same PCB). Only touches RAM state -- no FatFS, no printf, no
// blocking. Always frees the pbuf. Routes on the echoed command byte
// (offset 3) since all request types share this one callback.
static void tnfs_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;
    if (!p)
    {
        return;
    }

    uint8_t buf[SIDETNFS_RX_BUF_SIZE];
    uint16_t n = p->tot_len < sizeof(buf) ? (uint16_t)p->tot_len : (uint16_t)sizeof(buf);
    pbuf_copy_partial(p, buf, n, 0);

#if SIDETNFS_DEBUG_SHOW_RAW
    uint16_t raw_n = n < SIDETNFS_DEBUG_RAW_SIZE ? n : SIDETNFS_DEBUG_RAW_SIZE;
    s_state.last_response_len = p->tot_len;
    memcpy(s_state.last_raw, buf, raw_n);
    if (raw_n < sizeof(s_state.last_raw))
    {
        memset(&s_state.last_raw[raw_n], 0, sizeof(s_state.last_raw) - raw_n);
    }
#endif

    if (n < 4)
    {
        pbuf_free(p);
        return;
    }

    uint8_t cmd = buf[3];
    if (cmd == TNFS_CMD_MOUNT)
    {
        s_state.sid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        s_state.mount_rc = n > 4 ? buf[4] : 0;
        s_state.mount_response_received = true;
        s_state.debug_dirty = true;
    }
    else if (cmd == TNFS_CMD_OPENDIRX)
    {
        s_state.opendir_rc = n > 4 ? buf[4] : 0;
        s_state.opendir_handle = n > 5 ? buf[5] : 0;
        s_state.opendir_response_received = true;
        s_state.debug_dirty = true;
    }
    else if (cmd == TNFS_CMD_READDIRX)
    {
        s_state.readdirx_waiting_response = false;
        uint8_t rc = n > 4 ? buf[4] : 0xFF;
        s_state.readdirx_last_rc = rc;
        s_state.debug_dirty = true;

        if (rc != TNFS_OK && rc != TNFS_EOF)
        {
            s_state.readdirx_done = true;
        }
        else
        {
            uint8_t batch = n > 5 ? buf[5] : 0;
            if (n > 8 && batch > 0)
            {
                parse_readdirx_entries(buf, n, batch);
            }
            if (batch == 0 || rc == TNFS_EOF || s_state.readdirx_rounds >= SIDETNFS_READDIRX_MAX_ROUNDS)
            {
                s_state.readdirx_done = true;
            }
        }
    }
    // Unknown/unexpected command: ignore silently.

    pbuf_free(p);
}

// Fase 5C/5F: send a single TNFS MOUNT request and register a receive
// callback for the (optional, asynchronous) reply. Fire-and-forget from the
// caller's point of view -- this function never waits, never retries, never
// logs, and always returns immediately regardless of whether a reply ever
// arrives. Must only be called after WiFi is confirmed connected.
void sidetnfs_send_mount_probe(void)
{
    // Mark as attempted regardless of what happens below -- this is a
    // fire-and-forget probe, "sent" means "we tried this boot".
    s_state.mount_probe_sent = true;
    s_state.debug_dirty = true;

    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    struct udp_pcb *pcb = udp_new();
    if (!pcb)
    {
        cyw43_arch_lwip_end();
        return;
    }

    // Register the reply callback before sending, so a fast reply can never
    // race ahead of us registering it.
    udp_recv(pcb, tnfs_recv_callback, NULL);

    // Header: session=0x0000 (new session), seq=0x00, cmd=TNFS_CMD_MOUNT.
    // Payload: proto version (minor, major) + null-terminated mount path
    // "/Atari.ST" + empty user + empty password.
    uint8_t buf[6 + 1 + sizeof(SIDETNFS_MOUNT_NAME) + 2];
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00; // seq 0 for MOUNT
    buf[3] = TNFS_CMD_MOUNT;
    buf[4] = TNFS_PROTO_VER_MINOR;
    buf[5] = TNFS_PROTO_VER_MAJOR;
    buf[6] = '/';
    memcpy(&buf[7], SIDETNFS_MOUNT_NAME, sizeof(SIDETNFS_MOUNT_NAME)); // includes '\0'
    size_t offset = 7 + sizeof(SIDETNFS_MOUNT_NAME);
    buf[offset++] = '\0'; // user: anonymous
    buf[offset++] = '\0'; // password: none

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!p)
    {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, buf, offset);
    udp_sendto(pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);

    // Keep the PCB alive (see s_mount_pcb comment above) so the callback
    // registered above can still fire for a reply that arrives later.
    s_mount_pcb = pcb;

    cyw43_arch_lwip_end();
}

// Fase 5I: send a single OPENDIRX "/" request over the existing MOUNT PCB,
// using the session id learned from the MOUNT response. Fire-and-forget,
// same non-blocking guarantees as sidetnfs_send_mount_probe().
static void send_opendirx_probe(void)
{
    s_state.opendir_sent = true;
    s_state.debug_dirty = true;

    if (!s_mount_pcb)
    {
        return;
    }

    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    // Header: session=<from MOUNT response>, seq=0x01, cmd=TNFS_CMD_OPENDIRX.
    // Payload: diropts(1)=0 + sortopts(1)=0 + max_count(2 LE)=0 (server
    // returns total count) + pattern "*" (null-term) + path "/" (null-term).
    // MOUNT already scoped the session to "/Atari.ST", so "/" is its root.
    uint8_t buf[12];
    buf[0] = (uint8_t)(s_state.sid & 0xFF);
    buf[1] = (uint8_t)(s_state.sid >> 8);
    buf[2] = 0x01; // seq 1 for OPENDIRX
    buf[3] = TNFS_CMD_OPENDIRX;
    buf[4] = 0x00; // diropts
    buf[5] = 0x00; // sortopts
    buf[6] = 0x00; // max_count lo
    buf[7] = 0x00; // max_count hi
    buf[8] = '*';
    buf[9] = '\0';
    buf[10] = '/';
    buf[11] = '\0';

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_mount_pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);

    cyw43_arch_lwip_end();
}

// Fase 5I: send one READDIRX request (one batch of up to
// SIDETNFS_READDIRX_BATCH_SIZE entries) over the existing PCB, using the
// handle learned from the OPENDIRX response. Fire-and-forget, same
// non-blocking guarantees as the other send_* helpers.
static void send_readdirx_probe(void)
{
    s_state.readdirx_started = true;
    s_state.readdirx_waiting_response = true;
    s_state.readdirx_rounds++;
    s_state.debug_dirty = true;

    if (!s_mount_pcb)
    {
        return;
    }

    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    // Header: session=<from MOUNT response>, seq=2.. (one per round),
    // cmd=TNFS_CMD_READDIRX. Payload: dir handle(1) + max entries(1).
    uint8_t buf[6];
    buf[0] = (uint8_t)(s_state.sid & 0xFF);
    buf[1] = (uint8_t)(s_state.sid >> 8);
    buf[2] = s_readdirx_seq++;
    buf[3] = TNFS_CMD_READDIRX;
    buf[4] = s_state.opendir_handle;
    buf[5] = SIDETNFS_READDIRX_BATCH_SIZE;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return;
    }

    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_mount_pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);

    cyw43_arch_lwip_end();
}

// Sends at most one new network request per call: OPENDIRX once MOUNT
// succeeded, then one READDIRX round at a time once OPENDIRX succeeded,
// until EOF/error/round-cap. Safe to call every GEMDRIVE main-loop
// iteration -- all guards make it a cheap no-op otherwise.
//
// Fase 5P: while the cache-based TNFS listing is active
// (SIDETNFS_EXPERIMENTAL_FS_LISTING), this legacy root OPENDIRX/READDIRX
// probe is skipped entirely. Root-cause analysis of "root truncated to 1
// entry" (see report) found that this probe and the new
// sidetnfs_dir_cache_service() were both opening "/" under the SAME TNFS
// session id at/around boot time -- two concurrent, uncoordinated
// OPENDIRX/READDIRX conversations sharing one session is the most likely
// explanation for the truncation (either a shared server-side read cursor,
// or sequence-number collision between the two independent counters that
// existed before this fix). MOUNT alone (still done above this function)
// is sufficient for sidetnfs_tnfs_listing_ready() once the cache system
// owns all TNFS directory traffic.
void sidetnfs_probe_service(void)
{
#if SIDETNFS_EXPERIMENTAL_FS_LISTING
    return;
#else
    if (s_state.mount_response_received && s_state.mount_rc == 0x00 && !s_state.opendir_sent)
    {
        send_opendirx_probe();
        return;
    }

    if (s_state.opendir_response_received && s_state.opendir_rc == 0x00 &&
        !s_state.readdirx_done && !s_state.readdirx_waiting_response)
    {
        send_readdirx_probe();
    }
#endif
}

// Fase 5H: record that networking/TNFS was skipped this boot (no WiFi
// configured, connect/NTP timeout, or ESC/CANCEL during either wait). Only
// touches RAM state -- safe to call regardless of WiFi/cyw43 state.
void sidetnfs_mark_network_skipped(void)
{
    if (s_state.network_skipped)
    {
        return; // already recorded, avoid needless dirty-thrashing
    }
    s_state.network_skipped = true;
    s_state.debug_dirty = true;
}

// Fase 5J: one-shot SD/FatFS root scan for comparison against the TNFS
// READDIRX root count. Pure FatFS, no network/SCFS involved. Synchronous
// but bounded by the (small) number of entries in hd_folder's root -- same
// class of operation GEMDRIVE's own FatFS handlers already do.
void sidetnfs_scan_sd_root_if_needed(const char *hd_folder)
{
    if (s_state.sd_scan_done || hd_folder == NULL)
    {
        return;
    }
    // Attempt at most once per boot, regardless of the outcome below.
    s_state.sd_scan_done = true;

    DIR dir;
    FRESULT fr = f_opendir(&dir, hd_folder);
    if (fr != FR_OK)
    {
        s_state.debug_dirty = true;
        return;
    }

    FILINFO fno;
    for (;;)
    {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0')
        {
            break; // error, or FatFS's empty-name end-of-directory marker
        }
        if (fno.fattrib & AM_DIR)
        {
            s_state.sd_scan_count_dirs++;
        }
        else
        {
            s_state.sd_scan_count_files++;
        }
    }
    f_closedir(&dir);
    s_state.debug_dirty = true;
}

// Fase 5P: true once MOUNT has succeeded. Previously (Fase 5N) this also
// required the legacy root OPENDIRX/READDIRX probe (Fase 5I/5J) to have
// completed -- but that probe no longer runs while
// SIDETNFS_EXPERIMENTAL_FS_LISTING is active (see sidetnfs_probe_service()
// and the report), so MOUNT success is now sufficient: the cache system
// itself is the thing that proves whether TNFS directory listing works.
// Does NOT check WiFi/network-teardown state -- see sidetnfs_probe.h.
bool sidetnfs_tnfs_listing_ready(void)
{
    return s_state.mount_response_received && s_state.mount_rc == TNFS_OK &&
           !s_state.network_skipped;
}

void sidetnfs_note_tnfs_fs_hit(void)
{
    s_state.fs_listing_hits++;
    s_state.debug_dirty = true;
}

void sidetnfs_note_tnfs_fs_error(void)
{
    s_state.fs_listing_errors++;
    s_state.debug_dirty = true;
}

// lwIP receive callback for the experimental Fsfirst/Fsnext channel --
// entirely separate from tnfs_recv_callback() above so an arbitrary-path
// OPENDIRX/READDIRX here can never be misrouted into (or clobber) the root
// probe's own state. Only copies the raw response and sets a ready flag;
// no FatFS, no printf, no blocking.
static void tnfs_fslisting_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                          const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;
    if (!p)
    {
        return;
    }

    uint16_t n = p->tot_len < sizeof(s_fslisting_resp.buf) ? (uint16_t)p->tot_len : (uint16_t)sizeof(s_fslisting_resp.buf);
    pbuf_copy_partial(p, s_fslisting_resp.buf, n, 0);
    s_fslisting_resp.len = n;
    s_fslisting_resp.cmd = n >= 4 ? s_fslisting_resp.buf[3] : 0xFFu;
    s_fslisting_resp.seq = n >= 3 ? s_fslisting_resp.buf[2] : 0xFFu;
    s_fslisting_resp.response_ready = true;

    pbuf_free(p);
}

static bool fslisting_ensure_pcb(void)
{
    if (s_fslisting_pcb != NULL)
    {
        return true;
    }
    cyw43_arch_lwip_begin();
    s_fslisting_pcb = udp_new();
    if (s_fslisting_pcb != NULL)
    {
        udp_recv(s_fslisting_pcb, tnfs_fslisting_recv_callback, NULL);
    }
    cyw43_arch_lwip_end();
    return s_fslisting_pcb != NULL;
}

// Fire-and-forget OPENDIRX send over s_fslisting_pcb -- returns immediately,
// never waits. The response (if any) is picked up later by
// tnfs_fslisting_recv_callback() and consumed by dir_cache_advance_one_step()
// or the direct-scan path (Fase 5R). Wire pattern is always "*" (local
// filtering only -- see sidetnfs_probe.h). Session id reused from the root
// probe's MOUNT response (s_state.sid); TNFS sessions are keyed by session
// id, not UDP source port, so sharing it across this separate PCB is safe.
// *out_seq receives the sequence number used, for cmd+seq response
// correlation (Fase 5R) -- only valid if this function returns true.
static bool fslisting_send_opendirx(const char *tnfs_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[8 + 1 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(s_state.sid & 0xFFu);
    buf[offset++] = (uint8_t)(s_state.sid >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_OPENDIRX;
    buf[offset++] = 0x00; // diropts
    buf[offset++] = 0x00; // sortopts
    buf[offset++] = 0x00; // max_count lo (0 = server returns total count)
    buf[offset++] = 0x00; // max_count hi
    buf[offset++] = '*';
    buf[offset++] = '\0';
    memcpy(&buf[offset], tnfs_path, path_len);
    offset += path_len;
    buf[offset++] = '\0';

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, buf, offset);
    udp_sendto(s_fslisting_pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fire-and-forget READDIRX send for up to max_entries entries on an
// already-open handle. Same non-blocking contract as
// fslisting_send_opendirx(), including *out_seq.
static bool fslisting_send_readdirx(uint8_t dir_handle, uint8_t max_entries, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[6];
    buf[0] = (uint8_t)(s_state.sid & 0xFFu);
    buf[1] = (uint8_t)(s_state.sid >> 8);
    buf[2] = seq;
    buf[3] = TNFS_CMD_READDIRX;
    buf[4] = dir_handle;
    buf[5] = max_entries;

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_fslisting_pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 5AA: fire-and-forget CLOSEDIR send for a handle obtained from
// OPENDIRX. Same non-blocking contract and wire shape as
// fslisting_send_readdirx() (header + single handle byte).
static bool fslisting_send_closedir(uint8_t dir_handle, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(SIDETNFS_SERVER_IP, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[5];
    buf[0] = (uint8_t)(s_state.sid & 0xFFu);
    buf[1] = (uint8_t)(s_state.sid >> 8);
    buf[2] = seq;
    buf[3] = TNFS_CMD_CLOSEDIR;
    buf[4] = dir_handle;

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_fslisting_pcb, p, &server_ip, SIDETNFS_SERVER_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Parse up to max_entries raw READDIRX entries from s_fslisting_resp into
// out_entries, normalizing each via sidetnfs_normalize_dir_entry() (Fase
// 5K). Mirrors parse_readdirx_entries()'s byte layout exactly. Entries that
// fail to normalize (unsupported name, special flag) are not added, but are
// counted in *out_skipped (dot-entries are skipped silently, uncounted --
// matches the existing root-probe's own convention).
static uint8_t fslisting_parse_batch(uint8_t batch, SidetnfsAtariDirEntry *out_entries,
                                       uint8_t max_entries, uint16_t *out_skipped)
{
    const uint8_t *buf = s_fslisting_resp.buf;
    uint16_t n = s_fslisting_resp.len;
    uint16_t needle = 9;
    uint8_t count = 0;

    for (uint8_t i = 0; i < batch && count < max_entries; i++)
    {
        if ((uint32_t)needle + 13 >= n)
        {
            break;
        }
        uint8_t flags = buf[needle];
        uint32_t size = (uint32_t)buf[needle + 1] | ((uint32_t)buf[needle + 2] << 8) |
                        ((uint32_t)buf[needle + 3] << 16) | ((uint32_t)buf[needle + 4] << 24);
        // Fase 5V: mtime (4 bytes LE) follows size, ctime (4 bytes LE)
        // follows mtime -- name starts at needle+13 (1+4+4+4). ctime is
        // still unused (not needed for GEMDOS date/time).
        uint32_t mtime = (uint32_t)buf[needle + 5] | ((uint32_t)buf[needle + 6] << 8) |
                         ((uint32_t)buf[needle + 7] << 16) | ((uint32_t)buf[needle + 8] << 24);
        const char *name = (const char *)&buf[needle + 13];
        uint16_t avail = (uint16_t)(n - (needle + 13));
        size_t nlen = strnlen(name, avail);
        if (nlen >= avail)
        {
            break;
        }
        needle = (uint16_t)(needle + 14 + nlen);

        if (nlen == 0 || (name[0] == '.' && (nlen == 1 || (name[1] == '.' && nlen == 2))))
        {
            continue; // skip "." / ".." entries, not counted as skipped
        }
        if (sidetnfs_normalize_dir_entry(name, flags, size, mtime, &out_entries[count]))
        {
            count++;
        }
        else
        {
            (*out_skipped)++;
        }
    }
    return count;
}

#if !SIDETNFS_DIR_CACHE_ENABLED
// Fase 5Y: TNFS DTA registry -- lookupTnfsDTA()/insertTnfsDTA()/
// releaseTnfsDTA() mirror the shape (not the storage) of the FatFS
// lookupDTA()/insertDTA()/releaseDTA() hash table in gemdrvemul.c: keyed by
// ndta, insert on Fsfirst, lookup on Fsnext, release on EOF/error/repeated
// Fsfirst. See report -- the SD-baseline hardware test showed real
// GEMDRVEMUL_FSNEXT_CALL dispatch works once state is registered this way.
// Fase 5AA: defined below (after fslisting_wait_for()) -- forward-declared
// here so insertTnfsDTA()/releaseTnfsDTA() can call it.
static void tnfs_dta_closedir(uint32_t ndta, uint8_t dir_handle);

static SidetnfsTnfsDtaSearch *lookupTnfsDTA(uint32_t ndta)
{
    for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (s_tnfs_dta_searches[i].active && s_tnfs_dta_searches[i].ndta == ndta)
        {
            return &s_tnfs_dta_searches[i];
        }
    }
    return NULL;
}

static SidetnfsTnfsDtaSearch *alloc_tnfs_dta_slot(void)
{
    for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (!s_tnfs_dta_searches[i].active)
        {
            return &s_tnfs_dta_searches[i];
        }
    }
    // Extremely unlikely: all slots active -- evict the first slot. Close
    // its handle first (Fase 5AA) so an evicted-but-still-open search
    // doesn't leak its TNFS directory handle on the server.
    SidetnfsTnfsDtaSearch *victim = &s_tnfs_dta_searches[0];
    if (victim->handle_valid)
    {
        tnfs_dta_closedir(victim->ndta, victim->dir_handle);
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_HANDLE_RELEASE, victim->ndta, NULL, NULL, NULL, 0, victim->dir_handle, 0,
                           0);
        victim->handle_valid = false;
    }
    return victim;
}

// Register/replace ndta's TNFS DTA-registry entry -- a repeated Fsfirst for
// the same ndta reuses (and overwrites) its existing slot, exactly like the
// SD/FatFS backend's insertDTA() replacing an existing DTANode (see
// gemdrvemul.c, and report: SIDETNFS_FSFIRST_REPEAT_CONTINUE is OFF by
// default in this model -- a repeated Fsfirst always starts fresh). Only
// called after OPENDIRX has already succeeded for dir_handle. Fase 5AA: if
// the reused slot still has an open handle from the search it's replacing
// (no EOF/release happened in between), that handle is CLOSEDIR'd first --
// otherwise a repeated Fsfirst/refresh cycle leaks one handle per repeat.
static SidetnfsTnfsDtaSearch *insertTnfsDTA(uint32_t ndta, const char *path, const char *pattern, uint8_t attribs,
                                             uint8_t dir_handle)
{
    SidetnfsTnfsDtaSearch *slot = lookupTnfsDTA(ndta);
    if (slot)
    {
        if (slot->handle_valid)
        {
            tnfs_dta_closedir(ndta, slot->dir_handle);
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_HANDLE_RELEASE, ndta, NULL, NULL, NULL, 0, slot->dir_handle, 0, 0);
            slot->handle_valid = false;
        }
    }
    else
    {
        slot = alloc_tnfs_dta_slot();
    }
    memset(slot, 0, sizeof(*slot));
    slot->ndta = ndta;
    strncpy(slot->path, path, sizeof(slot->path) - 1);
    strncpy(slot->pattern, pattern, sizeof(slot->pattern) - 1);
    slot->attribs = attribs;
    slot->dir_handle = dir_handle;
    slot->handle_valid = true;
    slot->active = true;
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_INSERT, ndta, path, pattern, NULL, 0, 0, 0, attribs);
    return slot;
}

// Release ndta's TNFS DTA-registry entry, if any (no-op otherwise). Fase
// 5AA: now also sends a real TNFS CLOSEDIR for the entry's dir_handle
// first, if it's still open (handle_valid) -- see
// SIDETNFS_TNFS_CLOSEDIR_ENABLED. handle_valid is cleared regardless of
// whether CLOSEDIR actually succeeded (see tnfs_dta_closedir() -- a
// failed/timed-out CLOSEDIR still must not block local cleanup or be
// retried), so a slot's handle is never CLOSEDIR'd twice.
static void releaseTnfsDTA(uint32_t ndta)
{
    SidetnfsTnfsDtaSearch *slot = lookupTnfsDTA(ndta);
    if (slot)
    {
        if (slot->handle_valid)
        {
            tnfs_dta_closedir(ndta, slot->dir_handle);
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_HANDLE_RELEASE, ndta, NULL, NULL, NULL, 0, slot->dir_handle, 0, 0);
            slot->handle_valid = false;
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_RELEASE, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
        slot->active = false;
    }
}

// Bounded-wait for a response matching expect_cmd+expect_seq. Any
// stray/mismatched response is discarded (never misread as the answer to
// a later request) -- see report on sequence/callback correlation.
static bool fslisting_wait_for(uint8_t expect_cmd, uint8_t expect_seq)
{
    for (int i = 0; i < SIDETNFS_FS_WAIT_MAX_ITER; i++)
    {
        cyw43_arch_poll();
        if (s_fslisting_resp.response_ready)
        {
            if (s_fslisting_resp.cmd == expect_cmd && s_fslisting_resp.seq == expect_seq)
            {
                return true;
            }
            s_fslisting_resp.response_ready = false; // stray/late response -- discard
        }
        sleep_us(SIDETNFS_FS_WAIT_STEP_US);
    }
    return false; // bounded-wait timeout
}

// Fase 5AA: send CLOSEDIR for dir_handle and wait (bounded -- same
// SIDETNFS_FS_WAIT_MAX_ITER/STEP_US timeout and cmd+seq validation as
// OPENDIRX/READDIRX, via fslisting_wait_for()) for the response. Never
// blocks indefinitely and never retries -- a failed/timed-out CLOSEDIR is
// logged and otherwise ignored; the caller (insertTnfsDTA()/
// releaseTnfsDTA()) always proceeds with local cleanup regardless of the
// outcome (see report: cleanup must never hang or crash, even if this
// server doesn't support/recognize CLOSEDIR). When
// SIDETNFS_TNFS_CLOSEDIR_ENABLED is 0, does nothing (Fase 5W/5Y/5Z
// behavior -- local-only release).
static void tnfs_dta_closedir(uint32_t ndta, uint8_t dir_handle)
{
#if SIDETNFS_TNFS_CLOSEDIR_ENABLED
    uint8_t seq = 0;
    if (!fslisting_send_closedir(dir_handle, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_CLOSEDIR_ERROR, ndta, NULL, NULL, NULL, 0, dir_handle, 0, 0);
        return;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_CLOSEDIR, ndta, NULL, NULL, NULL, 0, dir_handle, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_CLOSEDIR, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_CLOSEDIR_TIMEOUT, ndta, NULL, NULL, NULL, 0, dir_handle, 0, 0);
        return;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    s_fslisting_resp.response_ready = false;
    if (rc == TNFS_OK)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_CLOSEDIR_OK, ndta, NULL, NULL, NULL, 0, dir_handle, rc, 0);
    }
    else
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_CLOSEDIR_ERROR, ndta, NULL, NULL, NULL, 0, dir_handle, rc, 0);
    }
#else
    (void)ndta;
    (void)dir_handle;
#endif
}

// Fase 5Y: read SIDETNFS_READDIRX_MAX_ENTRIES (default 1) entries at a
// time, checking each against pattern/attribs, until a match, EOF, or
// SIDETNFS_TNFS_DTA_MAX_ROUNDS round-trips are used up. No entry is ever
// kept beyond the current round -- nothing is cached. Does NOT touch
// search->active -- lifecycle (insert/release) is the caller's
// responsibility (sidetnfs_tnfs_dta_start()/next(), which release on any
// non-FOUND result), only search->eof is DTA-search state this helper owns.
static SidetnfsDirSearchResult tnfs_dta_find_next_match(SidetnfsTnfsDtaSearch *search, SidetnfsAtariDirEntry *out_entry)
{
    for (uint32_t round = 0; round < SIDETNFS_TNFS_DTA_MAX_ROUNDS; round++)
    {
        if (search->eof)
        {
            return SIDETNFS_DIR_SEARCH_NOT_FOUND;
        }

        uint8_t seq = 0;
        if (!fslisting_send_readdirx(search->dir_handle, (uint8_t)SIDETNFS_READDIRX_MAX_ENTRIES, &seq))
        {
            return SIDETNFS_DIR_SEARCH_ERROR;
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_ONE, search->ndta, search->path, NULL, NULL, (uint16_t)round, 0,
                           0, 0);
        if (!fslisting_wait_for(TNFS_CMD_READDIRX, seq))
        {
            return SIDETNFS_DIR_SEARCH_ERROR;
        }
        uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
        uint8_t batch = s_fslisting_resp.len > 5 ? s_fslisting_resp.buf[5] : 0;
        uint16_t resp_len = s_fslisting_resp.len;
        s_fslisting_resp.response_ready = false;

        if (rc != TNFS_OK && rc != TNFS_EOF)
        {
            return SIDETNFS_DIR_SEARCH_ERROR;
        }
        if (rc == TNFS_EOF)
        {
            search->eof = true; // no more after this round -- but this round
                                 // may still carry one last entry to check
        }
        if (batch == 0 || resp_len <= 8)
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_EOF, search->ndta, search->path, NULL, NULL, 0, 0, 0, 0);
            continue; // next loop iteration sees search->eof and returns NOT_FOUND
        }

        SidetnfsAtariDirEntry entry;
        uint16_t skipped = 0;
        uint8_t got = fslisting_parse_batch(batch, &entry, 1, &skipped);
        if (got == 0)
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_SKIP, search->ndta, search->path, NULL, NULL, 0, 0, 0, 0);
            continue;
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_ENTRY, search->ndta, search->path, NULL, entry.name, 0, 0, 0,
                           entry.attr);
        if (sidetnfs_gemdos_pattern_match(entry.name, search->pattern) &&
            sidetnfs_gemdos_attr_match(entry.attr, search->attribs))
        {
            *out_entry = entry;
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_MATCH, search->ndta, search->path, NULL, entry.name, 0, 0,
                               0, entry.attr);
            return SIDETNFS_DIR_SEARCH_FOUND;
        }
        // no match -- loop and read the next entry
    }
    // Round cap reached without a definitive answer.
    return SIDETNFS_DIR_SEARCH_ERROR;
}

SidetnfsDirSearchResult sidetnfs_tnfs_dta_start(uint32_t ndta, const char *path,
                                                  const char *pattern, uint8_t attribs,
                                                  SidetnfsAtariDirEntry *out_entry)
{
    // Fase 5Y: OPENDIRX first -- the registry entry is only inserted
    // (insertTnfsDTA()) once we actually have a dir_handle, exactly like
    // the SD/FatFS backend only calls insertDTA() after f_findfirst()
    // already produced a match (see gemdrvemul.c). A repeated Fsfirst for
    // the same ndta always starts fresh (SIDETNFS_FSFIRST_REPEAT_CONTINUE
    // is OFF by default in this model, deliberately -- see report).
    uint8_t seq = 0;
    if (!fslisting_send_opendirx(path, &seq))
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_OPENDIRX, ndta, path, pattern, NULL, 0, 0, 0, attribs);
    if (!fslisting_wait_for(TNFS_CMD_OPENDIRX, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_OPENDIRX_ERROR, ndta, path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    uint8_t handle = s_fslisting_resp.len > 5 ? s_fslisting_resp.buf[5] : 0;
    s_fslisting_resp.response_ready = false;
    if (rc != TNFS_OK)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_OPENDIRX_ERROR, ndta, path, NULL, NULL, 0, 0, rc, 0);
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_OPENDIRX_OK, ndta, path, NULL, NULL, 0, handle, 0, 0);

    SidetnfsTnfsDtaSearch *search = insertTnfsDTA(ndta, path, pattern, attribs, handle);

    SidetnfsDirSearchResult result = tnfs_dta_find_next_match(search, out_entry);
    if (result != SIDETNFS_DIR_SEARCH_FOUND)
    {
        // NOT_FOUND (empty/exhausted listing) or ERROR -- release the
        // registry entry now, same as SD releasing its DTANode on a
        // failed Fsfirst (see gemdrvemul.c).
        releaseTnfsDTA(ndta);
    }
    return result;
}

SidetnfsDirSearchResult sidetnfs_tnfs_dta_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry)
{
    SidetnfsTnfsDtaSearch *search = lookupTnfsDTA(ndta);
    if (!search)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_LOOKUP_FAIL, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_LOOKUP_OK, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    SidetnfsDirSearchResult result = tnfs_dta_find_next_match(search, out_entry);
    if (result != SIDETNFS_DIR_SEARCH_FOUND)
    {
        releaseTnfsDTA(ndta);
    }
    return result;
}

bool sidetnfs_tnfs_dta_is_active(uint32_t ndta)
{
    return lookupTnfsDTA(ndta) != NULL;
}

void sidetnfs_tnfs_dta_release(uint32_t ndta)
{
    releaseTnfsDTA(ndta);
}

uint16_t sidetnfs_tnfs_dta_count_active(void)
{
    uint16_t count = 0;
    for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (s_tnfs_dta_searches[i].active)
        {
            count++;
        }
    }
    return count;
}
#endif // !SIDETNFS_DIR_CACHE_ENABLED

// Find a slot already holding path (any state), or -1.
static int find_cache_slot(const char *path)
{
    for (int i = 0; i < (int)SIDETNFS_DIR_CACHE_SLOTS; i++)
    {
        if (s_dir_cache_slots[i].state != SIDETNFS_CACHE_EMPTY &&
            strncmp(s_dir_cache_slots[i].path, path, sizeof(s_dir_cache_slots[i].path)) == 0)
        {
            return i;
        }
    }
    return -1;
}

// Pick a slot to (re)use for a new path: an EMPTY slot if any, else the
// least-recently-used slot that is not currently the one mid-network-build
// (s_building_slot is never evicted out from under itself).
static int alloc_cache_slot(void)
{
    for (int i = 0; i < (int)SIDETNFS_DIR_CACHE_SLOTS; i++)
    {
        if (s_dir_cache_slots[i].state == SIDETNFS_CACHE_EMPTY)
        {
            return i;
        }
    }
    int victim = -1;
    for (int i = 0; i < (int)SIDETNFS_DIR_CACHE_SLOTS; i++)
    {
        if (i == s_building_slot)
        {
            continue;
        }
        if (victim < 0 || s_dir_cache_slots[i].last_used_seq < s_dir_cache_slots[victim].last_used_seq)
        {
            victim = i;
        }
    }
    return victim >= 0 ? victim : 0;
}

// Fase 5Q: advance whichever slot is currently mid-network-build
// (s_building_slot) by exactly one non-blocking step -- one send, or one
// already-arrived response consumed. Never blocks, never sleeps, never
// sends more than one request before a response (or timeout, handled by
// the caller) is seen. Used both by sidetnfs_dir_cache_service() (once per
// main-loop tick, for the early async root warmup) and by
// sidetnfs_dir_cache_wait_ready() (pumped in a tight bounded loop for an
// Fsfirst cache-miss) -- same state machine either way, just driven at
// different cadences. No-op if the channel is idle (s_building_slot < 0).
static void dir_cache_advance_one_step(void)
{
    if (s_building_slot < 0)
    {
        return;
    }
    SidetnfsDirCacheSlot *slot = &s_dir_cache_slots[s_building_slot];
    if (slot->state != SIDETNFS_CACHE_LOADING)
    {
        s_building_slot = -1; // defensive -- should not happen
        return;
    }

    if (!s_building_opendirx_sent)
    {
        s_building_opendirx_sent = fslisting_send_opendirx(slot->path, &s_building_opendirx_seq);
        if (!s_building_opendirx_sent)
        {
            slot->state = SIDETNFS_CACHE_ERROR;
            s_building_slot = -1;
        }
        return;
    }

    if (!s_building_opendirx_done)
    {
        if (!s_fslisting_resp.response_ready)
        {
            return; // still waiting for the OPENDIRX response
        }
        if (!(s_fslisting_resp.cmd == TNFS_CMD_OPENDIRX && s_fslisting_resp.seq == s_building_opendirx_seq))
        {
            s_fslisting_resp.response_ready = false; // stray/late response -- discard
            return;
        }
        uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
        uint8_t handle = s_fslisting_resp.len > 5 ? s_fslisting_resp.buf[5] : 0;
        s_fslisting_resp.response_ready = false;
        if (rc != TNFS_OK)
        {
            slot->state = SIDETNFS_CACHE_ERROR;
            slot->error_rc = rc;
            sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_BUILD_ERROR, 0, slot->path, NULL, NULL, 0, 0, rc, 0);
            s_building_slot = -1;
            return;
        }
        s_building_dir_handle = handle;
        s_building_opendirx_done = true;
        sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_OPENDIRX_OK, 0, slot->path, NULL, NULL, 0, handle, 0, 0);
        return;
    }

    if (!s_building_readdirx_pending)
    {
        s_building_readdirx_pending =
            fslisting_send_readdirx(s_building_dir_handle, (uint8_t)SIDETNFS_FS_BATCH_SIZE, &s_building_readdirx_seq);
        if (!s_building_readdirx_pending)
        {
            slot->state = SIDETNFS_CACHE_ERROR;
            s_building_slot = -1;
        }
        return;
    }

    if (!s_fslisting_resp.response_ready)
    {
        return; // still waiting for this READDIRX response
    }
    if (!(s_fslisting_resp.cmd == TNFS_CMD_READDIRX && s_fslisting_resp.seq == s_building_readdirx_seq))
    {
        s_fslisting_resp.response_ready = false; // stray/late response -- discard
        return;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    uint8_t batch = s_fslisting_resp.len > 5 ? s_fslisting_resp.buf[5] : 0;
    uint16_t resp_len = s_fslisting_resp.len;
    s_fslisting_resp.response_ready = false;
    s_building_readdirx_pending = false;

    if (rc != TNFS_OK && rc != TNFS_EOF)
    {
        slot->state = SIDETNFS_CACHE_ERROR;
        slot->error_rc = rc;
        sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_BUILD_ERROR, 0, slot->path, NULL, NULL, 0, slot->count, rc, 0);
        s_building_slot = -1;
        return;
    }

    if (batch > 0 && resp_len > 8)
    {
        SidetnfsAtariDirEntry batch_entries[SIDETNFS_FS_BATCH_SIZE];
        uint8_t got = fslisting_parse_batch(batch, batch_entries, SIDETNFS_FS_BATCH_SIZE, &slot->skipped);
        for (uint8_t i = 0; i < got; i++)
        {
            if (slot->count >= SIDETNFS_DIR_CACHE_MAX_ENTRIES)
            {
                slot->truncated = true;
                break;
            }
            slot->entries[slot->count++] = batch_entries[i];
            if (batch_entries[i].attr & FS_ST_FOLDER)
            {
                slot->dirs++;
            }
            else
            {
                slot->files++;
            }
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_READDIRX_BATCH, 0, slot->path, NULL, NULL, batch, slot->count, rc, 0);
    }

    if (rc == TNFS_EOF || batch == 0)
    {
        // The ONLY place a slot is ever marked READY -- always after a
        // full EOF, never a partially-filled intermediate state. Frees the
        // network channel for the next request.
        slot->state = SIDETNFS_CACHE_READY;
        sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_BUILD_READY, 0, slot->path, NULL, NULL, slot->dirs, slot->count, 0, 0);
        s_building_slot = -1;
    }
    // else: stay LOADING; the next step will send another READDIRX round.
}

void sidetnfs_dir_cache_service(void)
{
    if (find_cache_slot("/") < 0 &&
        s_state.mount_response_received && s_state.mount_rc == TNFS_OK)
    {
        sidetnfs_dir_cache_request("/"); // early root warmup, once, right after MOUNT
    }
    dir_cache_advance_one_step();
}

bool sidetnfs_dir_cache_is_ready(const char *path)
{
    int slot = find_cache_slot(path);
    return slot >= 0 && s_dir_cache_slots[slot].state == SIDETNFS_CACHE_READY;
}

void sidetnfs_dir_cache_request(const char *path)
{
    int slot = find_cache_slot(path);
    if (slot >= 0 && s_dir_cache_slots[slot].state == SIDETNFS_CACHE_LOADING)
    {
        return; // already building exactly this path
    }
    if (s_building_slot >= 0)
    {
        return; // channel busy with a different path -- caller retries later
    }
    if (slot < 0)
    {
        slot = alloc_cache_slot();
    }
    if (s_dir_cache_slots[slot].state != SIDETNFS_CACHE_EMPTY &&
        strncmp(s_dir_cache_slots[slot].path, path, sizeof(s_dir_cache_slots[slot].path)) != 0)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_REPLACE, 0, s_dir_cache_slots[slot].path, path, NULL, (uint16_t)slot, 0,
                           0, 0);
    }
    memset(&s_dir_cache_slots[slot], 0, sizeof(s_dir_cache_slots[slot]));
    strncpy(s_dir_cache_slots[slot].path, path, sizeof(s_dir_cache_slots[slot].path) - 1);
    s_dir_cache_slots[slot].state = SIDETNFS_CACHE_LOADING;
    s_dir_cache_slots[slot].last_used_seq = ++s_dir_cache_use_counter;
    sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_BUILD_START, 0, path, NULL, NULL, (uint16_t)slot, 0, 0, 0);

    s_building_slot = slot;
    s_building_opendirx_sent = false;
    s_building_opendirx_done = false;
    s_building_dir_handle = 0;
    s_building_readdirx_pending = false;
}

bool sidetnfs_dir_cache_wait_ready(const char *path, uint32_t max_wait_ms)
{
    absolute_time_t start = get_absolute_time();
    while (absolute_time_diff_us(start, get_absolute_time()) < (int64_t)max_wait_ms * 1000)
    {
        int slot = find_cache_slot(path);
        if (slot >= 0 && s_dir_cache_slots[slot].state == SIDETNFS_CACHE_READY)
        {
            return true;
        }
        if (slot >= 0 && s_dir_cache_slots[slot].state == SIDETNFS_CACHE_ERROR)
        {
            // Fase 5T: negative cache -- a build for this exact path already
            // failed (e.g. TNFS rc=2, path does not exist). Stop
            // immediately, never retry within this wait, and never retry on
            // a later Fsfirst for the same path either (this ERROR slot
            // stays put until a different path evicts it via
            // alloc_cache_slot()'s LRU logic, or reboot -- see report).
            // Fixing this one condition (previously treated the same as
            // "no slot at all", which re-triggered sidetnfs_dir_cache_request()
            // every loop iteration) is what caused the repeated
            // CACHE_BUILD_START/CACHE_BUILD_ERROR storm for /AUTO.
            return false;
        }
        if (slot < 0)
        {
            sidetnfs_dir_cache_request(path); // no-op while channel busy elsewhere
        }
        cyw43_arch_poll();
        dir_cache_advance_one_step();
        sleep_us(SIDETNFS_DIR_CACHE_WAIT_STEP_US);
    }
    bool ready = sidetnfs_dir_cache_is_ready(path); // one last check at the deadline
    if (!ready)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_CACHE_BUILD_TIMEOUT, 0, path, NULL, NULL, 0, 0, 0, 0);
    }
    return ready;
}

static SidetnfsCacheSearchSlot *find_search_slot(uint32_t ndta)
{
    for (int i = 0; i < (int)SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (s_cache_searches[i].active && s_cache_searches[i].ndta == ndta)
        {
            return &s_cache_searches[i];
        }
    }
    return NULL;
}

static SidetnfsCacheSearchSlot *alloc_search_slot(void)
{
    for (int i = 0; i < (int)SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (!s_cache_searches[i].active)
        {
            return &s_cache_searches[i];
        }
    }
    return &s_cache_searches[0]; // extremely unlikely: evict the first slot
}

// Shared by sidetnfs_cache_search_start()/sidetnfs_fake_search_start()/
// next(): scan from next_index onward for the first pattern+attrib match.
// Pure RAM access -- no network, ever.
static SidetnfsDirSearchResult cache_search_advance(SidetnfsCacheSearchSlot *search, SidetnfsAtariDirEntry *out_entry)
{
    if (search->fake)
    {
        // Fase 5Q: exactly one synthetic entry ("NO_NETW.TXT") for root,
        // nothing for any other path -- see sidetnfs_fake_search_start().
        if (search->next_index > 0 || strncmp(search->path, "/", sizeof(search->path)) != 0)
        {
            search->active = false;
            return SIDETNFS_DIR_SEARCH_NOT_FOUND;
        }
        search->next_index = 1;
        SidetnfsAtariDirEntry fake_entry;
        memset(&fake_entry, 0, sizeof(fake_entry));
        strncpy(fake_entry.name, "NO_NETW.TXT", sizeof(fake_entry.name) - 1);
        fake_entry.attr = 0; // plain file
        fake_entry.valid = true;
        if (sidetnfs_gemdos_pattern_match(fake_entry.name, search->pattern) &&
            sidetnfs_gemdos_attr_match(fake_entry.attr, search->attribs))
        {
            *out_entry = fake_entry;
            sidetnfs_diag_log(SIDETNFS_DIAG_FAKE_FOUND, search->ndta, search->path, search->pattern,
                               fake_entry.name, 0, 0, 0, fake_entry.attr);
            return SIDETNFS_DIR_SEARCH_FOUND;
        }
        search->active = false;
        sidetnfs_diag_log(SIDETNFS_DIAG_FAKE_NOT_FOUND, search->ndta, search->path, search->pattern, NULL, 0, 0, 0, 0);
        return SIDETNFS_DIR_SEARCH_NOT_FOUND;
    }

    int slot_idx = find_cache_slot(search->path);
    if (slot_idx < 0 || s_dir_cache_slots[slot_idx].state != SIDETNFS_CACHE_READY)
    {
        // The slot for this path is gone/not ready (evicted, or build
        // failed after the search started).
        search->active = false;
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    SidetnfsDirCacheSlot *cache = &s_dir_cache_slots[slot_idx];

    while (search->next_index < cache->count)
    {
        const SidetnfsAtariDirEntry *candidate = &cache->entries[search->next_index++];
        if (sidetnfs_gemdos_pattern_match(candidate->name, search->pattern) &&
            sidetnfs_gemdos_attr_match(candidate->attr, search->attribs))
        {
            *out_entry = *candidate;
            return SIDETNFS_DIR_SEARCH_FOUND;
        }
    }
    search->active = false;
    return SIDETNFS_DIR_SEARCH_NOT_FOUND;
}

// If search is currently active for a DIFFERENT ndta than the one about to
// claim it, log the overwrite. Fixed-size event struct has no room for
// both old+new ndta and old+new path in one event: old ndta/path go in the
// ndta/path fields, new path in the pattern field -- the new ndta itself
// shows up in the FSFIRST_ENTER event logged right after this one.
static void diag_log_search_overwrite_if_needed(const SidetnfsCacheSearchSlot *search, uint32_t new_ndta,
                                                  const char *new_path)
{
    if (search->active && search->ndta != new_ndta)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_SEARCH_OVERWRITE, search->ndta, search->path, new_path, NULL, 0, 0, 0, 0);
    }
}

SidetnfsDirSearchResult sidetnfs_cache_search_start(uint32_t ndta, const char *path,
                                                      const char *pattern, uint8_t attribs,
                                                      SidetnfsAtariDirEntry *out_entry)
{
#if SIDETNFS_FSFIRST_REPEAT_CONTINUE
    // Fase 5U/5V: diagnostic/workaround, NOT a confirmed fix -- see report.
    // Some GEM/TOS flows apparently re-issue Fsfirst for the same
    // ndta/path/pattern instead of ever calling Fsnext (hardware testing
    // never showed a single FSNEXT_ENTER). Detect this and continue the
    // existing search from its current cursor instead of restarting at
    // index 0 -- otherwise the listing looks permanently stuck on the
    // first entry. Per the chosen policy, attribs are allowed to differ
    // between repeats (the search adopts the newest value for matching
    // from here on); only ndta+path+pattern need to match. Gated behind
    // its own switch so it can be disabled later without touching the
    // rest of the cache/search code.
    SidetnfsCacheSearchSlot *existing = find_search_slot(ndta);
    if (existing != NULL && existing->active && !existing->fake &&
        strncmp(existing->path, path, sizeof(existing->path)) == 0)
    {
        if (strncmp(existing->pattern, pattern, sizeof(existing->pattern)) == 0)
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_REPEAT_CONTINUE, ndta, path, pattern, NULL,
                               existing->next_index, 0, 0, attribs);
            existing->attribs = attribs;
            return cache_search_advance(existing, out_entry);
        }
        // Same ndta+path but a different pattern -- not a continuation,
        // genuinely restart (log why, since this looked like a repeat at
        // first glance).
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_REPEAT_RESTART, ndta, path, pattern, NULL, 0, 0, 0, 0);
    }
#endif // SIDETNFS_FSFIRST_REPEAT_CONTINUE

    bool was_ready = sidetnfs_dir_cache_is_ready(path);
    if (was_ready)
    {
        int slot_idx = find_cache_slot(path);
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_CACHE_HIT, ndta, path, NULL, NULL, 0,
                           slot_idx >= 0 ? s_dir_cache_slots[slot_idx].count : 0, 0, 0);
    }
    else
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_CACHE_MISS, ndta, path, NULL, NULL, 0, 0, 0, 0);
        bool ready = sidetnfs_dir_cache_wait_ready(path, SIDETNFS_DIR_CACHE_FSFIRST_WAIT_MS);
        int slot_idx = find_cache_slot(path);
        if (ready)
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_CACHE_READY, ndta, path, NULL, NULL, 0,
                               slot_idx >= 0 ? s_dir_cache_slots[slot_idx].count : 0, 0, 0);
        }
        else
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_CACHE_ERROR, ndta, path, NULL, NULL, 0, 0,
                               slot_idx >= 0 ? s_dir_cache_slots[slot_idx].error_rc : 0xFFu, 0);
            return SIDETNFS_DIR_SEARCH_ERROR;
        }
    }

    SidetnfsCacheSearchSlot *search = find_search_slot(ndta);
    if (!search)
    {
        search = alloc_search_slot();
    }
    diag_log_search_overwrite_if_needed(search, ndta, path);
    memset(search, 0, sizeof(*search));
    search->ndta = ndta;
    strncpy(search->path, path, sizeof(search->path) - 1);
    strncpy(search->pattern, pattern, sizeof(search->pattern) - 1);
    search->attribs = attribs;
    search->active = true;

    return cache_search_advance(search, out_entry);
}

SidetnfsDirSearchResult sidetnfs_fake_search_start(uint32_t ndta, const char *path,
                                                     const char *pattern, uint8_t attribs,
                                                     SidetnfsAtariDirEntry *out_entry)
{
#if SIDETNFS_FSFIRST_REPEAT_CONTINUE
    // Fase 5U/5V: same repeat-Fsfirst-as-continuation handling as
    // sidetnfs_cache_search_start() -- see report.
    SidetnfsCacheSearchSlot *existing = find_search_slot(ndta);
    if (existing != NULL && existing->active && existing->fake &&
        strncmp(existing->path, path, sizeof(existing->path)) == 0)
    {
        if (strncmp(existing->pattern, pattern, sizeof(existing->pattern)) == 0)
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_REPEAT_CONTINUE, ndta, path, pattern, NULL,
                               existing->next_index, 0, 0, attribs);
            existing->attribs = attribs;
            return cache_search_advance(existing, out_entry);
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_REPEAT_RESTART, ndta, path, pattern, NULL, 0, 0, 0, 0);
    }
#endif // SIDETNFS_FSFIRST_REPEAT_CONTINUE

    sidetnfs_diag_log(SIDETNFS_DIAG_FAKE_SEARCH_START, ndta, path, pattern, NULL, 0, 0, 0, attribs);

    SidetnfsCacheSearchSlot *search = find_search_slot(ndta);
    if (!search)
    {
        search = alloc_search_slot();
    }
    diag_log_search_overwrite_if_needed(search, ndta, path);
    memset(search, 0, sizeof(*search));
    search->ndta = ndta;
    search->fake = true;
    strncpy(search->path, path, sizeof(search->path) - 1);
    strncpy(search->pattern, pattern, sizeof(search->pattern) - 1);
    search->attribs = attribs;
    search->active = true;

    return cache_search_advance(search, out_entry);
}

SidetnfsDirSearchResult sidetnfs_cache_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry)
{
    SidetnfsCacheSearchSlot *search = find_search_slot(ndta);
    if (!search)
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    return cache_search_advance(search, out_entry);
}

bool sidetnfs_cache_search_is_active(uint32_t ndta)
{
    return find_search_slot(ndta) != NULL;
}

void sidetnfs_cache_search_close(uint32_t ndta)
{
    SidetnfsCacheSearchSlot *search = find_search_slot(ndta);
    if (search)
    {
        search->active = false;
    }
}

uint16_t sidetnfs_cache_search_count_active(void)
{
    uint16_t count = 0;
    for (unsigned i = 0; i < SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (s_cache_searches[i].active)
        {
            count++;
        }
    }
    return count;
}

// Fase 5G/5H/5I: build the short status text. No raw dumps by default (see
// SIDETNFS_DEBUG_SHOW_RAW). Each line is independent so partial progress
// (e.g. mount done, opendir/readdirx still pending) is always
// representable.
static int build_status_text(char *text, size_t text_size)
{
    char line1[64];
    char line2[64] = "";
    char line3[64] = "";
    char line3b[64] = "";
    char match_lines[SIDETNFS_MATCH_PATTERN_COUNT * 40] = "";
    char attr_lines[3 * 40] = "";
    char line4[64] = "";
    char line5[80] = "";
    char line6[64] = "";

    if (s_state.network_skipped)
    {
        return snprintf(text, text_size, "[SKIP] tnfs disabled\r\n");
    }
    if (!s_state.mount_probe_sent)
    {
        return 0; // nothing to report yet
    }
    else if (!s_state.mount_response_received)
    {
        snprintf(line1, sizeof(line1), "[WAIT] mount response pending\r\n");
    }
    else if (s_state.mount_rc != 0x00)
    {
        snprintf(line1, sizeof(line1), "[ERR] mount failed - rc: %02X\r\n", s_state.mount_rc);
    }
    else
    {
        snprintf(line1, sizeof(line1), "[OK] mounted successful - session id: %04X\r\n", s_state.sid);

        if (s_state.opendir_sent)
        {
            if (!s_state.opendir_response_received)
            {
                snprintf(line2, sizeof(line2), "[WAIT] opendirx / response pending\r\n");
            }
            else if (s_state.opendir_rc != 0x00)
            {
                snprintf(line2, sizeof(line2), "[ERR] opendirx / failed - rc: %02X\r\n", s_state.opendir_rc);
            }
            else
            {
                snprintf(line2, sizeof(line2), "[OK] opendirx / successful - handle: %u\r\n", s_state.opendir_handle);

                if (s_state.readdirx_started)
                {
                    if (!s_state.readdirx_done)
                    {
                        snprintf(line3, sizeof(line3), "[WAIT] readdirx / pending\r\n");
                    }
                    else if (s_state.readdirx_last_rc != TNFS_OK && s_state.readdirx_last_rc != TNFS_EOF)
                    {
                        snprintf(line3, sizeof(line3), "[ERR] readdirx / failed - rc: %02X\r\n", s_state.readdirx_last_rc);
                    }
                    else
                    {
                        snprintf(line3, sizeof(line3), "[OK] readdirx / - %u dirs, %u files\r\n",
                                 (unsigned)s_state.readdirx_count_dirs, (unsigned)s_state.readdirx_count_files);
                        snprintf(line3b, sizeof(line3b), "[OK] translate / - %u ok, %u skipped\r\n",
                                 (unsigned)s_state.translate_ok_count, (unsigned)s_state.translate_skipped_count);

                        size_t match_len = 0;
                        for (uint8_t p = 0; p < SIDETNFS_MATCH_PATTERN_COUNT && match_len < sizeof(match_lines); p++)
                        {
                            int r = snprintf(&match_lines[match_len], sizeof(match_lines) - match_len,
                                              "[OK] match %s - %u entries\r\n",
                                              SIDETNFS_MATCH_PATTERNS[p], (unsigned)s_state.match_counts[p]);
                            if (r <= 0)
                            {
                                break;
                            }
                            match_len += (size_t)r;
                        }

                        size_t attr_len = 0;
                        int r = snprintf(&attr_lines[attr_len], sizeof(attr_lines) - attr_len,
                                          "[OK] attr normal - %u entries\r\n", (unsigned)s_state.attr_normal_count);
                        if (r > 0)
                        {
                            attr_len += (size_t)r;
                        }
                        r = snprintf(&attr_lines[attr_len], sizeof(attr_lines) - attr_len,
                                     "[OK] attr folder - %u entries\r\n", (unsigned)s_state.attr_folder_count);
                        if (r > 0)
                        {
                            attr_len += (size_t)r;
                        }
                        if (s_state.attr_hidden_count > 0)
                        {
                            snprintf(&attr_lines[attr_len], sizeof(attr_lines) - attr_len,
                                     "[OK] attr hidden - %u entries\r\n", (unsigned)s_state.attr_hidden_count);
                        }
                    }
                }
            }
        }
    }

    if (s_state.sd_scan_done)
    {
        snprintf(line4, sizeof(line4), "[OK] sd root - %u dirs, %u files\r\n",
                 (unsigned)s_state.sd_scan_count_dirs, (unsigned)s_state.sd_scan_count_files);

        if (s_state.readdirx_started && s_state.readdirx_done &&
            (s_state.readdirx_last_rc == TNFS_OK || s_state.readdirx_last_rc == TNFS_EOF))
        {
            snprintf(line5, sizeof(line5), "[INFO] diff expected: DEBUG.TXT exists only on SD\r\n");
        }
    }

    // Fase 5N (experimental): short summary only, never a per-entry line --
    // independent of the readdirx/sd-scan sections above since this is a
    // separate opt-in feature (see SIDETNFS_EXPERIMENTAL_FS_LISTING).
    if (s_state.fs_listing_errors > 0)
    {
        snprintf(line6, sizeof(line6), "[ERR] tnfs fs listing - %u errors\r\n", (unsigned)s_state.fs_listing_errors);
    }
    else if (s_state.fs_listing_hits > 0)
    {
        snprintf(line6, sizeof(line6), "[OK] tnfs fs listing - %u hits\r\n", (unsigned)s_state.fs_listing_hits);
    }

    int len = snprintf(text, text_size, "%s%s%s%s%s%s%s%s%s", line1, line2, line3, line3b, match_lines, attr_lines, line4, line5, line6);

#if SIDETNFS_DEBUG_SHOW_RAW
    if (len > 0 && (size_t)len < text_size)
    {
        char raw_hex[SIDETNFS_DEBUG_RAW_SIZE * 3 + 1] = {0};
        size_t raw_hex_len = 0;
        uint16_t raw_count = s_state.last_response_len < SIDETNFS_DEBUG_RAW_SIZE
                                 ? s_state.last_response_len
                                 : SIDETNFS_DEBUG_RAW_SIZE;
        for (uint16_t i = 0; i < raw_count && raw_hex_len < sizeof(raw_hex); i++)
        {
            int r = snprintf(&raw_hex[raw_hex_len], sizeof(raw_hex) - raw_hex_len, "%02X ", s_state.last_raw[i]);
            if (r <= 0)
            {
                break;
            }
            raw_hex_len += (size_t)r;
        }
        int extra = snprintf(text + len, text_size - (size_t)len, "raw: %s\r\n", raw_hex);
        if (extra > 0)
        {
            len += extra;
        }
    }
#endif

    return len;
}

// Fase 5F/5G/5I: (re)write DEBUG.TXT only when dirty, only when hd_folder is
// known, with simple throttling. Never blocks, never retries, silently
// does nothing on any failure -- the dirty flag is left set on failure or
// when throttled, so a later call will retry.
void sidetnfs_debug_file_service(const char *hd_folder)
{
#if !SIDETNFS_DEBUG_FILE_ENABLED
    (void)hd_folder;
    return; // Fase 5N stability investigation: DEBUG.TXT writes disabled entirely
#else
    if (!s_state.debug_dirty || hd_folder == NULL)
    {
        return;
    }

    static absolute_time_t s_last_write_time;
    static bool s_have_last_write_time = false;
    if (s_have_last_write_time &&
        absolute_time_diff_us(s_last_write_time, get_absolute_time()) < (SIDETNFS_DEBUG_WRITE_MIN_INTERVAL_MS * 1000))
    {
        return; // throttled -- stays dirty, retried on a later call
    }

    char path[160];
    int n = snprintf(path, sizeof(path), "%s/DEBUG.TXT", hd_folder);
    if (n <= 0 || (size_t)n >= sizeof(path))
    {
        return;
    }

    char text[900];
    int len = build_status_text(text, sizeof(text));
    if (len <= 0)
    {
        return;
    }
    if ((size_t)len >= sizeof(text))
    {
        len = sizeof(text) - 1; // defensive: still a safe, valid write
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);

    s_last_write_time = get_absolute_time();
    s_have_last_write_time = true;

    if (fr != FR_OK)
    {
        return; // stays dirty; a later call will retry
    }

    UINT written;
    f_write(&file, text, (UINT)len, &written);
    f_close(&file);

    s_state.debug_dirty = false;
    s_state.debug_write_count++;
#endif // SIDETNFS_DEBUG_FILE_ENABLED
}
