/**
 * File: sidetnfs_probe.h
 * Description: One-shot, fire-and-forget UDP reachability probe toward the
 * TNFS server. Not a TNFS client -- no mount, no session, no reply handling.
 * Only meant to prove network reachability (visible via tcpdump on the
 * server) without touching GEMDRIVE or its timing.
 */
#ifndef SIDETNFS_PROBE_H
#define SIDETNFS_PROBE_H

#include <stdbool.h>
#include <stdint.h>

// Fase 5N/5O: compile-time switch for the experimental TNFS-backed
// Fsfirst/Fsnext directory listing. When 0, gemdrvemul.c compiles out every
// TNFS-listing branch entirely, so GEMDRVEMUL_FSFIRST_CALL/
// GEMDRVEMUL_FSNEXT_CALL behave exactly as the existing, proven SD/FatFS
// backend (byte-for-byte unchanged). When 1 (Fase 5O meaning): directory
// listing is served from the RAM directory-cache described in
// sidetnfs_probe.h below -- never a live per-entry network wait inside
// Fsfirst/Fsnext (that was Fase 5N's approach, removed after real-hardware
// instability -- see report), and no SD fallback once TNFS listing is
// actually active (network ok + MOUNT succeeded) since the end goal is
// SD-less operation.
#ifndef SIDETNFS_EXPERIMENTAL_FS_LISTING
#define SIDETNFS_EXPERIMENTAL_FS_LISTING 1
#endif

// Fase 5N (stability investigation): compile-time switch for all DEBUG.TXT
// writes. Independent of SIDETNFS_EXPERIMENTAL_FS_LISTING above -- this
// lets Testbuild A disable the debug file while keeping the TNFS
// Fsfirst/Fsnext path active (and a future Testbuild B do the reverse), to
// isolate whether the SD/FatFS DEBUG.TXT write itself is a source of
// timing instability. When 0, sidetnfs_debug_file_service() returns
// immediately -- no f_open/f_write ever happens, DEBUG.TXT is neither
// created nor overwritten.
#ifndef SIDETNFS_DEBUG_FILE_ENABLED
#define SIDETNFS_DEBUG_FILE_ENABLED 1
#endif

// Fase 5R/5W (diagnostic): compile-time switch for the TNFS directory-cache
// layer, independent of SIDETNFS_EXPERIMENTAL_FS_LISTING above. Only
// meaningful when SIDETNFS_EXPERIMENTAL_FS_LISTING == 1. When 1:
// GEMDRVEMUL_FSFIRST_CALL/FSNEXT_CALL serve directory listing from the RAM
// cache (sidetnfs_cache_search_start()/next(), Fase 5O/5Q) -- this whole
// layer (root pre-cache, cache slots, batch fetching, repeat-Fsfirst
// continuation) turned out to be much more machinery than the problem
// needed, see report. When 0 (default, Fase 5Y): GEMDRVEMUL_FSFIRST_CALL/
// FSNEXT_CALL use the TNFS DTA-registry model instead
// (sidetnfs_tnfs_dta_start()/next(), Fase 5Y) -- one OPENDIRX + a READDIRX
// loop reading SIDETNFS_READDIRX_MAX_ENTRIES entries at a time, with search
// state registered under ndta exactly the way the SD/FatFS backend's
// insertDTA()/lookupDTA() works (one GEMDOS call in, one directory entry
// out, no RAM-resident directory cache at all -- see report: the SD
// baseline proved this ndta-keyed registration is what makes real Fsnext
// dispatch work). Neither setting ever falls back to SD/FatFS directory
// listing, and the fake no-network listing (sidetnfs_fake_search_start())
// is unaffected by this switch either way.
#ifndef SIDETNFS_DIR_CACHE_ENABLED
#define SIDETNFS_DIR_CACHE_ENABLED 0
#endif

// Fase 5S/5Y: explicit alias for "use the TNFS DTA-registry path" --
// complementary to SIDETNFS_DIR_CACHE_ENABLED by default, but named
// separately for clarity in build configs that set both explicitly. Not the
// old batch-of-8, single-global-slot diagnostic scan from Fase 5R (nor the
// unregistered Fase 5W "live search") -- a TNFS search registered under
// ndta (SIDETNFS_TNFS_DTA_SLOTS slots) reading SIDETNFS_READDIRX_MAX_ENTRIES
// entries per READDIRX round (default 1).
#ifndef SIDETNFS_DIRECT_SCAN_ENABLED
#define SIDETNFS_DIRECT_SCAN_ENABLED (!SIDETNFS_DIR_CACHE_ENABLED)
#endif

// Fase 5W: how many entries GEMDRVEMUL_FSFIRST_CALL/FSNEXT_CALL's live
// search requests per READDIRX round. Default 1 -- deliberately as close
// as possible to f_findnext()'s own one-entry-at-a-time contract (see
// report: fetching larger batches is what led to the whole cache/slot/
// repeat-Fsfirst machinery in the first place). Only meaningful when
// SIDETNFS_DIR_CACHE_ENABLED == 0.
#ifndef SIDETNFS_READDIRX_MAX_ENTRIES
#define SIDETNFS_READDIRX_MAX_ENTRIES 1
#endif

// Fase 5Y: number of concurrent TNFS directory searches (one per active
// ndta) the TNFS DTA-registry can track -- the direct TNFS-side analogue of
// the existing FatFS insertDTA()/lookupDTA() hash table (see report: the SD
// baseline proved Fsnext IS dispatched correctly when the search state is
// keyed by ndta the same way SD does it). No directory entries are ever
// cached -- each slot only holds the open dir_handle + path/pattern/attribs
// + eof flag (see SidetnfsTnfsDtaSearch). Renamed from
// SIDETNFS_LIVE_SEARCH_SLOTS (Fase 5W) -- same mechanism, explicitly framed
// as a DTA registry instead of a bespoke "live search" concept.
#ifndef SIDETNFS_TNFS_DTA_SLOTS
#define SIDETNFS_TNFS_DTA_SLOTS 8
#endif

// Fase 5Y: hard cap on READDIRX round-trips per single Fsfirst/Fsnext call
// (each round asks for SIDETNFS_READDIRX_MAX_ENTRIES entries and may not
// match the requested pattern/attribs, requiring another round). Prevents
// scanning a large/mismatched directory from ever becoming an unbounded
// loop. Renamed from SIDETNFS_LIVE_SEARCH_MAX_ROUNDS (Fase 5W).
#ifndef SIDETNFS_TNFS_DTA_MAX_ROUNDS
#define SIDETNFS_TNFS_DTA_MAX_ROUNDS 32
#endif

// Fase 5AA: send a real TNFS CLOSEDIR for every OPENDIRX handle once it's
// no longer needed (EOF, Fsnext exhausted, DTA_RELEASE, a repeated Fsfirst
// replacing an existing search, or an error mid-search) -- see report:
// without this, directory handles leaked on the server/session across
// repeated Fsfirst/refresh cycles, and after enough leaked handles the
// server stopped returning listings at all ("everything empty after a few
// refreshes"). When 0, falls back to the Fase 5W/5Y/5Z behavior of only
// clearing local bookkeeping (kept for a quick A/B revert if CLOSEDIR ever
// turns out to be the wrong opcode/format for this server -- see
// TNFS_CMD_CLOSEDIR in sidetnfs_probe.c).
#ifndef SIDETNFS_TNFS_CLOSEDIR_ENABLED
#define SIDETNFS_TNFS_CLOSEDIR_ENABLED 1
#endif

// Fase 5S: compile-time switch for the RAM-only Fsfirst/Fsnext/cache
// diagnostic eventlog (see SidetnfsDiagEvent below). When 0, every
// sidetnfs_diag_log() call site compiles to nothing (all arguments
// unused/no-op) -- zero cost, zero behavior change. When 1, each call
// records one fixed-size event into a bounded RAM array; never touches SD,
// never touches UART, never blocks.
#ifndef SIDETNFS_FS_DIAG_ENABLED
#define SIDETNFS_FS_DIAG_ENABLED 1
#endif

// Fase 5S: compile-time switch for dumping the diagnostic eventlog (plus a
// short cache/search-slot summary) to <hd_folder>/DEBUG.TXT when the
// SELECT button is pressed (edge-triggered, once per press -- see
// gemdrvemul.c). Independent of SIDETNFS_DEBUG_FILE_ENABLED and
// SIDETNFS_FS_DIAG_ENABLED: with the latter off there is simply nothing to
// dump (an empty event count), but the dump-on-SELECT mechanism itself
// still compiles in. Never triggers automatically -- no main-loop tick,
// no Fsfirst/Fsnext call site, and no network callback ever writes
// DEBUG.TXT; only the SELECT edge-handler does.
#ifndef SIDETNFS_DEBUG_DUMP_ON_SELECT
#define SIDETNFS_DEBUG_DUMP_ON_SELECT 1
#endif

// Fase 5U/5V: compile-time switch for treating a repeated Fsfirst (same
// ndta/path/pattern, no Fsnext seen in between) as a continuation of the
// existing search cursor rather than restarting at index 0. This is a
// diagnostic/workaround measure, NOT a confirmed fix for why
// GEMDRVEMUL_FSNEXT_CALL is never dispatched (see report) -- kept behind
// its own switch so it can be disabled later without touching the rest of
// the cache/search code.
#ifndef SIDETNFS_FSFIRST_REPEAT_CONTINUE
#define SIDETNFS_FSFIRST_REPEAT_CONTINUE 0
#endif

// Fase 5B: create a UDP PCB and udp_connect() it to the TNFS server, then
// immediately remove it again. Sends no payload at all -- udp_connect() is a
// local lwIP operation (sets the PCB's remote ip/port filter) and does not
// put anything on the wire. Must only be called after WiFi is confirmed
// connected. Never blocks, never retries, never waits for a reply, never
// logs. Returns true if the PCB was created and udp_connect() succeeded.
bool sidetnfs_udp_connect_test(void);

// Fase 5A: send a single UDP packet to the TNFS server. Kept for reference;
// not called automatically as of Fase 5B. Must only be called after WiFi is
// confirmed connected. Never blocks, never retries, never waits for a
// reply, never logs. Silently does nothing on any failure.
void sidetnfs_send_udp_probe(void);

// Fase 5C/5F: send a single TNFS MOUNT request and register a receive
// callback for the (optional, asynchronous) reply. Fire-and-forget -- never
// waits, never retries, never logs. Must only be called after WiFi is
// confirmed connected. Marks the debug state dirty so
// sidetnfs_debug_file_service() will pick this up on its next call.
void sidetnfs_send_mount_probe(void);

// Fase 5G: check whether the MOUNT response is in and successful, and if so
// (and OPENDIR "/" was not already sent this boot) send a single OPENDIR
// request over the existing MOUNT PCB. Fire-and-forget, non-blocking, safe
// to call every GEMDRIVE main-loop iteration -- the internal guards make it
// a cheap no-op otherwise. Must only be called after
// sidetnfs_send_mount_probe() was called (and ideally after a
// cyw43_arch_poll() so the MOUNT response has had a chance to arrive).
void sidetnfs_probe_service(void);

// Fase 5F: if the debug state is dirty (mount probe just sent, or a
// response just arrived) and hd_folder is known, (re)write
// <hd_folder>/DEBUG.TXT with FA_CREATE_ALWAYS, then clear the dirty flag.
// No-op (and stays dirty for a later call) if hd_folder is NULL, nothing is
// dirty, or the last write was too recent (simple throttling). Never
// blocks, never retries, silently does nothing on any failure. Safe to
// call from multiple places, including once per GEMDRIVE main-loop
// iteration -- the dirty-check keeps it a cheap no-op otherwise.
void sidetnfs_debug_file_service(const char *hd_folder);

// Fase 5H: mark that networking/TNFS was skipped this boot (no WiFi
// configured, WiFi connect failed/timed out, or the user pressed ESC/
// CANCEL during the WiFi or NTP wait). Only touches RAM state -- safe to
// call even though WiFi/cyw43 may already have been torn down by the
// caller. Marks the debug state dirty so sidetnfs_debug_file_service()
// will write a short "[SKIP] tnfs disabled" line once hd_folder is known.
// Never blocks, never logs, no UART.
void sidetnfs_mark_network_skipped(void);

// Fase 5J: one-shot SD/FatFS root directory scan (via f_opendir/f_readdir),
// purely to compare entry counts against the TNFS READDIRX root scan in
// DEBUG.TXT. Independent of network state -- uses hd_folder directly, no
// TNFS/SCFS involved. Runs at most once per boot (guarded internally).
// Synchronous but bounded by the size of hd_folder's root (a handful of
// entries) -- same order of magnitude as GEMDRIVE's own existing FatFS
// directory calls, so no new blocking-risk class is introduced. Never
// retries, never logs, silently does nothing on any failure.
void sidetnfs_scan_sd_root_if_needed(const char *hd_folder);

// Fase 5K: a TNFS OPENDIRX/READDIRX entry normalized into Atari/GEMDOS form.
// 8.3 names only for now -- see sidetnfs_is_supported_83_name(). No
// long-name<->short-name mapping table exists yet, so anything that isn't
// already a valid 8.3 name is skipped rather than shortened.
typedef struct
{
    char name[14];  // 8.3 name + NUL (matches the DTA d_fname convention)
    uint8_t attr;   // Atari/GEMDOS attribute bits (FS_ST_*)
    uint32_t size;  // file size; directories forced to 0
    uint16_t date;  // DOS/GEMDOS date -- fixed placeholder for now, see report
    uint16_t time;  // DOS/GEMDOS time -- fixed placeholder for now, see report
    bool valid;     // true if out was actually filled in
    bool skipped;   // true if this entry was intentionally skipped
} SidetnfsAtariDirEntry;

// Fase 5K: strict, uppercase-only 8.3 name check. Rejects anything that
// isn't already a valid "NAME" or "NAME.EXT" GEMDOS-compatible name
// (lowercase, long names, multiple dots, spaces, path separators, FAT-
// invalid characters, ".", "..", and leading-dot/AppleDouble names are all
// rejected). No malloc, no I/O.
bool sidetnfs_is_supported_83_name(const char *name);

// Fase 5K: convert one TNFS OPENDIRX/READDIRX entry into Atari/GEMDOS form.
// Returns true and fills *out (valid=true) on success; returns false and
// sets out->skipped=true (rest zeroed) if the entry is intentionally not
// representable yet (unsupported name, or the TNFS "special" flag). Pure
// RAM-to-RAM conversion -- no I/O, no network, no blocking.
bool sidetnfs_normalize_dir_entry(const char *tnfs_name, uint8_t tnfs_flags,
                                   uint32_t tnfs_size, uint32_t tnfs_mtime,
                                   SidetnfsAtariDirEntry *out);

// Fase 5L: local GEMDOS-style wildcard match against an already-normalized
// 8.3 Atari name (see sidetnfs_normalize_dir_entry()). Mirrors the existing
// SD/FatFS GEMDRIVE pattern semantics from seach_path_2_st() in
// gemdrvemul.c: a pattern ending in ".*" has that suffix stripped before
// matching (so "*.*" behaves like "*", and "FOLDER.*" behaves like
// "FOLDER" -- matching extension-less names too). This is a deliberate
// GEMDOS/TOS quirk already relied on by the SD backend, not standard FatFS
// wildcard behavior. After that adjustment, '*' matches zero or more
// characters and '?' matches exactly one, compared case-insensitively (both
// sides are expected to already be uppercase 8.3 per Fase 5K). No malloc,
// no I/O.
bool sidetnfs_gemdos_pattern_match(const char *name83, const char *pattern);

// Fase 5M: match one normalized entry's Atari/GEMDOS attribute byte (see
// sidetnfs_normalize_dir_entry()) against a GEMDOS Fsfirst-style search
// attribute mask. Mirrors the core bitwise check the existing SD/FatFS
// backend uses in GEMDRVEMUL_FSFIRST_CALL/GEMDRVEMUL_FSNEXT_CALL
// (gemdrvemul.c): "match if any requested bit is present on the entry"
// (entry_attr & search_attr). TNFS has no real read-only/system/archive
// bits (left unset by sidetnfs_normalize_dir_entry()), so a plain,
// non-folder, non-hidden entry (attr == 0) is treated as matching
// FS_ST_ARCH -- standing in for the archive bit a real FAT file almost
// always carries, so a normal-files search still finds TNFS files.
// Deliberately does NOT replicate GEMDRIVE's separate, caller-side
// "auto-widen search_attr with FS_ST_ARCH unless searching for
// FS_ST_LABEL" step from GEMDRVEMUL_FSFIRST_CALL -- that is an Fsfirst
// call-site concern, out of scope here (no Fsfirst changes), and left for
// a later phase to wire up. No malloc, no I/O.
bool sidetnfs_gemdos_attr_match(uint8_t entry_attr, uint8_t search_attr);

// Fase 5O/5Q (experimental, SIDETNFS_EXPERIMENTAL_FS_LISTING only): result
// of one cache-backed (or fake no-network) directory search step.
typedef enum
{
    SIDETNFS_DIR_SEARCH_FOUND = 0,   // *out_entry filled with a matching entry
    SIDETNFS_DIR_SEARCH_NOT_FOUND,   // end of listing reached, no (more) matching entry -- definitive
    SIDETNFS_DIR_SEARCH_ERROR,       // cache-miss refresh could not be resolved in time -- no SD fallback, see report
} SidetnfsDirSearchResult;

// Fase 5N: true if a TNFS-backed Fsfirst/Fsnext may be attempted this boot:
// MOUNT succeeded, and the existing OPENDIRX/READDIRX root probe (Fase
// 5I/5J) already completed successfully, proving this server/session
// actually works. Does NOT check WiFi/network teardown state -- the caller
// (gemdrvemul.c) must additionally gate on its own sidetnfs_network_ok
// before calling anything else below, exactly like the existing probe
// service call.
bool sidetnfs_tnfs_listing_ready(void);

// Fase 5O/5Q: non-blocking directory-cache tick. Call every GEMDRIVE
// main-loop iteration (gated behind sidetnfs_network_ok, right next to
// sidetnfs_probe_service()). Auto-starts a cache build for "/" the first
// time it runs after MOUNT succeeds (early root warmup). Otherwise advances
// whichever cache slot is currently mid-network-build by at most one
// network step (one send, or one already-arrived response consumed) --
// never blocks, never sleeps, never more than one request in flight across
// ALL slots (see report: two concurrent OPENDIRX/READDIRX conversations
// sharing one TNFS session caused entry loss -- there is only ever one
// slot "building" at a time, regardless of how many slots exist).
void sidetnfs_dir_cache_service(void);

// Fase 5Q: true if any cache slot is SIDETNFS_CACHE_READY for exactly this
// path (see report for the fixed-size, SIDETNFS_DIR_CACHE_SLOTS-slot cache
// design that replaced Fase 5O's single slot).
bool sidetnfs_dir_cache_is_ready(const char *path);

// Fase 5Q: (re)start a cache build for path. Reuses an existing slot
// already holding this exact path if there is one; otherwise claims an
// empty slot, or evicts the least-recently-used slot not currently
// mid-network-build. If a DIFFERENT path is currently mid-network-build,
// this call is a no-op (the channel handles one conversation at a time) --
// the caller (sidetnfs_dir_cache_wait_ready()) retries until the channel is
// free. Never blocks.
void sidetnfs_dir_cache_request(const char *path);

// Fase 5Q: used only by GEMDRVEMUL_FSFIRST_CALL on a cache miss. Repeatedly
// requests path (see sidetnfs_dir_cache_request() -- a no-op while a
// different path currently owns the network channel) and pumps
// cyw43_arch_poll()+the same non-blocking step function used by
// sidetnfs_dir_cache_service(), bounded by max_wait_ms wall-clock time
// (never an unbounded loop). Returns true once a slot is READY for path,
// false on SIDETNFS_CACHE_ERROR or on timeout. Never called from Fsnext --
// Fsnext only ever reads an already-built slot.
bool sidetnfs_dir_cache_wait_ready(const char *path, uint32_t max_wait_ms);

// Fase 5Q: start a new cache-backed directory search for ndta, replacing
// any previous search this ndta had (fixed-size search-slot table, see
// report; up to SIDETNFS_SEARCH_SLOTS concurrent searches for DIFFERENT
// ndta values are tracked independently -- this is what actually fixed the
// "root always truncated" bug, not a network/protocol issue). path/
// pattern/attribs follow the same conventions as the old Fase 5N
// sidetnfs_dir_search_start() (mount-root-relative path, GEMDOS-style
// pattern already preprocessed by seach_path_2_st(), attribs mask). If no
// slot is already READY for path, this blocks (bounded, see
// sidetnfs_dir_cache_wait_ready()) waiting for a build to finish -- this is
// the ONLY place a bounded wait happens; sidetnfs_cache_search_next()
// never waits. On FOUND, *out_entry is filled and the search stays active.
// On NOT_FOUND, the search is closed (definitive, listing fully scanned).
// On ERROR (build failed or timed out), the search is not left active --
// caller must not fall back to SD (see report: SD fallback for directory
// listing is removed entirely as of Fase 5Q).
SidetnfsDirSearchResult sidetnfs_cache_search_start(uint32_t ndta, const char *path,
                                                      const char *pattern, uint8_t attribs,
                                                      SidetnfsAtariDirEntry *out_entry);

// Fase 5Q: start a fake, memory-only directory search for ndta -- used
// instead of sidetnfs_cache_search_start() when TNFS was never available
// this boot (no WiFi, ESC-skip, or MOUNT never succeeded). NEVER touches
// cyw43/lwIP (which may already be torn down in this case, see Fase 5H).
// path == "/" yields exactly one synthetic entry, "NO_NETW.TXT" (a plain
// file, size 0, date/time 0/0 -- opening it is not expected to work yet,
// see report); any other path yields an immediately-empty listing (no
// entries, no error). Registers in the SAME search-slot table as
// sidetnfs_cache_search_start(), so sidetnfs_cache_search_next()/
// is_active()/close() work identically for fake and real searches --
// GEMDRVEMUL_FSNEXT_CALL needs no awareness of which kind is active.
SidetnfsDirSearchResult sidetnfs_fake_search_start(uint32_t ndta, const char *path,
                                                     const char *pattern, uint8_t attribs,
                                                     SidetnfsAtariDirEntry *out_entry);

// Fase 5Y (only compiled when SIDETNFS_DIR_CACHE_ENABLED == 0): start a
// TNFS directory search for ndta -- OPENDIRX for path, then a bounded
// READDIRX loop (SIDETNFS_READDIRX_MAX_ENTRIES entries per round, up to
// SIDETNFS_TNFS_DTA_MAX_ROUNDS rounds) until the first pattern+attribs
// match, EOF, or the round cap. No entry cache of any kind -- this is
// intentionally as close as possible to the SD/FatFS backend's
// f_findfirst()+insertDTA() contract (see report: the SD-baseline hardware
// test proved real GEMDRVEMUL_FSNEXT_CALL dispatch works once the search
// state is registered under ndta the same way SD's DTA hash table does it).
// Internally keyed by ndta in a small fixed-size registry
// (insertTnfsDTA()/lookupTnfsDTA()/releaseTnfsDTA(), SIDETNFS_TNFS_DTA_SLOTS
// slots) -- never a FatFS DIR*/FILINFO*, but the same "insert on Fsfirst,
// lookup on Fsnext" shape. Starting a new search for an ndta that already
// had one replaces it (mirrors the SD/FatFS backend's own behavior on a
// repeated Fsfirst -- see report; SIDETNFS_FSFIRST_REPEAT_CONTINUE is OFF by
// default). DOES touch the network on every call (bounded, cmd+seq
// validated) -- unlike the cache path, there is no RAM copy to serve a hit
// from. On a terminal result (NOT_FOUND/ERROR) the ndta's registry entry is
// released before returning (see sidetnfs_tnfs_dta_release()).
SidetnfsDirSearchResult sidetnfs_tnfs_dta_start(uint32_t ndta, const char *path,
                                                  const char *pattern, uint8_t attribs,
                                                  SidetnfsAtariDirEntry *out_entry);

// Fase 5Y: look up ndta's registry entry (lookupTnfsDTA()) and continue the
// search -- same bounded network behavior as sidetnfs_tnfs_dta_start(). On a
// terminal result the entry is released before returning, same as above.
SidetnfsDirSearchResult sidetnfs_tnfs_dta_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry);
bool sidetnfs_tnfs_dta_is_active(uint32_t ndta);

// Fase 5Y: release ndta's TNFS DTA-registry entry (releaseTnfsDTA()), if
// any -- no-op if ndta has none (already released, or never had one). Does
// NOT send a TNFS CLOSEDIR (never implemented in this codebase -- left for
// the server/session to reclaim, see report); this only clears local
// bookkeeping so the slot can be reused.
void sidetnfs_tnfs_dta_release(uint32_t ndta);

// Fase 5Z: number of currently-active TNFS DTA-registry entries -- used by
// GEMDRVEMUL_DTA_RELEASE_CALL to report a count back to the 68k side, the
// same way countDTA() does for the FatFS table (see gemdrvemul.c).
uint16_t sidetnfs_tnfs_dta_count_active(void);

// Fase 5O: continue the active search for ndta (real or fake -- see
// sidetnfs_cache_search_is_active()). Pure RAM scan -- no network, no
// wait, ever.
SidetnfsDirSearchResult sidetnfs_cache_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry);

// Fase 5O: true if ndta has a currently active search (real or fake).
bool sidetnfs_cache_search_is_active(uint32_t ndta);

// Fase 5O: explicitly close the active search for ndta, if it is the one
// currently active (no-op otherwise).
void sidetnfs_cache_search_close(uint32_t ndta);

// Fase 5Z: number of currently-active cache/fake searches -- see
// sidetnfs_tnfs_dta_count_active() above.
uint16_t sidetnfs_cache_search_count_active(void);

// Fase 5N: record one successful/failed TNFS Fsfirst/Fsnext hit for the
// short DEBUG.TXT summary line (throttled/dirty-flag driven, like every
// other debug line -- never a per-entry write).
void sidetnfs_note_tnfs_fs_hit(void);
void sidetnfs_note_tnfs_fs_error(void);

// Fase 5S: RAM-only Fsfirst/Fsnext/cache diagnostic eventlog. See
// SIDETNFS_FS_DIAG_ENABLED above and sidetnfs_diag_log()/
// sidetnfs_diag_dump_on_select() below.
typedef enum
{
    SIDETNFS_DIAG_FSFIRST_ENTER = 0,
    SIDETNFS_DIAG_FSFIRST_ATTR_PREP,
    SIDETNFS_DIAG_FSFIRST_CACHE_HIT,
    SIDETNFS_DIAG_FSFIRST_CACHE_MISS,
    SIDETNFS_DIAG_FSFIRST_CACHE_READY,
    SIDETNFS_DIAG_FSFIRST_CACHE_ERROR,
    SIDETNFS_DIAG_FSFIRST_FOUND,
    SIDETNFS_DIAG_FSFIRST_NOT_FOUND,
    SIDETNFS_DIAG_FSFIRST_RETURN,
    SIDETNFS_DIAG_FSNEXT_ENTER,
    SIDETNFS_DIAG_FSNEXT_SEARCH_ACTIVE,
    SIDETNFS_DIAG_FSNEXT_SEARCH_MISSING,
    SIDETNFS_DIAG_FSNEXT_FOUND,
    SIDETNFS_DIAG_FSNEXT_END,
    SIDETNFS_DIAG_FSNEXT_RETURN,
    SIDETNFS_DIAG_SEARCH_OVERWRITE,
    SIDETNFS_DIAG_CACHE_REPLACE,
    SIDETNFS_DIAG_CACHE_BUILD_START,
    SIDETNFS_DIAG_CACHE_OPENDIRX_OK,
    SIDETNFS_DIAG_CACHE_READDIRX_BATCH,
    SIDETNFS_DIAG_CACHE_BUILD_READY,
    SIDETNFS_DIAG_CACHE_BUILD_ERROR,
    SIDETNFS_DIAG_CACHE_BUILD_TIMEOUT,
    SIDETNFS_DIAG_FAKE_SEARCH_START,
    SIDETNFS_DIAG_FAKE_FOUND,
    SIDETNFS_DIAG_FAKE_NOT_FOUND,
    // Fase 5U: repeated-Fsfirst-as-continuation diagnostics (see report --
    // hardware testing never showed a single FSNEXT_ENTER; GEM/TOS appears
    // to re-issue Fsfirst for the same ndta/path/pattern instead).
    SIDETNFS_DIAG_FSFIRST_REPEAT_CONTINUE,
    SIDETNFS_DIAG_FSFIRST_REPEAT_RESTART,
    SIDETNFS_DIAG_FSNEXT_CASE_REACHED,
    // Fase 5V: every dispatched command_id (see report -- confirms whether
    // an unrecognized/unexpected command arrives during what looks like a
    // repeated Fsfirst, instead of a genuine FSNEXT_CALL).
    SIDETNFS_DIAG_COMMAND_ENTER,
    // Fase 5W/5Y: TNFS OPENDIRX/READDIRX events (renamed in Fase 5Y from
    // FSFIRST_OPENDIRX*/READDIRX_* to the TNFS_* names, to line up with the
    // TNFS DTA-registry model -- same events, no behavior change).
    SIDETNFS_DIAG_TNFS_OPENDIRX,
    SIDETNFS_DIAG_TNFS_OPENDIRX_OK,
    SIDETNFS_DIAG_TNFS_OPENDIRX_ERROR,
    SIDETNFS_DIAG_TNFS_READDIRX_ONE,
    SIDETNFS_DIAG_TNFS_READDIRX_ENTRY,
    SIDETNFS_DIAG_TNFS_READDIRX_SKIP,
    SIDETNFS_DIAG_TNFS_READDIRX_MATCH,
    SIDETNFS_DIAG_TNFS_READDIRX_EOF,
    // Fase 5X: SD-baseline measurement events. Logged from the SD/FatFS
    // Fsfirst/Fsnext code path itself (only reachable when
    // SIDETNFS_EXPERIMENTAL_FS_LISTING == 0) purely as logging -- no
    // f_findfirst()/f_findnext()/insertDTA()/lookupDTA()/populate_dta()
    // behavior is touched, see report. DTA_LOOKUP_OK/FAIL and DTA_INSERT are
    // the backend-agnostic names (mirrors what a future TNFS DTA-registry
    // model would log); the SD_* variants additionally mark that this
    // specific event came from the FatFS backend, so a mixed SD+TNFS log
    // (should that ever happen) stays unambiguous.
    SIDETNFS_DIAG_DTA_INSERT,
    SIDETNFS_DIAG_DTA_LOOKUP_OK,
    SIDETNFS_DIAG_DTA_LOOKUP_FAIL,
    SIDETNFS_DIAG_SD_FIND_FIRST,
    SIDETNFS_DIAG_SD_FIND_NEXT,
    SIDETNFS_DIAG_SD_DTA_INSERT,
    SIDETNFS_DIAG_SD_DTA_LOOKUP_OK,
    SIDETNFS_DIAG_SD_DTA_LOOKUP_FAIL,
    // Fase 5Y: TNFS DTA-registry events -- the TNFS-side analogue of
    // DTA_INSERT/DTA_LOOKUP_OK/DTA_LOOKUP_FAIL above, logged from
    // insertTnfsDTA()/lookupTnfsDTA()/releaseTnfsDTA() call sites (see
    // report).
    SIDETNFS_DIAG_TNFS_DTA_INSERT,
    SIDETNFS_DIAG_TNFS_DTA_LOOKUP_OK,
    SIDETNFS_DIAG_TNFS_DTA_LOOKUP_FAIL,
    SIDETNFS_DIAG_TNFS_DTA_RELEASE,
    // Fase 5Z: GEMDRVEMUL_DTA_EXIST_CALL/GEMDRVEMUL_DTA_RELEASE_CALL events
    // -- these two commands (not Fsfirst/Fsnext) are what the 68k side
    // actually queries between Fsfirst and Fsnext to decide whether to
    // issue a real Fsnext at all (see report: SD-baseline log showed
    // COMMAND_ENTER DTA_EXIST immediately before FSNEXT_ENTER). Before Fase
    // 5Z, DTA_EXIST/DTA_RELEASE only ever looked at the FatFS DTA table
    // (lookupDTA()/releaseDTA()), never the TNFS DTA-registry or the fake
    // no-network search table -- these events show which backend answered.
    SIDETNFS_DIAG_DTA_EXIST_ENTER,
    SIDETNFS_DIAG_DTA_EXIST_FATFS_OK,
    SIDETNFS_DIAG_DTA_EXIST_TNFS_OK,
    SIDETNFS_DIAG_DTA_EXIST_FAKE_OK,
    SIDETNFS_DIAG_DTA_EXIST_FAIL,
    SIDETNFS_DIAG_DTA_EXIST_RETURN,
    SIDETNFS_DIAG_DTA_RELEASE_ENTER,
    SIDETNFS_DIAG_DTA_RELEASE_FATFS,
    SIDETNFS_DIAG_DTA_RELEASE_TNFS,
    SIDETNFS_DIAG_DTA_RELEASE_FAKE,
    SIDETNFS_DIAG_DTA_RELEASE_RETURN,
    // Fase 5AA: TNFS CLOSEDIR events (see SIDETNFS_TNFS_CLOSEDIR_ENABLED).
    // No separate TNFS_HANDLE_ALREADY_CLOSED event -- handle_valid and
    // active are always set/cleared together in this codebase (see
    // insertTnfsDTA()/releaseTnfsDTA() in sidetnfs_probe.c), so a
    // double-close of an active slot cannot structurally occur; the
    // absence of a second TNFS_CLOSEDIR for the same handle in the log is
    // itself the proof.
    SIDETNFS_DIAG_TNFS_CLOSEDIR,
    SIDETNFS_DIAG_TNFS_CLOSEDIR_OK,
    SIDETNFS_DIAG_TNFS_CLOSEDIR_ERROR,
    SIDETNFS_DIAG_TNFS_CLOSEDIR_TIMEOUT,
    SIDETNFS_DIAG_TNFS_HANDLE_RELEASE,
} SidetnfsDiagEventType;

#define SIDETNFS_DIAG_MAX_EVENTS 256

typedef struct
{
    uint16_t seq;
    uint16_t event;
    uint32_t ndta;
    uint16_t index;
    uint16_t count;
    uint8_t result;
    uint8_t attr;
    char path[24];
    char pattern[16];
    char name[14];
} SidetnfsDiagEvent;

// Fase 5S: record one diagnostic event (see SIDETNFS_FS_DIAG_ENABLED). Pass
// NULL/0 for any field not relevant to a given event type. No malloc, no
// I/O, no blocking -- pure RAM array write. Stops recording once
// SIDETNFS_DIAG_MAX_EVENTS is reached (keeps the earliest events, which
// matter most for a boot/cold-start diagnosis -- see report) rather than
// wrapping as a ring buffer.
void sidetnfs_diag_log(SidetnfsDiagEventType event, uint32_t ndta, const char *path,
                        const char *pattern, const char *name, uint16_t index,
                        uint16_t count, uint8_t result, uint8_t attr);

// Fase 5S: write the diagnostic eventlog plus a short cache/search-slot
// summary to <hd_folder>/DEBUG.TXT. Call only from the SELECT-button
// edge-handler in gemdrvemul.c (never automatically, never from
// Fsfirst/Fsnext, never from a network callback). Silently does nothing if
// hd_folder is NULL or the SD write fails -- never crashes.
void sidetnfs_diag_dump_on_select(const char *hd_folder);

#endif // SIDETNFS_PROBE_H
