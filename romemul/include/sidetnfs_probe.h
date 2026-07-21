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

#include "sidetnfs_config.h" // sidetnfs_drive_config_t -- see sidetnfs_slot_tnfs_context_t below

// Fase 6A/6E: central backend choice for GEMDRIVE directory listing.
// Replaced the old on/off SIDETNFS_EXPERIMENTAL_FS_LISTING switch (removed
// in Fase 6E) with an explicit backend identifier, so future backends (e.g.
// a flash-image-backed one) have a real slot instead of yet another
// boolean. Only directory listing (Fsfirst/Fsnext/DTA_EXIST/DTA_RELEASE)
// is backend-routed as of this phase -- file I/O (Fopen/Fread/Fwrite/etc.)
// remains SD/FatFS-backed regardless of SIDETNFS_BACKEND_TYPE.
#define SIDETNFS_BACKEND_SD 0
#define SIDETNFS_BACKEND_TNFS 1
#define SIDETNFS_BACKEND_FLASH 2

#ifndef SIDETNFS_BACKEND_TYPE
#define SIDETNFS_BACKEND_TYPE SIDETNFS_BACKEND_TNFS
#endif

#if SIDETNFS_BACKEND_TYPE == SIDETNFS_BACKEND_FLASH
#error "SIDETNFS_BACKEND_FLASH not implemented yet"
#endif

// Fase 6A: compatibility macros -- gemdrvemul.c/sidetnfs_probe.c's main
// Fsfirst/Fsnext/DTA_EXIST/DTA_RELEASE routing checks
// SIDETNFS_USE_TNFS_LISTING/SIDETNFS_USE_SD_LISTING instead of the raw
// backend-type comparison, so a future third real backend doesn't require
// touching every call site again.
#define SIDETNFS_USE_TNFS_LISTING (SIDETNFS_BACKEND_TYPE == SIDETNFS_BACKEND_TNFS)
#define SIDETNFS_USE_SD_LISTING (SIDETNFS_BACKEND_TYPE == SIDETNFS_BACKEND_SD)
#define SIDETNFS_USE_FLASH_LISTING (SIDETNFS_BACKEND_TYPE == SIDETNFS_BACKEND_FLASH)

// Fase 5N/5O/6E: SIDETNFS_EXPERIMENTAL_FS_LISTING (the original on/off
// switch for the TNFS-backed Fsfirst/Fsnext directory listing) has been
// removed entirely -- superseded by SIDETNFS_BACKEND_TYPE above.
// SIDETNFS_USE_TNFS_LISTING/SIDETNFS_USE_SD_LISTING (both derived from
// SIDETNFS_BACKEND_TYPE) are now the only routing checks in
// gemdrvemul.c/sidetnfs_probe.c; the name "experimental" no longer applied
// once TNFS listing became the proven, stable default (see
// SIDETNFS_PHASE5_DIRECTORY_LISTING.md).

// Fase 8B: two central, single-purpose compile-time options for building a
// strict TNFS-only firmware image with no SD/FatFS touch at all (Falcon
// diagnostic build). Passed through from build.sh/CMakeLists.txt the same
// way _DEBUG already is (env var -> add_definitions()) -- see CMakeLists.txt.
// Deliberately just two macros, each with one clear meaning, instead of
// scattered booleans (SIDETNFS_ENABLE_SD_SUPPORT covers every SD/FatFS
// touch anywhere in the GEMDRIVE/TNFS trap-handling path;
// SIDETNFS_ENABLE_DEBUG covers the DEBUG.TXT/SELECT-button snapshot).
// Both default to 1 (today's existing behavior, completely unchanged for
// every normal build) -- only the explicit Fase 8B diagnostic build sets
// either to 0.
#ifndef SIDETNFS_ENABLE_SD_SUPPORT
#define SIDETNFS_ENABLE_SD_SUPPORT 1
#endif

#ifndef SIDETNFS_ENABLE_DEBUG
#define SIDETNFS_ENABLE_DEBUG 1
#endif

// Fase 10B: temporary test-build switch. When 1, GEMDRIVE offers ONLY the
// read-only configuration drive (GEMDRIVE_FILE_BACKEND_CONFIG_FLASH,
// romemul/sidetnfs_config_drive_backend.c) serving the Fase 10A
// flash-embedded SIDETNFS.PRG/README.TXT -- no SD, WiFi or TNFS access is
// attempted or needed for GEMDRIVE to become ready. Existing SD/TNFS code
// is not removed or rewritten; it is simply not reached while this is 1.
// Default 0 (normal behavior, unchanged).
#ifndef SIDETNFS_CONFIG_DRIVE_ONLY
#define SIDETNFS_CONFIG_DRIVE_ONLY 0
#endif

// Fase 5N (stability investigation): compile-time switch for all DEBUG.TXT
// writes. Independent of the backend/listing routing above -- this
// lets Testbuild A disable the debug file while keeping the TNFS
// Fsfirst/Fsnext path active (and a future Testbuild B do the reverse), to
// isolate whether the SD/FatFS DEBUG.TXT write itself is a source of
// timing instability. When 0, sidetnfs_debug_file_service() returns
// immediately -- no f_open/f_write ever happens, DEBUG.TXT is neither
// created nor overwritten. Fase 8B: this function is retired/never called
// (see report), but the macro is kept working and now also follows
// SIDETNFS_ENABLE_DEBUG by default, same as SIDETNFS_DEBUG_DUMP_ON_SELECT
// below.
#ifndef SIDETNFS_DEBUG_FILE_ENABLED
#define SIDETNFS_DEBUG_FILE_ENABLED SIDETNFS_ENABLE_DEBUG
#endif

// Fase 6B/6C: the RAM directory-cache layer (root pre-cache, cache slots,
// batch fetching) that used to live behind SIDETNFS_DIR_CACHE_ENABLED, and
// the SIDETNFS_DIRECT_SCAN_ENABLED marker that used to name the
// alternative, have both been removed entirely -- the cache worked, but
// didn't fit the GEMDOS/DTA/Fsnext flow cleanly (see report/
// SIDETNFS_PHASE5_DIRECTORY_LISTING.md). The TNFS DTA-registry model
// (sidetnfs_tnfs_dta_start()/next(), Fase 5Y) is now unconditionally the
// only TNFS directory-listing path for SIDETNFS_BACKEND_TNFS: one OPENDIRX
// + a READDIRX loop reading SIDETNFS_READDIRX_MAX_ENTRIES entries at a
// time, with search state registered under ndta exactly the way the
// SD/FatFS backend's insertDTA()/lookupDTA() works. Never falls back to
// SD/FatFS directory listing; the fake no-network listing
// (sidetnfs_fake_search_start()) is unaffected and unchanged.

// Fase 5W: how many entries GEMDRVEMUL_FSFIRST_CALL/FSNEXT_CALL's TNFS DTA
// search requests per READDIRX round. Default 1 -- deliberately as close
// as possible to f_findnext()'s own one-entry-at-a-time contract (see
// report: fetching larger batches is what led to the whole cache/slot/
// repeat-Fsfirst machinery this phase removed).
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

// Fase 5AA/6D: a real TNFS CLOSEDIR is always sent for every OPENDIRX
// handle once it's no longer needed (EOF, Fsnext exhausted, DTA_RELEASE, a
// repeated Fsfirst replacing an existing search, or an error mid-search) --
// see report: without this, directory handles leaked on the server/session
// across repeated Fsfirst/refresh cycles, and after enough leaked handles
// the server stopped returning listings at all ("everything empty after a
// few refreshes"). This was previously gated behind
// SIDETNFS_TNFS_CLOSEDIR_ENABLED (a quick A/B revert in case CLOSEDIR
// turned out to be the wrong opcode/format for this server -- see
// TNFS_CMD_CLOSEDIR in sidetnfs_probe.c); hardware testing confirmed it is
// correct and required, so the switch was removed in Fase 6D.

// Fase 5S/6F: the RAM-only Fsfirst/Fsnext diagnostic eventlog
// (sidetnfs_diag_log(), see SidetnfsDiagEvent below) is now always on --
// the SIDETNFS_FS_DIAG_ENABLED on/off switch was removed in Fase 6F, since
// the eventlog is what the SELECT-button DEBUG.TXT dump is built from, and
// that dump stays useful for diagnosing the next phases (Fopen/Fread, file
// handles, path mapping, TNFS errors). Each call records one fixed-size
// event into a bounded RAM array; never touches SD, never touches UART,
// never blocks.

// Fase 5S: compile-time switch for dumping the diagnostic eventlog to
// <hd_folder>/DEBUG.TXT when the SELECT button is pressed (edge-triggered,
// once per press -- see gemdrvemul.c). Independent of
// SIDETNFS_DEBUG_FILE_ENABLED. Never triggers automatically -- no
// main-loop tick, no Fsfirst/Fsnext call site, and no network callback
// ever writes DEBUG.TXT; only the SELECT edge-handler does. Fase 8B: this
// is the actual, currently-used DEBUG.TXT mechanism (SIDETNFS_DEBUG_FILE_ENABLED
// above guards a retired function that's never called) -- now follows
// SIDETNFS_ENABLE_DEBUG by default, the single knob the Fase 8B
// TNFS-only-nosd-nodebug build sets to 0 to guarantee zero f_open/f_write
// from diagnostic code.
#ifndef SIDETNFS_DEBUG_DUMP_ON_SELECT
#define SIDETNFS_DEBUG_DUMP_ON_SELECT SIDETNFS_ENABLE_DEBUG
#endif

// Fase 7D5: temporary file-I/O diagnosis focus mode -- NOT meant to stay on
// permanently (see report). When 1: per-round TNFS READ detail
// (SIDETNFS_DIAG_FREAD_TNFS_READ/READ_BUFF_TNFS_RC inside the internal
// chunk-loop) is logged in full, and the per-entry directory-listing detail
// events (TNFS_READDIRX_ONE/ENTRY/SKIP/MATCH) are additionally suppressed
// to make room in the fixed SIDETNFS_DIAG_MAX_EVENTS budget. When 0
// (default): file-I/O events are still logged, but only one summary per
// GEMDRVEMUL_READ_BUFF_CALL (same shape as before this phase), and
// directory-listing detail events log normally.
#ifndef SIDETNFS_DEBUG_FOCUS_FILE_IO
#define SIDETNFS_DEBUG_FOCUS_FILE_IO 0
#endif

// Fase 7F-debugfix: temporary Fseek diagnosis focus mode -- NOT meant to
// stay on permanently (see report). Defaults ON for this phase (unlike
// SIDETNFS_DEBUG_FOCUS_FILE_IO) since seeing FSEEK_* events was the whole
// point. When 1: GEMDRVEMUL_COMMAND_ENTER is only logged for the small set
// of commands relevant to Fopen/Fread/Fclose/Fseek/Fattrib/Fdatetime/
// Dgetpath/Dsetpath (see the GEMDRVEMUL_COMMAND_ENTER call site in
// gemdrvemul.c), and directory-listing per-entry detail events
// (TNFS_READDIRX_ONE/ENTRY/SKIP/MATCH/EOF, TNFS_OPENDIRX_OK) are
// additionally suppressed -- same reasoning and same "logging only, no
// control-flow change" contract as SIDETNFS_DEBUG_FOCUS_FILE_IO.
#ifndef SIDETNFS_DEBUG_FOCUS_FSEEK
#define SIDETNFS_DEBUG_FOCUS_FSEEK 1
#endif

// Fase 7G: temporary Fdelete diagnosis focus mode -- same style/contract as
// SIDETNFS_DEBUG_FOCUS_FSEEK, not meant to stay on permanently. Defaults
// ON. When 1: GEMDRVEMUL_COMMAND_ENTER also includes GEMDRVEMUL_FDELETE_CALL
// in its whitelist (composed with SIDETNFS_DEBUG_FOCUS_FSEEK's own
// whitelist -- either focus mode being on is enough to show its own
// commands), and directory-listing detail events are suppressed the same
// way (see SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL in sidetnfs_probe.c).
#ifndef SIDETNFS_DEBUG_FOCUS_FDELETE
#define SIDETNFS_DEBUG_FOCUS_FDELETE 1
#endif

// Fase 7H: temporary Frename diagnosis focus mode -- same style/contract as
// SIDETNFS_DEBUG_FOCUS_FSEEK/FDELETE. Defaults ON.
#ifndef SIDETNFS_DEBUG_FOCUS_FRENAME
#define SIDETNFS_DEBUG_FOCUS_FRENAME 1
#endif

// Fase 7I: temporary Dcreate diagnosis focus mode -- same style/contract as
// SIDETNFS_DEBUG_FOCUS_FSEEK/FDELETE/FRENAME. Defaults ON.
#ifndef SIDETNFS_DEBUG_FOCUS_DCREATE
#define SIDETNFS_DEBUG_FOCUS_DCREATE 1
#endif

// Fase 7J: temporary Ddelete diagnosis focus mode -- same style/contract as
// SIDETNFS_DEBUG_FOCUS_FSEEK/FDELETE/FRENAME/DCREATE. Defaults ON.
#ifndef SIDETNFS_DEBUG_FOCUS_DDELETE
#define SIDETNFS_DEBUG_FOCUS_DDELETE 1
#endif

// Fase 5U/5V (REMOVED in Fase 6B): repeated-Fsfirst-as-continuation was a
// diagnostic workaround for GEMDRVEMUL_FSNEXT_CALL never being dispatched --
// superseded by the real fix (Fase 5Z: DTA_EXIST/DTA_RELEASE recognizing
// TNFS DTA-registry state), which made real Fsnext dispatch work and this
// workaround unnecessary (see report). SIDETNFS_FSFIRST_REPEAT_CONTINUE and
// its code are gone; a repeated Fsfirst for the same ndta now always starts
// fresh, same as the SD/FatFS backend.

// Fase 9D: load the first usable (used, TNFS, UDP) drive from the
// persistent sidetnfs_config drive-list (sidetnfs_config_init() must
// already have run this boot) into this module's "active server" state,
// replacing the old hardcoded SIDETNFS_SERVER_IP/PORT/MOUNT_NAME
// constants. Must be called exactly once at boot, before any other
// function in this file that touches the network -- in practice, before
// sidetnfs_udp_connect_test() is first called from init_gemdrvemul().
// Pure RAM lookup: never touches WiFi/cyw43, never blocks. A TNFS drive
// whose transport is TCP is skipped (TCP stays explicitly unsupported
// this phase) -- scanning continues to the next drive. If no usable
// drive is found at all (missing/invalid config, only SD drives, or only
// TCP-configured TNFS drives), the active-server state stays "not
// configured": every network-touching function below already has an
// existing "if (!ipaddr_aton(...)) { <graceful bail> }" check (there to
// handle a theoretical bad address, though the old hardcoded literal
// never actually exercised it) -- an empty host string makes every one of
// those checks take its already-safe failure path, so GEMDRIVE stays
// fully responsive with no blocking and no crash, falling back to the
// same no-network behavior as sidetnfs_mark_network_skipped().
void sidetnfs_probe_load_active_server(void);

// True if sidetnfs_probe_load_active_server() found and loaded a usable
// TNFS/UDP drive. False before that call, or if none was found.
bool sidetnfs_probe_has_active_server(void);

// The drive letter (uppercase ASCII) of the active TNFS drive loaded by
// sidetnfs_probe_load_active_server(). Only meaningful when
// sidetnfs_probe_has_active_server() is true; returns '\0' otherwise.
char sidetnfs_probe_get_active_drive_letter(void);

// Fase 1 (multi-drive slot routing, TNFS runtime context): mirrors
// gemdrvemul.c's GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES (9), duplicated
// as a literal rather than included -- sidetnfs_probe.h must not depend
// on gemdrvemul.h (the dependency already goes the other way). See
// report for the cross-check this relies on.
#define SIDETNFS_PROBE_MAX_RUNTIME_SLOTS 9

// Fase 1 (multi-drive slot routing): per-slot TNFS/backend identity --
// host/port/mount_path/sd_path/backend/transport, structurally mirrored
// from a slot's persisted sidetnfs_drive_config_t (see
// sidetnfs_probe_set_slot_context()). session_id/session_established are
// the one piece of state this struct does NOT get from the config record
// -- they only ever reflect a real, already-established TNFS session, set
// by sidetnfs_probe_mount_runtime_slots() sequentially mounting each
// valid slot (slot 0 via the existing single-session client state,
// slot 1 via its own, separate MOUNT request/response over the same
// PCB -- see that function). The underlying client still supports only
// one *concurrent* session (see sidetnfs_probe_mount_runtime_slots()'s
// own comment) -- slots 0 and 1 are mounted one after another, not at
// the same time, but each keeps its own resulting session id afterward.
// Slots 2.. are never populated in this phase.
typedef struct
{
    bool valid;                                    // true once sidetnfs_probe_set_slot_context() has populated this slot
    uint8_t backend_type;                           // sidetnfs_drive_type_t
    uint8_t transport;                               // sidetnfs_transport_t (TNFS only)
    char host[SIDETNFS_HOST_LEN];                    // TNFS only
    uint16_t port;                                   // TNFS only
    char mount_path[SIDETNFS_MOUNTPATH_LEN];         // TNFS only, e.g. "/Atari.ST" or "/DOS"
    char sd_path[SIDETNFS_SDPATH_LEN];               // SD only
    uint16_t session_id;                             // meaningful only when session_established
    bool session_established;                        // true once that slot's own MOUNT has actually succeeded
} sidetnfs_slot_tnfs_context_t;

// Populates slot `slot`'s host/port/mount_path/sd_path/backend/transport
// from *cfg (a full copy, not a reference -- *cfg may be a stack-local
// the caller reuses/frees right after this call). No network/TNFS
// activity of any kind -- pure data copy. Does not touch
// session_id/session_established (see sidetnfs_probe_get_slot_context()).
// No-op if slot is out of range or cfg is NULL.
void sidetnfs_probe_set_slot_context(int slot, const sidetnfs_drive_config_t *cfg);

// Fills *out with slot `slot`'s context; returns false (out untouched) if
// the slot is out of range or was never populated via
// sidetnfs_probe_set_slot_context(). session_id/session_established are
// overlaid read-only at call time -- for slot 0, from the existing
// single-session TNFS client state (s_state.sid/mount_response_received/
// mount_rc); for slot 1, from its own MOUNT response state, set by
// sidetnfs_probe_mount_runtime_slots(). Both are always the live value,
// never stale. Every slot 2.. always reports session_established ==
// false (never populated in this phase).
bool sidetnfs_probe_get_slot_context(int slot, sidetnfs_slot_tnfs_context_t *out);

// Fase 9E: re-activate the SideTNFS drive-list config at the proven
// Atari-reset boundary (GEMDRVEMUL_PING in gemdrvemul.c, guarded by
// sidetnfs_config_is_pending()) -- closes the old TNFS directory search
// state (both the DTA registry and the offline fallback table), resets
// mount/session identity (never the DEBUG.TXT counters), reloads the
// persistent config and active server/drive letter from it
// (sidetnfs_config_init() + sidetnfs_probe_load_active_server(), the same
// calls used at Pico cold boot -- one shared init path), then -- only if
// wifi_connected -- fires the same non-blocking mount probe used at cold
// boot. Never blocks: a failed/missing new mount just leaves
// sidetnfs_tnfs_listing_ready() false, same as any other offline boot.
void sidetnfs_probe_reinit_active_server(bool wifi_connected);

// Fase 9E: bulk-release every active TNFS DTA-registry search slot (each
// with a real CLOSEDIR, same as releaseTnfsDTA() does for one slot) --
// used by sidetnfs_probe_reinit_active_server() so no directory handle
// from the OLD server/session is left open across an Atari reset.
void sidetnfs_tnfs_dta_release_all(void);

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

// Fase 1 (multi-drive slot routing, TNFS mount sequencing): mounts every
// valid TNFS/UDP runtime slot strictly sequentially -- slot 0 (N:, via
// sidetnfs_send_mount_probe() above, unchanged) first, then, only after
// its response/timeout, slot 1 (O:, if valid) over the same PCB. Bounded
// (~200ms per slot, same proven timeout as the existing fs-listing wait)
// -- blocks the caller for at most ~400ms total, unlike
// sidetnfs_send_mount_probe()'s own fire-and-forget contract. Must only
// be called after WiFi is confirmed connected, and only once slot
// contexts have been populated via sidetnfs_probe_set_slot_context().
// Use this instead of sidetnfs_send_mount_probe() at Pico boot; reinit
// (sidetnfs_probe_reinit_active_server()) still uses
// sidetnfs_send_mount_probe() directly (slot 0 only, out of scope for
// this phase).
void sidetnfs_probe_mount_runtime_slots(void);

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

// Fase 8A introduced sidetnfs_note_tnfs_io()/sidetnfs_note_sd_io() and four
// I/O counters here as one-time backend-isolation review instrumentation.
// Fase 8C removed them: the isolation review is complete, its findings are
// now permanent code fixes (see gemdrvemul.c), and the review-only counters
// added no lasting production value over the new sd_available/
// sidetnfs_network_ok/sidetnfs_tnfs_listing_ready() status model. See
// report.

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

// Fase 5O/5Q (only meaningful for SIDETNFS_USE_TNFS_LISTING): result of
// one TNFS DTA-registry (or fake no-network) directory search step.
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

// Fase 5O/5Q (REMOVED in Fase 6B): the RAM directory-cache
// (sidetnfs_dir_cache_service()/is_ready()/request()/wait_ready() and
// sidetnfs_cache_search_start()) is gone -- see the comment above and
// SIDETNFS_PHASE5_DIRECTORY_LISTING.md. TNFS directory listing is served
// exclusively by sidetnfs_tnfs_dta_start()/next() below.

// Fase 5Q: start a fake, memory-only directory search for ndta -- used
// instead of a real TNFS search when TNFS was never available this boot
// (no WiFi, ESC-skip, or MOUNT never succeeded). NEVER touches cyw43/lwIP
// (which may already be torn down in this case, see Fase 5H). path == "/"
// yields exactly one synthetic entry, "NO_NETW.TXT" (a plain file, size 0,
// date/time 0/0 -- opening it is not expected to work yet, see report); any
// other path yields an immediately-empty listing (no entries, no error).
// Registers in its own fixed-size search-slot table (SIDETNFS_SEARCH_SLOTS
// concurrent searches for DIFFERENT ndta values), so
// sidetnfs_fake_search_next()/is_active()/close() work identically for
// fake and real (TNFS DTA-registry) searches -- GEMDRVEMUL_FSNEXT_CALL
// needs no awareness of which kind is active.
SidetnfsDirSearchResult sidetnfs_fake_search_start(uint32_t ndta, const char *path,
                                                     const char *pattern, uint8_t attribs,
                                                     SidetnfsAtariDirEntry *out_entry);

// Fase 5Y: start a TNFS directory search for ndta -- OPENDIRX for path,
// then a bounded READDIRX loop (SIDETNFS_READDIRX_MAX_ENTRIES entries per
// round, up to SIDETNFS_TNFS_DTA_MAX_ROUNDS rounds) until the first
// pattern+attribs match, EOF, or the round cap. No entry cache of any kind
// -- this is intentionally as close as possible to the SD/FatFS backend's
// f_findfirst()+insertDTA() contract (see report: the SD-baseline hardware
// test proved real GEMDRVEMUL_FSNEXT_CALL dispatch works once the search
// state is registered under ndta the same way SD's DTA hash table does it).
// Internally keyed by ndta in a small fixed-size registry
// (insertTnfsDTA()/lookupTnfsDTA()/releaseTnfsDTA(), SIDETNFS_TNFS_DTA_SLOTS
// slots) -- never a FatFS DIR*/FILINFO*, but the same "insert on Fsfirst,
// lookup on Fsnext" shape. Starting a new search for an ndta that already
// had one replaces it (mirrors the SD/FatFS backend's own behavior on a
// repeated Fsfirst -- see report: a repeated Fsfirst always starts fresh,
// the Fase 5U/5V repeat-continuation workaround was removed in Fase 6B).
// DOES touch the network on every call (bounded, cmd+seq validated) --
// unlike the removed cache path, there is no RAM copy to serve a hit from.
// On a terminal result (NOT_FOUND/ERROR) the ndta's registry entry is
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

// Fase 7D: TNFS file-op results. Mirrors the FOUND/NOT_FOUND/ERROR shape of
// SidetnfsDirSearchResult above -- NOT_FOUND lets the caller map to
// GEMDOS_EFILNF specifically (TNFS ENOENT), ERROR to a generic GEMDOS_EINTRN
// (any other wire error, protocol timeout, or send failure).
typedef enum
{
    SIDETNFS_FILE_OPEN_OK = 0,
    SIDETNFS_FILE_OPEN_NOT_FOUND,
    SIDETNFS_FILE_OPEN_ERROR
} SidetnfsFileOpenResult;

// Open tnfs_path over TNFS for GEMDOS Fopen. gemdos_mode is the raw Fopen
// mode (0=read-only, 1=write-only, 2=read/write); the caller must still
// deny anything outside 0-2 before ever calling this (see
// gemdrive_backend_fopen() in gemdrvemul.c). Never creates the file --
// Fase 7K, use sidetnfs_tnfs_file_create() below for Fcreate instead.
// Bounded wait, same contract as sidetnfs_tnfs_dta_start() -- never blocks
// indefinitely, never crashes on a wrong/unsupported opcode guess. On
// SIDETNFS_FILE_OPEN_OK, *out_handle is the TNFS-side file handle to use
// for subsequent read/write/close calls.
SidetnfsFileOpenResult sidetnfs_tnfs_file_open(const char *tnfs_path, uint16_t gemdos_mode, uint8_t *out_handle);

// Fase 7K: open tnfs_path over TNFS for GEMDOS Fcreate -- always
// create-if-missing + truncate-to-zero + read/write, matching the SD/FatFS
// route's own FA_READ|FA_WRITE|FA_CREATE_ALWAYS unconditionally (GEMDOS
// Fcreate never preserves existing content). Same bounded-wait contract as
// sidetnfs_tnfs_file_open(). On SIDETNFS_FILE_OPEN_OK, *out_handle is the
// TNFS-side file handle to use for subsequent write/close calls.
SidetnfsFileOpenResult sidetnfs_tnfs_file_create(const char *tnfs_path, uint8_t *out_handle);

// Read up to requested bytes (internally chunked and bounded -- see
// SIDETNFS_TNFS_READ_CHUNK_MAX in sidetnfs_probe.c) from tnfs_handle
// directly into out_buf (the caller's shared-memory read buffer -- no
// intermediate stack copy). guest_fd is only used for diagnostic logging.
// *out_actual receives the actual byte count (0 at EOF -- not an error).
// Returns false only on a genuine protocol error/timeout/unexpected wire
// error, never for EOF or a short read.
bool sidetnfs_tnfs_file_read(uint32_t guest_fd, uint8_t tnfs_handle, uint8_t *out_buf,
                              uint16_t requested, uint16_t *out_actual);

// Fase 7K: write up to requested bytes (internally chunked and bounded --
// see SIDETNFS_TNFS_WRITE_CHUNK_MAX in sidetnfs_probe.c) from data (the
// caller's shared-memory write buffer -- no intermediate stack copy of the
// chunk payload) to tnfs_handle. guest_fd is only used for diagnostic
// logging. Loops internally to fill up to `requested`, but -- unlike
// sidetnfs_tnfs_file_read()'s fill-the-whole-request contract -- stops
// immediately the first time the server accepts fewer bytes than a given
// internal chunk asked for (a genuine short/partial write, not an error):
// *out_actual reports whatever total was actually accepted, matching the
// SD/FatFS route's f_write() contract of reporting the real (possibly
// partial) byte count on success. Returns false only on a genuine protocol
// error/timeout/unexpected wire error, in which case *out_actual is
// whatever was accepted before the failing round (the caller must still
// treat the whole call as failed and not act on that count -- matching the
// SD/FatFS route, which also discards its own bytes_write on an FR_*
// error). *out_rc (nullable) receives the last raw TNFS wire rc byte seen
// (0xFF if a send/timeout failure occurred instead).
bool sidetnfs_tnfs_file_write(uint32_t guest_fd, uint8_t tnfs_handle, const uint8_t *data, uint16_t requested,
                               uint16_t *out_actual, uint8_t *out_rc);

// Send TNFS CLOSE for tnfs_handle and wait (bounded, same contract as
// tnfs_dta_closedir()). Always logs the outcome but never reports failure
// back to the caller -- per Fase 7D requirements, the local file descriptor
// must always be released regardless of whether the network close
// succeeded, so there is nothing meaningful for the caller to act on.
void sidetnfs_tnfs_file_close(uint32_t guest_fd, uint8_t tnfs_handle);

// Fase 7E: one-shot TNFS directory-existence probe for GEMDRVEMUL_DSETPATH_CALL
// -- OPENDIRX followed immediately by CLOSEDIR, never touching the TNFS
// DTA-registry (sidetnfs_tnfs_dta_start()/next()) or the fake no-network
// search table, so it can never collide with or pollute an in-progress
// Fsfirst/Fsnext for some other ndta. Returns true iff tnfs_path exists as
// a directory; *out_rc receives the raw TNFS wire rc byte (0xFF if no
// response was ever received) for logging/error-mapping upstream.
bool sidetnfs_tnfs_directory_exists(const char *tnfs_path, uint8_t *out_rc);

// Fase 7F: TNFS SEEK for an already-open file handle. seek_from_end==false
// always sends an absolute TNFS SEEK_SET request for `offset` (the caller
// -- gemdrvemul.c's GEMDRVEMUL_FSEEK_CALL TNFS branch -- has already done
// the GEMDOS SEEK_SET/SEEK_CUR arithmetic itself, mirroring the SD/RAM
// route's own file->offset math exactly); seek_from_end==true sends a real
// TNFS SEEK_END request and reads the resulting absolute position back
// from the response, since only the server knows the file size. On
// success, *out_new_offset is the new absolute offset (for SEEK_SET this
// simply echoes back what the caller asked for; for SEEK_END it comes from
// the server). Returns false on any send/timeout/wire error, or if a
// SEEK_END response doesn't carry the expected position field. out_rc
// (nullable, Fase 7F-debugfix) receives the raw TNFS wire rc byte (0xFF if
// no response was ever received) -- purely additive, for diagnostic
// counters upstream, does not affect the seek itself.
bool sidetnfs_tnfs_file_seek(uint32_t guest_fd, uint8_t tnfs_handle, bool seek_from_end,
                              int32_t offset, uint32_t *out_new_offset, uint8_t *out_rc);

// Fase 7G: TNFS delete results. Mirrors the SidetnfsFileOpenResult shape
// above -- keeps raw TNFS wire errno values (TNFS_ENOENT/TNFS_EACCES/
// TNFS_EISDIR, private to sidetnfs_probe.c) out of gemdrvemul.c, which only
// needs to pick a GEMDOS status from this small result set.
typedef enum
{
    SIDETNFS_FILE_DELETE_OK = 0,
    SIDETNFS_FILE_DELETE_NOT_FOUND,
    SIDETNFS_FILE_DELETE_ACCESS_DENIED,
    SIDETNFS_FILE_DELETE_ERROR
} SidetnfsFileDeleteResult;

// Fase 7G: TNFS UNLINK for a file (never a directory -- see
// sidetnfs_tnfs_file_delete()'s definition for how the server's own
// refusal to unlink a directory is handled). out_rc (nullable) receives
// the raw TNFS wire rc byte (0xFF if no response was ever received), for
// diagnostic logging/counters upstream (does not affect the result
// mapping itself).
SidetnfsFileDeleteResult sidetnfs_tnfs_file_delete(const char *tnfs_path, uint8_t *out_rc);

// Fase 7H: TNFS rename results. Same shape/reasoning as
// SidetnfsFileDeleteResult above. EXISTS is folded into ACCESS_DENIED (see
// sidetnfs_tnfs_file_rename()'s definition) -- kept as a single result
// value rather than a separate one, since GEMDOS has no dedicated
// "destination exists" error code to map it to differently anyway.
typedef enum
{
    SIDETNFS_FILE_RENAME_OK = 0,
    SIDETNFS_FILE_RENAME_NOT_FOUND,
    SIDETNFS_FILE_RENAME_ACCESS_DENIED,
    SIDETNFS_FILE_RENAME_ERROR
} SidetnfsFileRenameResult;

// Fase 7H: TNFS RENAME from old_path to new_path (both already-resolved
// TNFS-relative paths). Never called with a pre-check that the source or
// destination exists -- relies entirely on the server's own rename() rc,
// so a rename can never be reported as successful unless the server says
// so. out_rc (nullable) receives the raw TNFS wire rc byte (0xFF if no
// response was ever received), for diagnostic logging/counters upstream.
SidetnfsFileRenameResult sidetnfs_tnfs_file_rename(const char *old_path, const char *new_path, uint8_t *out_rc);

// Fase 7I: TNFS mkdir results. PATH_NOT_FOUND is unambiguous here (unlike
// the Fopen/Fdelete/Frename ENOENT cases) -- for Dcreate, ENOENT can only
// mean "a parent path component doesn't exist" (the target itself not
// existing is the expected precondition for creating it, not an error).
typedef enum
{
    SIDETNFS_DIR_CREATE_OK = 0,
    SIDETNFS_DIR_CREATE_PATH_NOT_FOUND,
    SIDETNFS_DIR_CREATE_ACCESS_DENIED,
    SIDETNFS_DIR_CREATE_ERROR
} SidetnfsDirCreateResult;

// Fase 7I: TNFS MKDIR for tnfs_path (already-resolved TNFS-relative path).
// Never called with a pre-check that the directory already exists or that
// its parent exists -- relies entirely on the server's own mkdir() rc, no
// preceding directory listing. out_rc (nullable) receives the raw TNFS
// wire rc byte (0xFF if no response was ever received), for diagnostic
// logging/counters upstream.
SidetnfsDirCreateResult sidetnfs_tnfs_directory_create(const char *tnfs_path, uint8_t *out_rc);

// Fase 7J: TNFS rmdir results. PATH_NOT_FOUND covers both "directory
// doesn't exist" (ENOENT) and "not a directory" (ENOTDIR, per the report's
// explicit EPTHNF mapping choice) -- ACCESS_DENIED covers EACCES and
// "directory not empty" (ENOTEMPTY).
typedef enum
{
    SIDETNFS_DIR_DELETE_OK = 0,
    SIDETNFS_DIR_DELETE_PATH_NOT_FOUND,
    SIDETNFS_DIR_DELETE_ACCESS_DENIED,
    SIDETNFS_DIR_DELETE_ERROR
} SidetnfsDirDeleteResult;

// Fase 7J: TNFS RMDIR for tnfs_path (already-resolved TNFS-relative path).
// Never called with a preceding directory-listing/enumeration to check
// whether the directory is empty -- relies entirely on the server's own
// rmdir() rc, so a delete can never be reported as successful unless the
// server actually performed it, and a non-empty directory is never
// silently emptied or force-removed. Never falls back to
// sidetnfs_tnfs_file_delete()/UNLINK. out_rc (nullable) receives the raw
// TNFS wire rc byte (0xFF if no response was ever received), for
// diagnostic logging/counters upstream.
SidetnfsDirDeleteResult sidetnfs_tnfs_directory_delete(const char *tnfs_path, uint8_t *out_rc);

// Fase 7L: TNFS attribute-query/set results for GEMDRVEMUL_FATTRIB_CALL.
// Mirrors the OK/NOT_FOUND/PATH_NOT_FOUND/ACCESS_DENIED/ERROR shape used
// elsewhere in this header -- keeps raw TNFS wire errno codes and the raw
// POSIX-style mode out of gemdrvemul.c, which only ever needs to pick a
// GEMDOS status and an FS_ST_* attribute byte from this result.
typedef enum
{
    SIDETNFS_ATTR_OK = 0,
    SIDETNFS_ATTR_NOT_FOUND,
    SIDETNFS_ATTR_PATH_NOT_FOUND,
    SIDETNFS_ATTR_ACCESS_DENIED,
    SIDETNFS_ATTR_ERROR
} SidetnfsAttrResult;

// Fase 7L: TNFS STAT -> GEMDOS/DOS-style (FS_ST_*) attributes, for Fattrib
// inquire (wflag 0). *out_st_attribs (on SIDETNFS_ATTR_OK) is an FS_ST_*
// bitmask -- only FS_ST_FOLDER and FS_ST_READONLY are ever set (see
// report: hidden/system/archive/label have no POSIX-mode equivalent this
// server exposes). out_rc (nullable) receives the raw TNFS wire rc byte
// (0xFF if no response was ever received).
SidetnfsAttrResult sidetnfs_tnfs_get_attributes(const char *tnfs_path, uint8_t *out_st_attribs, uint8_t *out_rc);

// Fase 7Lb: for Fattrib set (wflag 1). Confirmed against the actual server
// source (tnfsd 24.0522.1) that TNFS_CHMODFILE (0x27) is registered in its
// command-dispatch table but the handler function body is empty (no
// payload parse, no chmod(), no response) -- this function therefore never
// sends any wire request at all (no STAT, no CHMOD) and unconditionally
// reports the operation as unsupported: always returns
// SIDETNFS_ATTR_ACCESS_DENIED, always sets *out_unsupported (nullable) to
// true, always sets *out_result_attribs (nullable) to 0 -- the caller must
// map this to GEMDOS_EACCDN and must never surface *out_result_attribs as
// if anything were actually changed (no false success, no local attribute
// cache). requested_attribs/mask are only used for diagnostic logging.
// out_rc (nullable) is always set to 0xFF (no wire traffic, nothing to
// report).
SidetnfsAttrResult sidetnfs_tnfs_set_attributes(const char *tnfs_path, uint8_t requested_attribs, uint8_t mask,
                                                 uint8_t *out_result_attribs, uint8_t *out_rc, bool *out_unsupported);

// Fase 7M: TNFS STAT -> GEMDOS date/time, for Fdatime inquire (wflag 0).
// Reuses the same STAT request as sidetnfs_tnfs_get_attributes() above,
// reading mtime (confirmed at payload offset 0x0E) instead of mode.
// *out_gemdos_date/*out_gemdos_time (on SIDETNFS_ATTR_OK) are the FAT/
// GEMDOS-bit-layout date/time words, converted from the Unix mtime using
// this project's own local-time policy (see get_utc_offset_seconds() in
// rtcemul.h). *out_unix_mtime (nullable) is the raw value STAT reported.
// out_rc (nullable) receives the raw TNFS wire rc byte (0xFF if no
// response was ever received).
SidetnfsAttrResult sidetnfs_tnfs_get_datetime(const char *tnfs_path, uint16_t *out_gemdos_date,
                                               uint16_t *out_gemdos_time, uint32_t *out_unix_mtime, uint8_t *out_rc);

// Fase 7M: for Fdatime set (wflag 1). The actual server's protocol header
// defines no UTIME/SETTIME command at all (confirmed against the tnfsd
// 24.0522.1 source -- a stronger gap than Fattrib's CHMOD, which at least
// has a registered-but-empty opcode), so this function never sends any
// wire request -- it only logs the outcome (SIDETNFS_DIAG_FDATIME_SET_UNSUPPORTED).
// The caller (GEMDRVEMUL_FDATETIME_CALL in gemdrvemul.c) must map every
// TNFS Fdatime set to a GEMDOS error and never surface the requested
// date/time as if it were actually persisted, and must call
// sidetnfs_note_tnfs_fdatime() itself (this function does not).
void sidetnfs_tnfs_set_datetime_unsupported(const char *tnfs_path, uint16_t requested_date, uint16_t requested_time);

// Fase 7J: release any active TNFS DTA-registry search (see
// sidetnfs_tnfs_dta_start()/next() above) whose directory path exactly
// matches tnfs_path -- targeted, not a broad reset of all DTA slots.
// Fase 7J-correctie: no longer called by Ddelete (a still-open OPENDIRX
// handle on the target directory can make the server refuse RMDIR, so the
// close now has to happen -- and be confirmed -- *before* RMDIR is even
// attempted; see sidetnfs_tnfs_dta_close_by_path() below and the report).
// Left in place as a general-purpose targeted-release utility. No-op if no
// matching search is currently active.
void sidetnfs_tnfs_dta_release_by_path(const char *tnfs_path);

// Fase 7J-correctie: close (CLOSEDIR) every active TNFS DTA-registry
// search whose directory path exactly matches tnfs_path, confirming each
// CLOSEDIR actually succeeded before touching any local state -- unlike
// releaseTnfsDTA()/sidetnfs_tnfs_dta_release_by_path() (which always clear
// local state regardless of the server's CLOSEDIR response, matching the
// general "cleanup can't hang or retry" contract used elsewhere), this
// function must not silently assume success: RMDIR can only safely be
// attempted once every matching handle is confirmed closed.
// *out_matches receives how many active slots matched tnfs_path (0 if
// none -- nothing to close, safe to proceed). *out_close_rc receives the
// last raw TNFS wire rc seen for a CLOSEDIR attempt (0xFF if none were
// sent, or if a send/timeout failure occurred). Returns true iff every
// matching slot (if any) was successfully closed and released -- the
// caller (Ddelete) must not send RMDIR when this returns false.
bool sidetnfs_tnfs_dta_close_by_path(const char *tnfs_path, uint16_t *out_matches, uint8_t *out_close_rc);

// Fase 5O/6B: continue the active fake no-network search for ndta. Pure
// RAM scan -- no network, no wait, ever.
SidetnfsDirSearchResult sidetnfs_fake_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry);

// Fase 5O: true if ndta has a currently active fake no-network search.
bool sidetnfs_fake_search_is_active(uint32_t ndta);

// Fase 5O: explicitly close the active fake no-network search for ndta, if
// it is the one currently active (no-op otherwise).
void sidetnfs_fake_search_close(uint32_t ndta);

// Fase 9E: clear every active fake (no-network) directory search slot.
// Pure RAM, no network -- companion to sidetnfs_tnfs_dta_release_all(),
// used by sidetnfs_probe_reinit_active_server().
void sidetnfs_fake_search_close_all(void);

// Fase 5Z: number of currently-active fake no-network searches -- see
// sidetnfs_tnfs_dta_count_active() above.
uint16_t sidetnfs_fake_search_count_active(void);

// Fase 5N: record one successful/failed TNFS Fsfirst/Fsnext hit for the
// short DEBUG.TXT summary line (throttled/dirty-flag driven, like every
// other debug line -- never a per-entry write).
void sidetnfs_note_tnfs_fs_hit(void);
void sidetnfs_note_tnfs_fs_error(void);

// Fase 7F-debugfix: record one GEMDRVEMUL_FSEEK_CALL outcome for the
// DEBUG.TXT header counters (fseek calls/ok/errors/last mode/last fd/last
// rc) -- independent of the diagnostic eventlog's fixed-size budget, so
// these always reflect reality even once the eventlog itself is full.
void sidetnfs_note_tnfs_fseek(uint16_t mode, uint16_t fd, uint8_t rc, bool ok);

// Fase 7G: record one GEMDRVEMUL_FDELETE_CALL outcome for the DEBUG.TXT
// header counters (fdelete calls/ok/errors/last rc/last path) --
// independent of the diagnostic eventlog's fixed-size budget, same
// contract as sidetnfs_note_tnfs_fseek(). path is copied into a fixed,
// explicitly null-terminated buffer (may truncate a very long path -- this
// is a header summary, not a full record).
void sidetnfs_note_tnfs_fdelete(const char *path, uint8_t rc, bool ok);

// Fase 7H: record one GEMDRVEMUL_FRENAME_CALL outcome for the DEBUG.TXT
// header counters (frename calls/ok/errors/last rc/last old path/last new
// path) -- same contract as sidetnfs_note_tnfs_fdelete().
void sidetnfs_note_tnfs_frename(const char *old_path, const char *new_path, uint8_t rc, bool ok);

// Fase 7I: record one GEMDRVEMUL_DCREATE_CALL outcome for the DEBUG.TXT
// header counters (dcreate calls/ok/errors/last rc/last path) -- same
// contract as sidetnfs_note_tnfs_fdelete().
void sidetnfs_note_tnfs_dcreate(const char *path, uint8_t rc, bool ok);

// Fase 7J: record one GEMDRVEMUL_DDELETE_CALL outcome for the DEBUG.TXT
// header counters (ddelete calls/ok/errors/last rc/last path) -- same
// contract as sidetnfs_note_tnfs_dcreate().
void sidetnfs_note_tnfs_ddelete(const char *path, uint8_t rc, bool ok);

// Fase 7J-correctie: record one sidetnfs_tnfs_dta_close_by_path() outcome
// for the DEBUG.TXT header counters (ddelete dta matches/closed/close
// errors/last close rc). matches/closed/close_errors are added to the
// running totals; last_close_rc is a direct set of the most recent raw
// CLOSEDIR wire rc (0xFF if none was sent, e.g. no matching slot).
void sidetnfs_note_tnfs_ddelete_dta(uint16_t matches, uint16_t closed, uint16_t close_errors, uint8_t last_close_rc);

// Fase 7J-correctie-diag: record one GEMDRVEMUL_DDELETE_CALL outcome
// breakdown for the DEBUG.TXT header counters (ddelete cwd rejects/root
// rejects/rmdir attempts/last reject reason). cwd_reject/root_reject/
// rmdir_attempt each add at most 1 when true; reason (pass NULL to leave
// the stored reason untouched) overwrites the last-reject-reason string.
void sidetnfs_note_tnfs_ddelete_diag(bool cwd_reject, bool root_reject, bool rmdir_attempt, const char *reason);

// Fase 7J-correctie2: record one GEMDRVEMUL_DDELETE_CALL cwd-target-match/
// parent-update outcome for the DEBUG.TXT header counters (ddelete cwd
// target matches/parent updates/last cwd before/last cwd after).
// target_match/parent_update each add at most 1 when true; cwd_before/
// cwd_after (pass NULL to leave the corresponding stored string untouched)
// overwrite the last-cwd-before/-after strings.
void sidetnfs_note_tnfs_ddelete_cwd(bool target_match, bool parent_update, const char *cwd_before,
                                     const char *cwd_after);

// Fase 7K: record one GEMDRVEMUL_WRITE_BUFF_CALL outcome for the DEBUG.TXT
// header counters (fwrite calls/ok/errors/requested bytes/written bytes/
// partial writes/last handle/last requested/last written/last tnfs rc).
// requested/written accumulate into running totals; partial adds 1 to the
// partial-writes counter when true; the rest are direct sets.
void sidetnfs_note_tnfs_fwrite(uint8_t handle, uint16_t requested, uint16_t written, uint8_t rc, bool ok,
                                bool partial);

// Fase 7L: record one GEMDRVEMUL_FATTRIB_CALL outcome for the DEBUG.TXT
// header counters (fattrib calls/inquire calls/set calls/ok/errors/
// unsupported/last wflag/last requested/last returned/last tnfs rc/last
// path). wflag (0=inquire, 1=set) picks which sub-counter increments;
// unsupported adds 1 to the unsupported counter when true; the rest are
// direct sets/accumulate as documented on sidetnfs_note_tnfs_fwrite()
// above.
void sidetnfs_note_tnfs_fattrib(uint16_t wflag, uint8_t requested, uint8_t returned, uint8_t rc, bool ok,
                                 bool unsupported, const char *path);

// Fase 7M: record one GEMDRVEMUL_FDATETIME_CALL outcome for the DEBUG.TXT
// header counters (fdatime count/inquire count/set count/error count/
// unsupported count/last wflag/last handle/last path/last tnfs rc/last
// unix mtime/last gemdos date/last gemdos time). wflag (0=inquire, 1=set)
// picks which sub-counter increments; !ok adds 1 to the error counter;
// unsupported adds 1 to the unsupported counter; the rest are direct sets.
void sidetnfs_note_tnfs_fdatime(uint16_t wflag, uint8_t handle, const char *path, uint8_t rc, bool ok,
                                 bool unsupported, uint32_t unix_mtime, uint16_t gemdos_date, uint16_t gemdos_time);

// Fase 5S: RAM-only Fsfirst/Fsnext diagnostic eventlog. See
// sidetnfs_diag_log()/sidetnfs_diag_dump_on_select() below.
typedef enum
{
    SIDETNFS_DIAG_FSFIRST_ENTER = 0,
    SIDETNFS_DIAG_FSFIRST_ATTR_PREP,
    SIDETNFS_DIAG_FSFIRST_FOUND,
    SIDETNFS_DIAG_FSFIRST_NOT_FOUND,
    SIDETNFS_DIAG_FSFIRST_RETURN,
    SIDETNFS_DIAG_FSNEXT_ENTER,
    SIDETNFS_DIAG_FSNEXT_SEARCH_ACTIVE,
    SIDETNFS_DIAG_FSNEXT_SEARCH_MISSING,
    SIDETNFS_DIAG_FSNEXT_FOUND,
    SIDETNFS_DIAG_FSNEXT_END,
    SIDETNFS_DIAG_FSNEXT_RETURN,
    // Fase 6B: SIDETNFS_DIAG_SEARCH_OVERWRITE still used (fake no-network
    // search overwrite); the CACHE_* events above it (removed) were only
    // for the deleted RAM directory-cache.
    SIDETNFS_DIAG_SEARCH_OVERWRITE,
    SIDETNFS_DIAG_FAKE_SEARCH_START,
    SIDETNFS_DIAG_FAKE_FOUND,
    SIDETNFS_DIAG_FAKE_NOT_FOUND,
    // Fase 5U/6B: repeated-Fsfirst-as-continuation diagnostics REMOVED --
    // superseded by the real Fase 5Z fix (see report).
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
    // SIDETNFS_USE_SD_LISTING) purely as logging -- no
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
    // Fase 5AA: TNFS CLOSEDIR events -- always sent (see tnfs_dta_closedir()
    // in sidetnfs_probe.c). No separate TNFS_HANDLE_ALREADY_CLOSED event --
    // handle_valid and
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
    // Fase 7C: a mutating GEMDOS trap was denied at SIDETNFS_BACKEND_TNFS
    // (see report -- TNFS write/create/delete/rename isn't implemented
    // yet, so these traps must never fall through to a real FatFS/SD
    // mutation while the TNFS backend is active).
    SIDETNFS_DIAG_FCREATE_DENIED_TNFS,
    SIDETNFS_DIAG_FWRITE_DENIED_TNFS,
    SIDETNFS_DIAG_FDELETE_DENIED_TNFS,
    SIDETNFS_DIAG_FRENAME_DENIED_TNFS,
    SIDETNFS_DIAG_DCREATE_DENIED_TNFS,
    SIDETNFS_DIAG_DDELETE_DENIED_TNFS,
    SIDETNFS_DIAG_FATTRIB_SET_DENIED_TNFS,
    SIDETNFS_DIAG_FDATETIME_SET_DENIED_TNFS,
    // Fase 7D: real TNFS-backed Fopen(mode 0 only)/Fread/Fclose. ENTER/
    // DENY_MODE events are logged from gemdrvemul.c (routing/guest-side);
    // the TNFS_* wire-level events are logged from the new
    // sidetnfs_tnfs_file_open()/read()/close() functions in
    // sidetnfs_probe.c (mirrors the FSFIRST_*/TNFS_OPENDIRX* split above).
    SIDETNFS_DIAG_FOPEN_ENTER,
    SIDETNFS_DIAG_FOPEN_TNFS_OPEN,
    SIDETNFS_DIAG_FOPEN_TNFS_OK,
    SIDETNFS_DIAG_FOPEN_TNFS_DENY_MODE,
    SIDETNFS_DIAG_FOPEN_TNFS_ERROR,
    SIDETNFS_DIAG_FREAD_ENTER,
    SIDETNFS_DIAG_FREAD_TNFS_READ,
    SIDETNFS_DIAG_FREAD_TNFS_OK,
    SIDETNFS_DIAG_FREAD_TNFS_EOF,
    SIDETNFS_DIAG_FREAD_TNFS_ERROR,
    SIDETNFS_DIAG_FCLOSE_ENTER,
    SIDETNFS_DIAG_FCLOSE_TNFS_CLOSE,
    SIDETNFS_DIAG_FCLOSE_TNFS_OK,
    SIDETNFS_DIAG_FCLOSE_TNFS_ERROR,
    // Fase 7D-debug: exact-faaltrap diagnosis. GEMDRVEMUL_DSETPATH_CALL is
    // still unconditionally hard-SD (scfs_directory_exists()) -- these
    // events exist purely to observe whether/how it's involved, no
    // behavior change. The FOPEN_*/READ_BUFF_*/FCLOSE_* additions below
    // give a per-field trace (raw path -> internal path -> tnfs path ->
    // wire rc -> handle -> final GEMDOS return value) alongside the
    // higher-level Fase 7D events above -- purely additive, nothing above
    // is removed or renamed.
    SIDETNFS_DIAG_DSETPATH_ENTER,
    SIDETNFS_DIAG_DSETPATH_PATH_RAW,
    SIDETNFS_DIAG_DSETPATH_SD_CHECK,
    SIDETNFS_DIAG_DSETPATH_RETURN,
    SIDETNFS_DIAG_FOPEN_MODE,
    SIDETNFS_DIAG_FOPEN_RAW_PATH,
    SIDETNFS_DIAG_FOPEN_INTERNAL_PATH,
    SIDETNFS_DIAG_FOPEN_TNFS_PATH,
    SIDETNFS_DIAG_FOPEN_TNFS_RC,
    SIDETNFS_DIAG_FOPEN_TNFS_HANDLE,
    SIDETNFS_DIAG_FOPEN_RETURN,
    SIDETNFS_DIAG_READ_BUFF_ENTER,
    SIDETNFS_DIAG_READ_BUFF_HANDLE,
    SIDETNFS_DIAG_READ_BUFF_REQUESTED,
    SIDETNFS_DIAG_READ_BUFF_BACKEND,
    SIDETNFS_DIAG_READ_BUFF_TNFS_RC,
    SIDETNFS_DIAG_READ_BUFF_ACTUAL,
    SIDETNFS_DIAG_READ_BUFF_RETURN,
    SIDETNFS_DIAG_FCLOSE_HANDLE,
    SIDETNFS_DIAG_FCLOSE_BACKEND,
    SIDETNFS_DIAG_FCLOSE_TNFS_RC,
    SIDETNFS_DIAG_FCLOSE_RETURN,
    // Fase 7D5: local file->offset before/after a READ_BUFF_CALL, logged
    // once per call (not per internal TNFS-read round) -- the offset value
    // is formatted as text into the `path` field to avoid uint16_t
    // truncation for files >64KB.
    SIDETNFS_DIAG_READ_BUFF_OFFSET_BEFORE,
    SIDETNFS_DIAG_READ_BUFF_OFFSET_AFTER,
    // Fase 7E: Dsetpath/Dgetpath/current-directory backend-aware. The
    // TNFS_* events mirror the existing DSETPATH_SD_CHECK/RETURN shape but
    // for the real TNFS existence check (sidetnfs_tnfs_directory_exists())
    // that now actually determines BACKEND_TNFS's GEMDOS status, instead of
    // the (still separately logged, now informational-only)
    // scfs_directory_exists() SD check. PATH_RESOLVE_* covers the
    // relative-path + current-directory concatenation step in
    // GEMDRVEMUL_FSFIRST_CALL (which had no dedicated diagnostics of its
    // own before this phase) and, via PATH_RESOLVE_CWD only, the same step
    // inside get_tnfs_relative_pathname() for Fopen (whose raw/resolved
    // path was already covered by FOPEN_RAW_PATH/FOPEN_TNFS_PATH).
    SIDETNFS_DIAG_DSETPATH_TNFS_PATH,
    SIDETNFS_DIAG_DSETPATH_TNFS_EXISTS_RC,
    SIDETNFS_DIAG_DSETPATH_TNFS_CWD_SET,
    SIDETNFS_DIAG_DGETPATH_RETURN,
    SIDETNFS_DIAG_PATH_RESOLVE_INPUT,
    SIDETNFS_DIAG_PATH_RESOLVE_CWD,
    SIDETNFS_DIAG_PATH_RESOLVE_OUTPUT,
    // Fase 7F: TNFS Fseek. ENTER/HANDLE/MODE/OFFSET_IN/OFFSET_OUT/RETURN
    // are logged from gemdrvemul.c (GEMDRVEMUL_FSEEK_CALL's TNFS branch);
    // TNFS_SEEK/TNFS_RC are logged from sidetnfs_tnfs_file_seek() in
    // sidetnfs_probe.c (mirrors the FOPEN_*/FOPEN_TNFS_* split).
    SIDETNFS_DIAG_FSEEK_ENTER,
    SIDETNFS_DIAG_FSEEK_HANDLE,
    SIDETNFS_DIAG_FSEEK_MODE,
    SIDETNFS_DIAG_FSEEK_OFFSET_IN,
    SIDETNFS_DIAG_FSEEK_BACKEND,
    SIDETNFS_DIAG_FSEEK_TNFS_SEEK,
    SIDETNFS_DIAG_FSEEK_TNFS_RC,
    SIDETNFS_DIAG_FSEEK_OFFSET_OUT,
    SIDETNFS_DIAG_FSEEK_RETURN,
    // Fase 7G: TNFS Fdelete (files only -- Ddelete/directories untouched).
    // ENTER/RAW_PATH/TNFS_PATH/RETURN logged from gemdrvemul.c
    // (GEMDRVEMUL_FDELETE_CALL's TNFS branch); TNFS_UNLINK/TNFS_RC logged
    // from sidetnfs_tnfs_file_delete() in sidetnfs_probe.c (mirrors the
    // FSEEK_*/FSEEK_TNFS_* split). SIDETNFS_DIAG_FDELETE_DENIED_TNFS
    // (Fase 7C) is no longer reachable for the file-delete path but is
    // left defined -- see report.
    SIDETNFS_DIAG_FDELETE_ENTER,
    SIDETNFS_DIAG_FDELETE_RAW_PATH,
    SIDETNFS_DIAG_FDELETE_TNFS_PATH,
    SIDETNFS_DIAG_FDELETE_TNFS_UNLINK,
    SIDETNFS_DIAG_FDELETE_TNFS_RC,
    SIDETNFS_DIAG_FDELETE_RETURN,
    // Fase 7H: TNFS Frename (files, and directories when the server
    // allows it -- SidecarT/GEMDOS doesn't distinguish file/directory
    // rename at the trap level). ENTER/RAW_SRC/RAW_DST/TNFS_SRC/TNFS_DST/
    // HANDLE_UPDATE/RETURN logged from gemdrvemul.c
    // (GEMDRVEMUL_FRENAME_CALL's TNFS branch); TNFS_RENAME/TNFS_RC logged
    // from sidetnfs_tnfs_file_rename() in sidetnfs_probe.c (mirrors the
    // FSEEK_*/FDELETE_* split).
    SIDETNFS_DIAG_FRENAME_ENTER,
    SIDETNFS_DIAG_FRENAME_RAW_SRC,
    SIDETNFS_DIAG_FRENAME_RAW_DST,
    SIDETNFS_DIAG_FRENAME_TNFS_SRC,
    SIDETNFS_DIAG_FRENAME_TNFS_DST,
    SIDETNFS_DIAG_FRENAME_TNFS_RENAME,
    SIDETNFS_DIAG_FRENAME_TNFS_RC,
    SIDETNFS_DIAG_FRENAME_HANDLE_UPDATE,
    SIDETNFS_DIAG_FRENAME_RETURN,
    // Fase 7I: TNFS Dcreate. ENTER/RAW_PATH/TNFS_PATH/RETURN logged from
    // gemdrvemul.c (GEMDRVEMUL_DCREATE_CALL's TNFS branch); TNFS_MKDIR/
    // TNFS_RC logged from sidetnfs_tnfs_directory_create() in
    // sidetnfs_probe.c (mirrors the FDELETE_*/FRENAME_* split).
    SIDETNFS_DIAG_DCREATE_ENTER,
    SIDETNFS_DIAG_DCREATE_RAW_PATH,
    SIDETNFS_DIAG_DCREATE_TNFS_PATH,
    SIDETNFS_DIAG_DCREATE_TNFS_MKDIR,
    SIDETNFS_DIAG_DCREATE_TNFS_RC,
    SIDETNFS_DIAG_DCREATE_RETURN,
    // Fase 7J: TNFS Ddelete (single empty directory only -- never
    // recursive). ENTER/RAW_PATH/TNFS_PATH/CWD_CHECK/RETURN logged from
    // gemdrvemul.c (GEMDRVEMUL_DDELETE_CALL's TNFS branch); TNFS_RMDIR/
    // TNFS_RC logged from sidetnfs_tnfs_directory_delete() in
    // sidetnfs_probe.c (mirrors the DCREATE_*/FRENAME_* split).
    // SIDETNFS_DIAG_DDELETE_DTA_RELEASE is superseded by the Fase
    // 7J-correctie DTA_* events below (closing matching search handles
    // moved from after a successful RMDIR to before attempting it, so a
    // still-open OPENDIRX handle can never block the server's RMDIR) --
    // left defined, no longer emitted, see report.
    SIDETNFS_DIAG_DDELETE_ENTER,
    SIDETNFS_DIAG_DDELETE_RAW_PATH,
    SIDETNFS_DIAG_DDELETE_TNFS_PATH,
    SIDETNFS_DIAG_DDELETE_CWD_CHECK,
    SIDETNFS_DIAG_DDELETE_TNFS_RMDIR,
    SIDETNFS_DIAG_DDELETE_TNFS_RC,
    SIDETNFS_DIAG_DDELETE_DTA_RELEASE,
    SIDETNFS_DIAG_DDELETE_RETURN,
    // Fase 7J-correctie: pre-RMDIR targeted directory-search-handle close.
    // PRECHECK/RELEASE_BEFORE logged from gemdrvemul.c; MATCH/CLOSE/
    // CLOSE_RC logged from sidetnfs_tnfs_dta_close_by_path() in
    // sidetnfs_probe.c.
    SIDETNFS_DIAG_DDELETE_DTA_PRECHECK,
    SIDETNFS_DIAG_DDELETE_DTA_MATCH,
    SIDETNFS_DIAG_DDELETE_DTA_CLOSE,
    SIDETNFS_DIAG_DDELETE_DTA_CLOSE_RC,
    SIDETNFS_DIAG_DDELETE_DTA_RELEASE_BEFORE,
    // Fase 7J-correctie2: the local target-path == current-working-directory
    // reject was proven wrong by hardware evidence (Desktop routinely
    // Dsetpath's into a folder before deleting it) and is removed --
    // CWD_MATCH_ALLOWED marks that a Ddelete-of-cwd was let through to
    // RMDIR instead of being rejected. RMDIR_SENT/RMDIR_OK are
    // gemdrvemul.c-level bookends around the sidetnfs_tnfs_directory_delete()
    // call (mirrors the TNFS_RMDIR/TNFS_RC wire-level events already logged
    // from within that call in sidetnfs_probe.c). CWD_PARENT_UPDATE logged
    // only when dpath_string is actually rewritten to the parent directory,
    // which happens only after a confirmed TNFS_OK RMDIR of the directory
    // that was the CWD.
    SIDETNFS_DIAG_DDELETE_CWD_MATCH_ALLOWED,
    SIDETNFS_DIAG_DDELETE_RMDIR_SENT,
    SIDETNFS_DIAG_DDELETE_RMDIR_OK,
    SIDETNFS_DIAG_DDELETE_CWD_PARENT_UPDATE,
    // Fase 7K: TNFS Fwrite -- deliberately minimal (see report: the write
    // ACK handshake is timing-sensitive, so this phase logs only these 5
    // failure/partial events, no ENTER/RETURN/per-chunk detail). BAD_HANDLE/
    // READONLY logged from gemdrvemul.c (GEMDRVEMUL_WRITE_BUFF_CALL's TNFS
    // branch, local checks before ever contacting the server);
    // TRANSPORT_ERROR/SERVER_ERROR logged from sidetnfs_tnfs_file_write() in
    // sidetnfs_probe.c; PARTIAL logged from gemdrvemul.c after a successful
    // call that wrote fewer bytes than requested. SIDETNFS_DIAG_FWRITE_DENIED_TNFS
    // (above) is superseded -- TNFS Fwrite is implemented now -- left
    // defined, no longer emitted.
    SIDETNFS_DIAG_FWRITE_BAD_HANDLE,
    SIDETNFS_DIAG_FWRITE_READONLY,
    SIDETNFS_DIAG_FWRITE_TRANSPORT_ERROR,
    SIDETNFS_DIAG_FWRITE_SERVER_ERROR,
    SIDETNFS_DIAG_FWRITE_PARTIAL,
    // Fase 7L/7Lb: TNFS Fattrib -- deliberately minimal, same reasoning as
    // Fase 7K's Fwrite (compact counters, only failure/decision events, no
    // per-field detail). STAT_ERROR covers inquire (set no longer sends
    // STAT either, see Fase 7Lb). SET_UNSUPPORTED is now the *only* set
    // (wflag 1) event ever emitted -- confirmed against the actual server
    // source (tnfsd 24.0522.1) that its TNFS_CHMODFILE (0x27) handler is
    // an empty function body, so Fase 7Lb removed the CHMOD call entirely
    // and TNFS Fattrib set is unconditionally reported as unsupported.
    // SET_DENIED/SET_OK are kept defined (unreachable with this server,
    // for a hypothetical future server that implements CHMOD for real) but
    // no longer emitted. SIDETNFS_DIAG_FATTRIB_SET_DENIED_TNFS (above) is
    // also superseded -- left defined, no longer emitted.
    SIDETNFS_DIAG_FATTRIB_STAT_ERROR,
    SIDETNFS_DIAG_FATTRIB_SET_UNSUPPORTED,
    SIDETNFS_DIAG_FATTRIB_SET_DENIED,
    SIDETNFS_DIAG_FATTRIB_SET_ERROR,
    SIDETNFS_DIAG_FATTRIB_SET_OK,
    // Fase 7M: TNFS Fdatime -- same deliberately-minimal reasoning as
    // Fase 7L's Fattrib. INQUIRE_OK/INQUIRE_ERR logged from
    // sidetnfs_tnfs_get_datetime() in sidetnfs_probe.c. SET_UNSUPPORTED is
    // the only set (wflag 1) event ever emitted -- confirmed against the
    // actual server source (tnfsd 24.0522.1) that its protocol header
    // defines no UTIME/SETTIME command at all (a stronger gap than
    // Fattrib's CHMOD, which at least has a registered-but-empty opcode).
    // SET_OK/SET_ERR are kept defined (unreachable with this server, for a
    // hypothetical future server/protocol version that adds a real
    // set-time operation) but never emitted.
    SIDETNFS_DIAG_FDATIME_INQUIRE_OK,
    SIDETNFS_DIAG_FDATIME_INQUIRE_ERR,
    SIDETNFS_DIAG_FDATIME_SET_OK,
    SIDETNFS_DIAG_FDATIME_SET_ERR,
    SIDETNFS_DIAG_FDATIME_SET_UNSUPPORTED,
    // Fase 7N: GEMDOS Dfree under TNFS -- tnfsd 24.0522.1 has no free/total
    // disk space command (no SIZE/FREE/SIZEBYTES/FREEBYTES-equivalent, no
    // statvfs()/statfs() call anywhere in its source). No TNFS packet is
    // ever sent; fixed synthetic disk-size values are returned instead
    // (see GEMDRVEMUL_DFREE_CALL in gemdrvemul.c). Logged once per call,
    // purely informational -- Dfree still always reports GEMDOS_EOK.
    SIDETNFS_DIAG_DFREE_SYNTHETIC,
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

// Fase 5S: record one diagnostic event. Pass
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
