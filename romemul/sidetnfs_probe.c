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
#include "include/rtcemul.h"  // Fase 7M: get_utc_offset_seconds() -- same local-time policy as NTP->RTC
#include "include/sidetnfs_config.h" // Fase 9D: sidetnfs_config_get_drive() -- source of the active server

// Fase 9D: these used to be hardcoded compile-time constants. They are now
// runtime state, loaded once at boot from the first usable (used, TNFS,
// UDP) drive in the persistent sidetnfs_config drive-list (Fase 9C) by
// sidetnfs_probe_load_active_server(). A brief rollback to hardcoded
// defaults confirmed the wiring itself was correct -- the earlier
// NO_NETW.TXT symptom traced to a stale/incorrect IP left over in the
// saved flash config from an earlier bug, not to this code. Re-enabled
// once the user corrected and re-saved the config. s_active_host defaults
// to an empty string deliberately: every network-touching function below
// already has an existing "if (!ipaddr_aton(...)) { <graceful bail> }"
// check (needed in theory even when the old literal was hardcoded, though
// that branch was then unreachable) -- ipaddr_aton("") reliably fails, so
// with no active server configured every one of those existing checks
// takes its already-safe failure path, with no extra guards needed
// anywhere else in this file.
static char s_active_host[SIDETNFS_HOST_LEN] = "";
static uint16_t s_active_port = 0;
static char s_active_mount_path[SIDETNFS_MOUNTPATH_LEN] = "";
static char s_active_drive_letter = '\0';
static bool s_active_server_configured = false;

void sidetnfs_probe_load_active_server(void)
{
    uint8_t i;

    s_active_server_configured = false;
    s_active_host[0] = '\0';
    s_active_port = 0;
    s_active_mount_path[0] = '\0';
    s_active_drive_letter = '\0';

    for (i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_drive_config_t drive;
        sidetnfs_config_status_t status = sidetnfs_config_get_drive(i, &drive);

        if (status != SIDETNFS_CONFIG_STATUS_OK)
        {
            continue;
        }
        if (drive.type != SIDETNFS_DRIVE_TNFS)
        {
            continue;
        }
        if (drive.transport != SIDETNFS_TRANSPORT_UDP)
        {
            // TCP stays explicitly unsupported this phase -- keep
            // scanning for the next usable drive rather than adopting it.
            continue;
        }

        strncpy(s_active_host, drive.host, SIDETNFS_HOST_LEN - 1);
        s_active_host[SIDETNFS_HOST_LEN - 1] = '\0';
        s_active_port = drive.port;
        strncpy(s_active_mount_path, drive.mount_path, SIDETNFS_MOUNTPATH_LEN - 1);
        s_active_mount_path[SIDETNFS_MOUNTPATH_LEN - 1] = '\0';
        s_active_drive_letter = (char)drive.drive_letter;
        s_active_server_configured = true;
        break;
    }
}

bool sidetnfs_probe_has_active_server(void)
{
    return s_active_server_configured;
}

char sidetnfs_probe_get_active_drive_letter(void)
{
    return s_active_drive_letter;
}

// Fase 7F-debugfix: either focus mode suppresses the same per-entry
// directory-listing detail events (see the SIDETNFS_DEBUG_FOCUS_FSEEK
// comment in sidetnfs_probe.h) -- logging-only, no control-flow change.
#define SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL                                                                        \
    (SIDETNFS_DEBUG_FOCUS_FILE_IO || SIDETNFS_DEBUG_FOCUS_FSEEK || SIDETNFS_DEBUG_FOCUS_FDELETE ||                \
     SIDETNFS_DEBUG_FOCUS_FRENAME || SIDETNFS_DEBUG_FOCUS_DCREATE || SIDETNFS_DEBUG_FOCUS_DDELETE)

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
// Fase 7I: TNFS MKDIR -- same directory-op block as the hardware-confirmed
// CLOSEDIR(0x12)/OPENDIRX(0x17)/READDIRX(0x18) above (the published TNFS
// protocol groups OPENDIR/READDIR/CLOSEDIR/MKDIR/RMDIR/TELLDIR/SEEKDIR/
// OPENDIRX/READDIRX as 0x10-0x18); higher confidence than a fresh guess
// since 3 other opcodes from this same block are already hardware-verified,
// but MKDIR itself is not independently verified against this specific
// server. Wire request shape (header + null-terminated path, no other
// fields) mirrors TNFS_CMD_UNLINK.
#define TNFS_CMD_MKDIR 0x13u
// Fase 7J: TNFS RMDIR -- directly adjacent to MKDIR(0x13) in the same
// directory-op block referenced above; same confidence level. Wire request
// shape (header + null-terminated path, no other fields) mirrors
// TNFS_CMD_MKDIR/TNFS_CMD_UNLINK. Never used as a fallback for UNLINK or
// vice versa -- RMDIR only ever targets directories.
#define TNFS_CMD_RMDIR 0x14u
#define TNFS_PROTO_VER_MINOR 0x02
#define TNFS_PROTO_VER_MAJOR 0x01

// Fase 7D3: TNFS file-op opcodes -- OPEN/READ/CLOSE (WRITE added Fase 7K).
// Corrected against the actual TNFS server command-dispatch table
// (checked against server source this round, unlike the Fase 7D guess
// below): file ops sit in the 0x20-0x29 block, not directly after
// MOUNT/UMOUNT as originally (incorrectly) guessed.
//   TNFS_CMD_OPEN_OLD 0x20 -- deprecated open, not used here
//   TNFS_CMD_READ     0x21
//   TNFS_CMD_WRITE     0x22 -- Fase 7K
//   TNFS_CMD_CLOSE    0x23
//   TNFS_CMD_OPEN     0x29 -- current/non-deprecated open, used here since
//                             the existing OPEN request format (flags(2) +
//                             mode(2) + path) matches this variant, not the
//                             deprecated 0x20 one.
// Fase 7D's original guess (OPEN=0x02/READ=0x03/CLOSE=0x05) was wrong --
// the server was answering a different command (rc=22/EINVAL-shaped
// response on every open), not timing out, which is why it wasn't caught
// by the bounded-wait/timeout defense alone. Kept here for the record, not
// used anymore.
#define TNFS_CMD_OPEN_OLD 0x20u
#define TNFS_CMD_READ 0x21u
#define TNFS_CMD_WRITE 0x22u
#define TNFS_CMD_CLOSE 0x23u
// Fase 7F: TNFS_CMD_STAT/TNFS_CMD_SEEK come from the same verified
// server-side command-dispatch table that corrected OPEN/READ/CLOSE in
// Fase 7D3 (0x24/0x25, directly adjacent to the already hardware-confirmed
// 0x21/0x23/0x29) -- much higher confidence than the original Fase 7D
// opcode guess, though the wire request/response *shapes* below are still
// derived from the general published TNFS protocol family, not
// independently verified against this specific server. STAT was not sent
// until Fase 7L (SEEK_END uses the server-returned position instead of a
// separate STATFILE call, so it wasn't needed before Fattrib inquire
// needed real file/directory attributes).
#define TNFS_CMD_STAT 0x24u
#define TNFS_CMD_SEEK 0x25u
// Fase 7G: TNFS_CMD_UNLINK, same verified command-dispatch table as
// STAT/SEEK above (0x26, directly adjacent to the hardware-confirmed
// 0x25/0x29) -- same confidence level as Fase 7F's SEEK opcode. Wire
// request shape (header + null-terminated path, no extra fields) is
// derived from the general published TNFS protocol family, not
// independently verified against this specific server.
#define TNFS_CMD_UNLINK 0x26u
// Fase 7Lb: TNFS_CMD_CHMOD (TNFS_CHMODFILE, 0x27) is confirmed present in
// the actual server's command-dispatch table (FujiNetWIFI/tnfsd
// 24.0522.1, /opt/tnfsd/bin/tnfsd) -- but its handler is an empty function
// body:
//
//     void tnfs_chmod(Header *hdr, Session *s, unsigned char *buf, int bufsz)
//     {
//     }
//
// tnfsd 24.0522.1 registers command 0x27 but its tnfs_chmod() handler is
// empty and sends no response. TNFS Fattrib set is therefore reported as
// unsupported. STAT-based inquiry remains supported.
//
// It parses no payload, calls no chmod(), persists nothing, and sends no
// response at all -- so this opcode is defined here for documentation only
// and is deliberately never sent (see sidetnfs_tnfs_set_attributes()
// below, which reports SIDETNFS_ATTR_ACCESS_DENIED/unsupported without any
// wire traffic). Sending it would only ever produce a bounded-wait
// timeout, never a real response.
#define TNFS_CMD_CHMOD 0x27u
// Fase 7H: TNFS_CMD_RENAME, same verified command-dispatch table as
// UNLINK/STAT/SEEK above (0x28, directly adjacent to the hardware-confirmed
// 0x26/0x29) -- same confidence level as Fase 7F/7G's opcodes. Wire request
// shape (header + null-terminated old path + null-terminated new path) is
// derived from the general published TNFS protocol family, not
// independently verified against this specific server.
#define TNFS_CMD_RENAME 0x28u
#define TNFS_CMD_OPEN 0x29u

// TNFS OPEN flags (protocol-defined, OR-able bitmask -- not a POSIX
// O_RDONLY/O_WRONLY/O_RDWR single-value triad). TNFS_OPEN_RDONLY (the
// "read" bit) is the only one hardware-verified so far (every Fopen mode 0
// this codebase has ever sent). TNFS_OPEN_WRITE/CREAT/TRUNC (Fase 7K) come
// from the same general published TNFS protocol family as every other
// opcode/flag in this file -- not independently verified against this
// specific server yet; a wrong guess fails safely (bounded-wait timeout or
// a server error rc), never a hang or crash, same as every other
// not-yet-verified opcode here.
#define TNFS_OPEN_RDONLY 0x0001u
#define TNFS_OPEN_WRITE 0x0002u
#define TNFS_OPEN_CREAT 0x0100u
#define TNFS_OPEN_TRUNC 0x0200u

// TNFS SEEK whence values -- same numbering as POSIX lseek()/GEMDOS Fseek
// (0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END). Only SET and END are actually sent
// (see sidetnfs_tnfs_file_seek(): SEEK_CUR is always translated to an
// absolute SEEK_SET locally, so the server's own SEEK_CUR semantics are
// never depended on).
#define TNFS_SEEK_SET 0u
#define TNFS_SEEK_END 2u

// TNFS wire error codes are POSIX errno values truncated to a byte.
#define TNFS_ENOENT 2u
#define TNFS_EACCES 13u
#define TNFS_EEXIST 17u
#define TNFS_ENOTDIR 20u
#define TNFS_EISDIR 21u
#define TNFS_ENOTEMPTY 39u

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

// Fase 7D: max payload bytes requested per single TNFS READ wire
// round-trip. Bounded well under SIDETNFS_RX_BUF_SIZE (256) so a full
// response (header+rc+size+data) always fits s_fslisting_resp.buf with room
// to spare, and well under a typical UDP MTU.
//
// Fase 7D5 correction: this used to also be the effective per-
// GEMDRVEMUL_READ_BUFF_CALL limit (one TNFS READ per guest call) -- that
// was wrong. The SD/FatFS route's f_read() fills the *entire* requested
// buff_size (up to DEFAULT_FOPEN_READ_BUFFER_SIZE=16384) in one guest call,
// short only at real EOF; a copy operation that requests a large buff_size
// per call was silently getting back only SIDETNFS_TNFS_READ_CHUNK_MAX
// bytes with the rest of the shared buffer left stale/zeroed, corrupting
// the copy without ever raising a GEMDOS error (see report). This constant
// still bounds each individual wire round-trip; sidetnfs_tnfs_file_read()
// below now loops internally (bounded by SIDETNFS_TNFS_READ_MAX_ROUNDS) to
// fill up to the full `requested` amount, matching f_read()'s contract.
#define SIDETNFS_TNFS_READ_CHUNK_MAX 200u

// Fase 7D5: hard cap on internal TNFS READ round-trips per single
// sidetnfs_tnfs_file_read() call. ceil(DEFAULT_FOPEN_READ_BUFFER_SIZE /
// SIDETNFS_TNFS_READ_CHUNK_MAX) = ceil(16384/200) = 82 covers the largest
// legitimate single-call fill with comfortable margin; a pathological
// server that never signals EOF still cannot turn this into an unbounded
// loop.
#define SIDETNFS_TNFS_READ_MAX_ROUNDS 128u

// Fase 7K: max payload bytes sent per single TNFS WRITE wire round-trip.
// Same value and same reasoning as SIDETNFS_TNFS_READ_CHUNK_MAX -- unlike a
// READ request (fixed ~7-byte request, response carries the data), a WRITE
// *request* itself carries the data, so this bounds the outbound pbuf size
// (7-byte header + chunk) well under a typical UDP MTU and comfortably
// within a single non-fragmenting packet. sidetnfs_tnfs_file_write() below
// loops internally (bounded by SIDETNFS_TNFS_WRITE_MAX_ROUNDS) to fill up
// to the full `requested` amount, matching f_write()'s per-call contract,
// but -- unlike read -- stops immediately on the first short/partial
// server-accepted chunk rather than continuing (see report: avoids masking
// a genuine partial-write condition, e.g. a full disk, behind further
// chunks that would silently pad the guest's byte count).
#define SIDETNFS_TNFS_WRITE_CHUNK_MAX 200u

// Fase 7K: hard cap on internal TNFS WRITE round-trips per single
// sidetnfs_tnfs_file_write() call. ceil(DEFAULT_FWRITE_BUFFER_SIZE /
// SIDETNFS_TNFS_WRITE_CHUNK_MAX) = ceil(2048/200) = 11 covers the largest
// legitimate single-call fill (one GEMDRVEMUL_WRITE_BUFF_CALL never asks
// for more than DEFAULT_FWRITE_BUFFER_SIZE bytes) with comfortable margin.
#define SIDETNFS_TNFS_WRITE_MAX_ROUNDS 16u

// Fase 5L: small, fixed test set of GEMDOS-style patterns counted against
// each successfully normalized READDIRX entry during the existing root
// scan. Kept intentionally short (<=5) per the debug-line budget.
#define SIDETNFS_MATCH_PATTERN_COUNT 5
static const char *const SIDETNFS_MATCH_PATTERNS[SIDETNFS_MATCH_PATTERN_COUNT] = {
    "*.*", "*", "*.PRG", "*.ACC", "CONFIG.*"};

// Fase 5Q: number of concurrent fake no-network searches (one per active
// ndta). Root-cause analysis of "root always truncated to 1 entry" (see
// report) found that GEM/TOS issues more than one concurrent Fsfirst/Fsnext
// sequence around a single folder-window refresh (e.g. an icon-layout .INF
// lookup interleaved with the real listing) -- a single shared search slot
// let one clobber the other. 4 slots comfortably covers root + a couple of
// subdirectory/auxiliary lookups at once.
#define SIDETNFS_SEARCH_SLOTS 4u

// Fase 5R: bounded wait for a single OPENDIRX/READDIRX/CLOSEDIR round-trip
// in the TNFS DTA-registry path. Worst case per round:
// SIDETNFS_FS_WAIT_MAX_ITER * SIDETNFS_FS_WAIT_STEP_US = 200ms.
#define SIDETNFS_FS_WAIT_MAX_ITER 200
#define SIDETNFS_FS_WAIT_STEP_US 1000

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

    // Fase 7D4: compact file-I/O counters, incremented from
    // sidetnfs_tnfs_file_open()/read()/close() -- cheap, RAM-only, no
    // control-flow change, just visibility for the DEBUG.TXT header.
    uint16_t tnfs_fopen_calls;
    uint16_t tnfs_fopen_ok;
    uint16_t tnfs_fread_calls;
    uint32_t tnfs_fread_bytes;
    uint16_t tnfs_fclose_calls;

    // Fase 7F-debugfix: Fseek counters, incremented via
    // sidetnfs_note_tnfs_fseek() from GEMDRVEMUL_FSEEK_CALL in
    // gemdrvemul.c -- independent of the (fixed-size,
    // stop-when-full) diagnostic eventlog, so the DEBUG.TXT header always
    // shows an accurate Fseek call count even once the eventlog itself is
    // full.
    uint16_t tnfs_fseek_calls;
    uint16_t tnfs_fseek_ok;
    uint16_t tnfs_fseek_errors;
    uint16_t tnfs_fseek_last_mode;
    uint16_t tnfs_fseek_last_fd;
    uint8_t tnfs_fseek_last_rc;

    // Fase 7G: Fdelete counters, incremented via sidetnfs_note_tnfs_fdelete()
    // from GEMDRVEMUL_FDELETE_CALL -- same "independent of the eventlog
    // budget" contract as the Fase 7F-debugfix Fseek counters above.
    uint16_t tnfs_fdelete_calls;
    uint16_t tnfs_fdelete_ok;
    uint16_t tnfs_fdelete_errors;
    uint8_t tnfs_fdelete_last_rc;
    char tnfs_fdelete_last_path[32];

    // Fase 7H: Frename counters, incremented via sidetnfs_note_tnfs_frename()
    // from GEMDRVEMUL_FRENAME_CALL -- same contract as the Fdelete counters
    // above.
    uint16_t tnfs_frename_calls;
    uint16_t tnfs_frename_ok;
    uint16_t tnfs_frename_errors;
    uint8_t tnfs_frename_last_rc;
    char tnfs_frename_last_old_path[32];
    char tnfs_frename_last_new_path[32];

    // Fase 7I: Dcreate counters, incremented via sidetnfs_note_tnfs_dcreate()
    // from GEMDRVEMUL_DCREATE_CALL -- same contract as the Fdelete counters
    // above.
    uint16_t tnfs_dcreate_calls;
    uint16_t tnfs_dcreate_ok;
    uint16_t tnfs_dcreate_errors;
    uint8_t tnfs_dcreate_last_rc;
    char tnfs_dcreate_last_path[32];

    // Fase 7J: Ddelete counters, incremented via sidetnfs_note_tnfs_ddelete()
    // from GEMDRVEMUL_DDELETE_CALL -- same contract as the Dcreate counters
    // above.
    uint16_t tnfs_ddelete_calls;
    uint16_t tnfs_ddelete_ok;
    uint16_t tnfs_ddelete_errors;
    uint8_t tnfs_ddelete_last_rc;
    char tnfs_ddelete_last_path[32];

    // Fase 7J-correctie: pre-RMDIR targeted DTA-search-handle close
    // counters, incremented via sidetnfs_note_tnfs_ddelete_dta() from
    // sidetnfs_tnfs_dta_close_by_path(). matches/closed/close_errors are
    // cumulative across all Ddelete calls; last_close_rc is the most
    // recent raw CLOSEDIR wire rc seen (0xFF if none sent, e.g. no match).
    uint16_t tnfs_ddelete_dta_matches;
    uint16_t tnfs_ddelete_dta_closed;
    uint16_t tnfs_ddelete_dta_close_errors;
    uint8_t tnfs_ddelete_last_close_rc;

    // Fase 7J-correctie-diag: extra Ddelete outcome breakdown requested
    // after the correctie's own counters/events failed to show up in a
    // hardware test (see report) -- lets a single DEBUG.TXT distinguish a
    // cwd/root-denied call from a DTA-close abort from an actual RMDIR
    // attempt, without needing the eventlog to have survived intact.
    // Incremented via sidetnfs_note_tnfs_ddelete_diag().
    uint16_t tnfs_ddelete_cwd_rejects;
    uint16_t tnfs_ddelete_root_rejects;
    uint16_t tnfs_ddelete_rmdir_attempts;
    char tnfs_ddelete_last_reject_reason[16];

    // Fase 7J-correctie2: the local cwd-equals-target reject was removed
    // (hardware evidence showed Desktop routinely Dsetpath's into a folder
    // it's about to delete, so tnfs_ddelete_cwd_rejects must now stay 0).
    // These track the replacement behavior instead: how often the target
    // matched the current TNFS CWD, and how often dpath_string was actually
    // rewritten to the parent directory afterward (only on a confirmed
    // TNFS_OK RMDIR). Incremented via sidetnfs_note_tnfs_ddelete_cwd().
    uint16_t tnfs_ddelete_cwd_target_matches;
    uint16_t tnfs_ddelete_cwd_parent_updates;
    char tnfs_ddelete_last_cwd_before[32];
    char tnfs_ddelete_last_cwd_after[32];

    // Fase 7K: Fwrite counters, incremented via sidetnfs_note_tnfs_fwrite()
    // from GEMDRVEMUL_WRITE_BUFF_CALL -- one call to sidetnfs_note_tnfs_fwrite()
    // per guest WRITE_BUFF_CALL (not per internal wire round-trip), matching
    // this phase's compact/call-level-only diagnostics requirement.
    uint16_t tnfs_fwrite_calls;
    uint16_t tnfs_fwrite_ok;
    uint16_t tnfs_fwrite_errors;
    uint32_t tnfs_fwrite_requested_bytes;
    uint32_t tnfs_fwrite_written_bytes;
    uint16_t tnfs_fwrite_partial_writes;
    uint8_t tnfs_fwrite_last_handle;
    uint16_t tnfs_fwrite_last_requested;
    uint16_t tnfs_fwrite_last_written;
    uint8_t tnfs_fwrite_last_rc;

    // Fase 7L: Fattrib counters, incremented via sidetnfs_note_tnfs_fattrib()
    // from GEMDRVEMUL_FATTRIB_CALL -- one call per guest FATTRIB_CALL,
    // matching this phase's compact/call-level-only diagnostics
    // requirement (same style as Fase 7K's Fwrite counters).
    uint16_t tnfs_fattrib_calls;
    uint16_t tnfs_fattrib_inquire_calls;
    uint16_t tnfs_fattrib_set_calls;
    uint16_t tnfs_fattrib_ok;
    uint16_t tnfs_fattrib_errors;
    uint16_t tnfs_fattrib_unsupported;
    uint8_t tnfs_fattrib_last_wflag;
    uint8_t tnfs_fattrib_last_requested;
    uint8_t tnfs_fattrib_last_returned;
    uint8_t tnfs_fattrib_last_rc;
    char tnfs_fattrib_last_path[32];

    // Fase 7M: Fdatime counters, incremented via sidetnfs_note_tnfs_fdatime()
    // from GEMDRVEMUL_FDATETIME_CALL -- one call per guest FDATETIME_CALL,
    // matching the compact/call-level-only diagnostics style already used
    // for Fase 7K/7L/7Lb.
    uint16_t tnfs_fdatime_count;
    uint16_t tnfs_fdatime_inquire_count;
    uint16_t tnfs_fdatime_set_count;
    uint16_t tnfs_fdatime_error_count;
    uint16_t tnfs_fdatime_unsupported_count;
    uint8_t tnfs_fdatime_last_wflag;
    uint8_t tnfs_fdatime_last_handle;
    char tnfs_fdatime_last_path[32];
    uint8_t tnfs_fdatime_last_tnfs_rc;
    uint32_t tnfs_fdatime_last_unix_mtime;
    uint16_t tnfs_fdatime_last_gemdos_date;
    uint16_t tnfs_fdatime_last_gemdos_time;

#if SIDETNFS_DEBUG_SHOW_RAW
    uint16_t last_response_len;
    uint8_t last_raw[SIDETNFS_DEBUG_RAW_SIZE];
#endif
} SidetnfsDebugState;

static SidetnfsDebugState s_state = {0};
static uint8_t s_readdirx_seq = 2; // MOUNT uses 0, OPENDIRX uses 1

// Fase 1 (multi-drive slot routing): the per-slot TNFS/backend identity
// table -- see sidetnfs_slot_tnfs_context_t's own comment in
// sidetnfs_probe.h. Populated exclusively by sidetnfs_probe_set_slot_context()
// below; nothing else in this file writes to it.
static sidetnfs_slot_tnfs_context_t s_slot_contexts[SIDETNFS_PROBE_MAX_RUNTIME_SLOTS];

// Fase 1 (multi-drive slot routing, TNFS mount sequencing): slot 1's own
// MOUNT response state, the direct analogue of
// s_state.sid/mount_rc/mount_response_received for slot 0 -- kept
// separate from s_state (which remains exclusively slot 0's, unchanged)
// so a failed/slow slot 1 mount can never perturb slot 0's own state.
// Written only by tnfs_recv_callback()'s pending-slot-1 branch and
// send_slot1_mount_request() (both further down in this file); read by
// sidetnfs_probe_get_slot_context() below.
static uint16_t s_slot1_mount_sid = 0;
static uint8_t s_slot1_mount_rc = 0xFF; // 0xFF: no response yet (not a real TNFS rc)
static bool s_slot1_mount_response_received = false;

// Fase 2 (mount pending-slot fix): explicit single-outstanding-MOUNT
// tracker. -1 means "no MOUNT currently outstanding" -- deliberately NOT
// 0, since slot 0 is itself a valid slot index and a zero-initialized
// "0 means none" sentinel would make slot 0's own pending window
// indistinguishable from "nothing pending" at static-init time. Set to
// 0/1 by sidetnfs_send_mount_probe()/send_slot1_mount_request()
// respectively, BEFORE the packet is actually sent (so a very fast
// response can never race ahead of us marking it pending); cleared back
// to -1 by tnfs_recv_callback() on an accepted response, and by
// sidetnfs_probe_mount_runtime_slots() after each bounded wait
// completes (covers the timeout case, so a late straggler arriving after
// the wait gave up is never attributed to whichever slot is mounted
// next).
//
// Replaces the previous, protocol-non-conformant design that used a
// hardcoded, non-zero MOUNT sequence number (0x40) to tell slot 1's
// response apart from slot 0's. Root cause (see report): on the actual
// production TNFS server this Pico talks to, a MOUNT response's echoed
// sequence byte could not be relied on to distinguish slot 1's request
// from slot 0's the way a synthetic local tnfsd test suggested -- slot
// 0 and slot 1 mount the same server (only mount_path differs), so a
// slot 1 response landing with the same address/port as slot 0's own
// expected reply, while carrying an unexpected seq value, fell through
// the old seq==0-gated slot-0 branch entirely and was silently dropped
// by neither branch matching -- or, in the reverse case, could have been
// wrongly captured by slot 0's own seq==0 gate. This pending-slot design
// no longer depends on the received sequence byte to attribute a
// response at all -- only on which single slot is currently the one
// outstanding MOUNT, plus a defensive address+port match against that
// slot's own configured server.
static int s_mount_pending_slot = -1;

void sidetnfs_probe_set_slot_context(int slot, const sidetnfs_drive_config_t *cfg)
{
    if (slot < 0 || slot >= SIDETNFS_PROBE_MAX_RUNTIME_SLOTS || cfg == NULL)
    {
        return;
    }

    sidetnfs_slot_tnfs_context_t *ctx = &s_slot_contexts[slot];
    memset(ctx, 0, sizeof(*ctx));

    ctx->backend_type = cfg->type;
    ctx->transport = cfg->transport;
    strncpy(ctx->host, cfg->host, sizeof(ctx->host) - 1);
    ctx->port = cfg->port;
    strncpy(ctx->mount_path, cfg->mount_path, sizeof(ctx->mount_path) - 1);
    strncpy(ctx->sd_path, cfg->sd_path, sizeof(ctx->sd_path) - 1);
    // session_id/session_established stay 0/false here -- see
    // sidetnfs_probe_get_slot_context() for the one place slot 0's real
    // session state is ever reflected.
    ctx->valid = true;
}

bool sidetnfs_probe_get_slot_context(int slot, sidetnfs_slot_tnfs_context_t *out)
{
    if (slot < 0 || slot >= SIDETNFS_PROBE_MAX_RUNTIME_SLOTS || out == NULL)
    {
        return false;
    }
    if (!s_slot_contexts[slot].valid)
    {
        return false;
    }

    *out = s_slot_contexts[slot];

    if (slot == 0)
    {
        // Fase 1: slot 0's real session state -- read live from the
        // existing single-session TNFS client fields, never mutating
        // s_slot_contexts itself. Same "mounted successfully" condition
        // already used elsewhere in this file (e.g. sidetnfs_tnfs_listing_ready()).
        out->session_id = s_state.sid;
        out->session_established = s_state.mount_response_received && (s_state.mount_rc == TNFS_OK);
    }
    else if (slot == 1)
    {
        // Fase 1 (multi-drive slot routing, TNFS mount sequencing):
        // slot 1's own real session state, set by
        // sidetnfs_probe_mount_runtime_slots()'s sequential MOUNT of
        // slot 1 -- read live, same shape as slot 0 above. No longer
        // forced false.
        out->session_id = s_slot1_mount_sid;
        out->session_established = s_slot1_mount_response_received && (s_slot1_mount_rc == TNFS_OK);
    }
    // Every other slot: session_id/session_established stay at
    // s_slot_contexts[slot]'s own values (0/false) -- never populated by
    // anything in this phase.

    return true;
}

// Fase 7D4: suppress back-to-back TNFS_READDIRX_EOF events for the same
// ndta -- repeated fresh directory scans (Desktop refresh, repeated
// Fsfirst) each end in one EOF event, and these were crowding out the
// fixed-size (SIDETNFS_DIAG_MAX_EVENTS) event log before the interesting
// file-I/O events ever got recorded. Purely a logging suppression -- does
// not touch search->eof or any other control flow. The first EOF for a
// given ndta (or the first EOF after a *different* ndta's EOF) still logs.
static uint32_t s_last_readdirx_eof_ndta = 0;
static bool s_last_readdirx_eof_ndta_valid = false;

// Fase 5O/6B: the RAM directory cache that used to live here (per-path
// cache slots, a network-build state machine, root pre-cache warmup) has
// been removed entirely -- see SIDETNFS_PHASE5_DIRECTORY_LISTING.md and the
// TNFS DTA-registry declarations further down (sidetnfs_tnfs_dta_start()/
// next()), which are now the only TNFS directory-listing path.

// Fase 5Q/6B: fixed-size search-slot table for the fake, memory-only
// no-network listing (see sidetnfs_fake_search_start()) -- one slot per
// concurrently-active ndta (see report -- this is what actually fixed "root
// always truncated to 1 entry": GEM/TOS runs more than one Fsfirst/Fsnext
// sequence around a single folder refresh). Used only when TNFS was never
// available this boot; real TNFS searches use the separate
// SidetnfsTnfsDtaSearch registry below.
typedef struct
{
    bool active;
    uint32_t ndta;
    char path[MAX_FOLDER_LENGTH];
    char pattern[13];
    uint8_t attribs;
    uint16_t next_index;
} SidetnfsFakeSearchSlot;

static SidetnfsFakeSearchSlot s_fake_searches[SIDETNFS_SEARCH_SLOTS] = {0};

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
    // Fase 1 (multi-drive slot routing): the runtime slot this search's
    // OPENDIRX/READDIRX/CLOSEDIR traffic belongs to -- set once by
    // insertTnfsDTA() (from GEMDRVEMUL_FSFIRST_CALL's already-validated
    // slot), then read back by tnfs_dta_find_next_match()/
    // releaseTnfsDTA()/tnfs_dta_closedir() to resolve the right
    // session_id/host/port via sidetnfs_probe_get_slot_context(). Named
    // "runtime_slot", not "slot", to avoid colliding with the existing
    // `SidetnfsTnfsDtaSearch *slot` local-variable convention used
    // throughout this file.
    int runtime_slot;
} SidetnfsTnfsDtaSearch;

static SidetnfsTnfsDtaSearch s_tnfs_dta_searches[SIDETNFS_TNFS_DTA_SLOTS] = {0};

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
    // Fase 7F-debugfix: strncpy() does not null-terminate the destination
    // when the source is >= the copy length -- the initial e->x[0]='\0'
    // above only covers the NULL-source case, not a too-long source. Made
    // explicit here defensively; purely a safety hardening, no behavior
    // change for any source string that already fit.
    e->path[0] = '\0';
    if (path != NULL)
    {
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }
    e->pattern[0] = '\0';
    if (pattern != NULL)
    {
        strncpy(e->pattern, pattern, sizeof(e->pattern) - 1);
        e->pattern[sizeof(e->pattern) - 1] = '\0';
    }
    e->name[0] = '\0';
    if (name != NULL)
    {
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
    }
    s_diag_event_count++;
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
    case SIDETNFS_DIAG_FAKE_SEARCH_START:
        return "FAKE_SEARCH_START";
    case SIDETNFS_DIAG_FAKE_FOUND:
        return "FAKE_FOUND";
    case SIDETNFS_DIAG_FAKE_NOT_FOUND:
        return "FAKE_NOT_FOUND";
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
    case SIDETNFS_DIAG_FCREATE_DENIED_TNFS:
        return "FCREATE_DENIED_TNFS";
    case SIDETNFS_DIAG_FWRITE_DENIED_TNFS:
        return "FWRITE_DENIED_TNFS";
    case SIDETNFS_DIAG_FDELETE_DENIED_TNFS:
        return "FDELETE_DENIED_TNFS";
    case SIDETNFS_DIAG_FRENAME_DENIED_TNFS:
        return "FRENAME_DENIED_TNFS";
    case SIDETNFS_DIAG_DCREATE_DENIED_TNFS:
        return "DCREATE_DENIED_TNFS";
    case SIDETNFS_DIAG_DDELETE_DENIED_TNFS:
        return "DDELETE_DENIED_TNFS";
    case SIDETNFS_DIAG_FATTRIB_SET_DENIED_TNFS:
        return "FATTRIB_SET_DENIED_TNFS";
    case SIDETNFS_DIAG_FDATETIME_SET_DENIED_TNFS:
        return "FDATETIME_SET_DENIED_TNFS";
    case SIDETNFS_DIAG_FOPEN_ENTER:
        return "FOPEN_ENTER";
    case SIDETNFS_DIAG_FOPEN_TNFS_OPEN:
        return "FOPEN_TNFS_OPEN";
    case SIDETNFS_DIAG_FOPEN_TNFS_OK:
        return "FOPEN_TNFS_OK";
    case SIDETNFS_DIAG_FOPEN_TNFS_DENY_MODE:
        return "FOPEN_TNFS_DENY_MODE";
    case SIDETNFS_DIAG_FOPEN_TNFS_ERROR:
        return "FOPEN_TNFS_ERROR";
    case SIDETNFS_DIAG_FREAD_ENTER:
        return "FREAD_ENTER";
    case SIDETNFS_DIAG_FREAD_TNFS_READ:
        return "FREAD_TNFS_READ";
    case SIDETNFS_DIAG_FREAD_TNFS_OK:
        return "FREAD_TNFS_OK";
    case SIDETNFS_DIAG_FREAD_TNFS_EOF:
        return "FREAD_TNFS_EOF";
    case SIDETNFS_DIAG_FREAD_TNFS_ERROR:
        return "FREAD_TNFS_ERROR";
    case SIDETNFS_DIAG_FCLOSE_ENTER:
        return "FCLOSE_ENTER";
    case SIDETNFS_DIAG_FCLOSE_TNFS_CLOSE:
        return "FCLOSE_TNFS_CLOSE";
    case SIDETNFS_DIAG_FCLOSE_TNFS_OK:
        return "FCLOSE_TNFS_OK";
    case SIDETNFS_DIAG_FCLOSE_TNFS_ERROR:
        return "FCLOSE_TNFS_ERROR";
    case SIDETNFS_DIAG_DSETPATH_ENTER:
        return "DSETPATH_ENTER";
    case SIDETNFS_DIAG_DSETPATH_PATH_RAW:
        return "DSETPATH_PATH_RAW";
    case SIDETNFS_DIAG_DSETPATH_SD_CHECK:
        return "DSETPATH_SD_CHECK";
    case SIDETNFS_DIAG_DSETPATH_RETURN:
        return "DSETPATH_RETURN";
    case SIDETNFS_DIAG_FOPEN_MODE:
        return "FOPEN_MODE";
    case SIDETNFS_DIAG_FOPEN_RAW_PATH:
        return "FOPEN_RAW_PATH";
    case SIDETNFS_DIAG_FOPEN_INTERNAL_PATH:
        return "FOPEN_INTERNAL_PATH";
    case SIDETNFS_DIAG_FOPEN_TNFS_PATH:
        return "FOPEN_TNFS_PATH";
    case SIDETNFS_DIAG_FOPEN_TNFS_RC:
        return "FOPEN_TNFS_RC";
    case SIDETNFS_DIAG_FOPEN_TNFS_HANDLE:
        return "FOPEN_TNFS_HANDLE";
    case SIDETNFS_DIAG_FOPEN_RETURN:
        return "FOPEN_RETURN";
    case SIDETNFS_DIAG_READ_BUFF_ENTER:
        return "READ_BUFF_ENTER";
    case SIDETNFS_DIAG_READ_BUFF_HANDLE:
        return "READ_BUFF_HANDLE";
    case SIDETNFS_DIAG_READ_BUFF_REQUESTED:
        return "READ_BUFF_REQUESTED";
    case SIDETNFS_DIAG_READ_BUFF_BACKEND:
        return "READ_BUFF_BACKEND";
    case SIDETNFS_DIAG_READ_BUFF_TNFS_RC:
        return "READ_BUFF_TNFS_RC";
    case SIDETNFS_DIAG_READ_BUFF_ACTUAL:
        return "READ_BUFF_ACTUAL";
    case SIDETNFS_DIAG_READ_BUFF_RETURN:
        return "READ_BUFF_RETURN";
    case SIDETNFS_DIAG_FCLOSE_HANDLE:
        return "FCLOSE_HANDLE";
    case SIDETNFS_DIAG_FCLOSE_BACKEND:
        return "FCLOSE_BACKEND";
    case SIDETNFS_DIAG_FCLOSE_TNFS_RC:
        return "FCLOSE_TNFS_RC";
    case SIDETNFS_DIAG_FCLOSE_RETURN:
        return "FCLOSE_RETURN";
    case SIDETNFS_DIAG_READ_BUFF_OFFSET_BEFORE:
        return "READ_BUFF_OFFSET_BEFORE";
    case SIDETNFS_DIAG_READ_BUFF_OFFSET_AFTER:
        return "READ_BUFF_OFFSET_AFTER";
    case SIDETNFS_DIAG_DSETPATH_TNFS_PATH:
        return "DSETPATH_TNFS_PATH";
    case SIDETNFS_DIAG_DSETPATH_TNFS_EXISTS_RC:
        return "DSETPATH_TNFS_EXISTS_RC";
    case SIDETNFS_DIAG_DSETPATH_TNFS_CWD_SET:
        return "DSETPATH_TNFS_CWD_SET";
    case SIDETNFS_DIAG_DGETPATH_RETURN:
        return "DGETPATH_RETURN";
    case SIDETNFS_DIAG_PATH_RESOLVE_INPUT:
        return "PATH_RESOLVE_INPUT";
    case SIDETNFS_DIAG_PATH_RESOLVE_CWD:
        return "PATH_RESOLVE_CWD";
    case SIDETNFS_DIAG_PATH_RESOLVE_OUTPUT:
        return "PATH_RESOLVE_OUTPUT";
    case SIDETNFS_DIAG_FSEEK_ENTER:
        return "FSEEK_ENTER";
    case SIDETNFS_DIAG_FSEEK_HANDLE:
        return "FSEEK_HANDLE";
    case SIDETNFS_DIAG_FSEEK_MODE:
        return "FSEEK_MODE";
    case SIDETNFS_DIAG_FSEEK_OFFSET_IN:
        return "FSEEK_OFFSET_IN";
    case SIDETNFS_DIAG_FSEEK_BACKEND:
        return "FSEEK_BACKEND";
    case SIDETNFS_DIAG_FSEEK_TNFS_SEEK:
        return "FSEEK_TNFS_SEEK";
    case SIDETNFS_DIAG_FSEEK_TNFS_RC:
        return "FSEEK_TNFS_RC";
    case SIDETNFS_DIAG_FSEEK_OFFSET_OUT:
        return "FSEEK_OFFSET_OUT";
    case SIDETNFS_DIAG_FSEEK_RETURN:
        return "FSEEK_RETURN";
    case SIDETNFS_DIAG_FDELETE_ENTER:
        return "FDELETE_ENTER";
    case SIDETNFS_DIAG_FDELETE_RAW_PATH:
        return "FDELETE_RAW_PATH";
    case SIDETNFS_DIAG_FDELETE_TNFS_PATH:
        return "FDELETE_TNFS_PATH";
    case SIDETNFS_DIAG_FDELETE_TNFS_UNLINK:
        return "FDELETE_TNFS_UNLINK";
    case SIDETNFS_DIAG_FDELETE_TNFS_RC:
        return "FDELETE_TNFS_RC";
    case SIDETNFS_DIAG_FDELETE_RETURN:
        return "FDELETE_RETURN";
    case SIDETNFS_DIAG_FRENAME_ENTER:
        return "FRENAME_ENTER";
    case SIDETNFS_DIAG_FRENAME_RAW_SRC:
        return "FRENAME_RAW_SRC";
    case SIDETNFS_DIAG_FRENAME_RAW_DST:
        return "FRENAME_RAW_DST";
    case SIDETNFS_DIAG_FRENAME_TNFS_SRC:
        return "FRENAME_TNFS_SRC";
    case SIDETNFS_DIAG_FRENAME_TNFS_DST:
        return "FRENAME_TNFS_DST";
    case SIDETNFS_DIAG_FRENAME_TNFS_RENAME:
        return "FRENAME_TNFS_RENAME";
    case SIDETNFS_DIAG_FRENAME_TNFS_RC:
        return "FRENAME_TNFS_RC";
    case SIDETNFS_DIAG_FRENAME_HANDLE_UPDATE:
        return "FRENAME_HANDLE_UPDATE";
    case SIDETNFS_DIAG_FRENAME_RETURN:
        return "FRENAME_RETURN";
    case SIDETNFS_DIAG_DCREATE_ENTER:
        return "DCREATE_ENTER";
    case SIDETNFS_DIAG_DCREATE_RAW_PATH:
        return "DCREATE_RAW_PATH";
    case SIDETNFS_DIAG_DCREATE_TNFS_PATH:
        return "DCREATE_TNFS_PATH";
    case SIDETNFS_DIAG_DCREATE_TNFS_MKDIR:
        return "DCREATE_TNFS_MKDIR";
    case SIDETNFS_DIAG_DCREATE_TNFS_RC:
        return "DCREATE_TNFS_RC";
    case SIDETNFS_DIAG_DCREATE_RETURN:
        return "DCREATE_RETURN";
    case SIDETNFS_DIAG_DDELETE_ENTER:
        return "DDELETE_ENTER";
    case SIDETNFS_DIAG_DDELETE_RAW_PATH:
        return "DDELETE_RAW_PATH";
    case SIDETNFS_DIAG_DDELETE_TNFS_PATH:
        return "DDELETE_TNFS_PATH";
    case SIDETNFS_DIAG_DDELETE_CWD_CHECK:
        return "DDELETE_CWD_CHECK";
    case SIDETNFS_DIAG_DDELETE_TNFS_RMDIR:
        return "DDELETE_TNFS_RMDIR";
    case SIDETNFS_DIAG_DDELETE_TNFS_RC:
        return "DDELETE_TNFS_RC";
    case SIDETNFS_DIAG_DDELETE_DTA_RELEASE:
        return "DDELETE_DTA_RELEASE";
    case SIDETNFS_DIAG_DDELETE_RETURN:
        return "DDELETE_RETURN";
    case SIDETNFS_DIAG_DDELETE_DTA_PRECHECK:
        return "DDELETE_DTA_PRECHECK";
    case SIDETNFS_DIAG_DDELETE_DTA_MATCH:
        return "DDELETE_DTA_MATCH";
    case SIDETNFS_DIAG_DDELETE_DTA_CLOSE:
        return "DDELETE_DTA_CLOSE";
    case SIDETNFS_DIAG_DDELETE_DTA_CLOSE_RC:
        return "DDELETE_DTA_CLOSE_RC";
    case SIDETNFS_DIAG_DDELETE_DTA_RELEASE_BEFORE:
        return "DDELETE_DTA_RELEASE_BEFORE";
    case SIDETNFS_DIAG_DDELETE_CWD_MATCH_ALLOWED:
        return "DDELETE_CWD_MATCH_ALLOWED";
    case SIDETNFS_DIAG_DDELETE_RMDIR_SENT:
        return "DDELETE_RMDIR_SENT";
    case SIDETNFS_DIAG_DDELETE_RMDIR_OK:
        return "DDELETE_RMDIR_OK";
    case SIDETNFS_DIAG_DDELETE_CWD_PARENT_UPDATE:
        return "DDELETE_CWD_PARENT_UPDATE";
    case SIDETNFS_DIAG_FWRITE_BAD_HANDLE:
        return "FWRITE_BAD_HANDLE";
    case SIDETNFS_DIAG_FWRITE_READONLY:
        return "FWRITE_READONLY";
    case SIDETNFS_DIAG_FWRITE_TRANSPORT_ERROR:
        return "FWRITE_TRANSPORT_ERROR";
    case SIDETNFS_DIAG_FWRITE_SERVER_ERROR:
        return "FWRITE_SERVER_ERROR";
    case SIDETNFS_DIAG_FWRITE_PARTIAL:
        return "FWRITE_PARTIAL";
    case SIDETNFS_DIAG_FATTRIB_STAT_ERROR:
        return "FATTRIB_STAT_ERROR";
    case SIDETNFS_DIAG_FATTRIB_SET_UNSUPPORTED:
        return "FATTRIB_SET_UNSUPPORTED";
    case SIDETNFS_DIAG_FATTRIB_SET_DENIED:
        return "FATTRIB_SET_DENIED";
    case SIDETNFS_DIAG_FATTRIB_SET_ERROR:
        return "FATTRIB_SET_ERROR";
    case SIDETNFS_DIAG_FATTRIB_SET_OK:
        return "FATTRIB_SET_OK";
    case SIDETNFS_DIAG_FDATIME_INQUIRE_OK:
        return "FDATIME_INQUIRE_OK";
    case SIDETNFS_DIAG_FDATIME_INQUIRE_ERR:
        return "FDATIME_INQUIRE_ERR";
    case SIDETNFS_DIAG_FDATIME_SET_OK:
        return "FDATIME_SET_OK";
    case SIDETNFS_DIAG_FDATIME_SET_ERR:
        return "FDATIME_SET_ERR";
    case SIDETNFS_DIAG_FDATIME_SET_UNSUPPORTED:
        return "FDATIME_SET_UNSUPPORTED";
    case SIDETNFS_DIAG_DFREE_SYNTHETIC:
        return "DFREE_SYNTHETIC";
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
    case GEMDRVEMUL_READ_BUFF_CALL:
        return "READ_BUFF";
    case GEMDRVEMUL_PEXEC_CALL:
        return "PEXEC";
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
    // Fase 7J-correctie-diag: unambiguous build marker -- lets a hardware
    // test immediately confirm the DEBUG.TXT being reviewed actually came
    // from this diagnostic build, rather than a stale copy from an earlier
    // phase (see report: the previous test's DEBUG.TXT showed none of the
    // Fase 7J-correctie counters/events at all, which is only possible if
    // it wasn't generated by this build).
    int len = snprintf(line, sizeof(line), "debug build: 7J-DDELETE-DIAG-2\r\n");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line),
                        "SIDETNFS FS DIAG\r\n"
                        "backend: %s\r\n"
                        "events: %u\r\n",
#if SIDETNFS_USE_TNFS_LISTING
                        "TNFS",
#else
                        "SD",
#endif
                        (unsigned)s_diag_event_count);
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
    // Fase 5AA/6B: how many TNFS DTA-registry slots are active vs. how many
    // still have an unclosed OPENDIRX handle -- if handles-open ever grows
    // without bound across repeated Fsfirst/refresh cycles, that's a leak;
    // it should track active closely (0 or 1 higher, briefly, between
    // OPENDIRX success and the next release).
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
                    "fsnext case reached: %s\r\n"
                    "tnfs dta slots: %u\r\n"
                    "tnfs dta active: %u\r\n"
                    "tnfs handles open: %u\r\n"
                    "readdirx max entries: %u\r\n"
                    "closedir enabled: %s\r\n"
                    "file io: TNFS enabled\r\n"
                    "tnfs file cmds: open=0x%02x read=0x%02x close=0x%02x\r\n",
                    fsnext_ever_reached ? "YES" : "NO", (unsigned)SIDETNFS_TNFS_DTA_SLOTS, tnfs_active,
                    tnfs_handles_open, (unsigned)SIDETNFS_READDIRX_MAX_ENTRIES, "YES",
                    (unsigned)TNFS_CMD_OPEN, (unsigned)TNFS_CMD_READ, (unsigned)TNFS_CMD_CLOSE);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7D4: compact file-I/O counters (see SidetnfsDebugState comment).
    len = snprintf(line, sizeof(line),
                    "fopen calls: %u\r\n"
                    "fopen ok: %u\r\n"
                    "fread calls: %u\r\n"
                    "fread bytes: %lu\r\n"
                    "fclose calls: %u\r\n",
                    (unsigned)s_state.tnfs_fopen_calls, (unsigned)s_state.tnfs_fopen_ok,
                    (unsigned)s_state.tnfs_fread_calls, (unsigned long)s_state.tnfs_fread_bytes,
                    (unsigned)s_state.tnfs_fclose_calls);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7F-debugfix: Fseek counters -- independent of the eventlog
    // budget, see SidetnfsDebugState comment.
    len = snprintf(line, sizeof(line),
                    "fseek calls: %u\r\n"
                    "fseek ok: %u\r\n"
                    "fseek errors: %u\r\n"
                    "fseek last mode: %u\r\n"
                    "fseek last fd: %u\r\n"
                    "fseek last rc: %u\r\n\r\n",
                    (unsigned)s_state.tnfs_fseek_calls, (unsigned)s_state.tnfs_fseek_ok,
                    (unsigned)s_state.tnfs_fseek_errors, (unsigned)s_state.tnfs_fseek_last_mode,
                    (unsigned)s_state.tnfs_fseek_last_fd, (unsigned)s_state.tnfs_fseek_last_rc);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7G: Fdelete counters -- independent of the eventlog budget, see
    // SidetnfsDebugState comment.
    len = snprintf(line, sizeof(line),
                    "fdelete calls: %u\r\n"
                    "fdelete ok: %u\r\n"
                    "fdelete errors: %u\r\n"
                    "fdelete last rc: %u\r\n"
                    "fdelete last path: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_fdelete_calls, (unsigned)s_state.tnfs_fdelete_ok,
                    (unsigned)s_state.tnfs_fdelete_errors, (unsigned)s_state.tnfs_fdelete_last_rc,
                    s_state.tnfs_fdelete_last_path);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7H: Frename counters -- independent of the eventlog budget, see
    // SidetnfsDebugState comment.
    len = snprintf(line, sizeof(line),
                    "frename calls: %u\r\n"
                    "frename ok: %u\r\n"
                    "frename errors: %u\r\n"
                    "frename last rc: %u\r\n"
                    "frename last old path: %s\r\n"
                    "frename last new path: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_frename_calls, (unsigned)s_state.tnfs_frename_ok,
                    (unsigned)s_state.tnfs_frename_errors, (unsigned)s_state.tnfs_frename_last_rc,
                    s_state.tnfs_frename_last_old_path, s_state.tnfs_frename_last_new_path);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7I: Dcreate counters -- independent of the eventlog budget, see
    // SidetnfsDebugState comment.
    len = snprintf(line, sizeof(line),
                    "dcreate calls: %u\r\n"
                    "dcreate ok: %u\r\n"
                    "dcreate errors: %u\r\n"
                    "dcreate last rc: %u\r\n"
                    "dcreate last path: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_dcreate_calls, (unsigned)s_state.tnfs_dcreate_ok,
                    (unsigned)s_state.tnfs_dcreate_errors, (unsigned)s_state.tnfs_dcreate_last_rc,
                    s_state.tnfs_dcreate_last_path);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7J: Ddelete counters -- independent of the eventlog budget, see
    // SidetnfsDebugState comment.
    len = snprintf(line, sizeof(line),
                    "ddelete calls: %u\r\n"
                    "ddelete ok: %u\r\n"
                    "ddelete errors: %u\r\n"
                    "ddelete last rc: %u\r\n"
                    "ddelete last path: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_ddelete_calls, (unsigned)s_state.tnfs_ddelete_ok,
                    (unsigned)s_state.tnfs_ddelete_errors, (unsigned)s_state.tnfs_ddelete_last_rc,
                    s_state.tnfs_ddelete_last_path);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7J-correctie: pre-RMDIR targeted DTA-close counters -- independent
    // of the eventlog budget, see SidetnfsDebugState comment.
    len = snprintf(line, sizeof(line),
                    "ddelete dta matches: %u\r\n"
                    "ddelete dta closed: %u\r\n"
                    "ddelete dta close errors: %u\r\n"
                    "ddelete last close rc: %u\r\n\r\n",
                    (unsigned)s_state.tnfs_ddelete_dta_matches, (unsigned)s_state.tnfs_ddelete_dta_closed,
                    (unsigned)s_state.tnfs_ddelete_dta_close_errors, (unsigned)s_state.tnfs_ddelete_last_close_rc);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7J-correctie-diag: extra Ddelete outcome breakdown -- always
    // printed, even when every value is zero, so a fresh DEBUG.TXT is
    // unambiguous about whether this firmware even attempted a Ddelete.
    len = snprintf(line, sizeof(line),
                    "ddelete cwd rejects: %u\r\n"
                    "ddelete root rejects: %u\r\n"
                    "ddelete rmdir attempts: %u\r\n"
                    "ddelete last reject reason: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_ddelete_cwd_rejects, (unsigned)s_state.tnfs_ddelete_root_rejects,
                    (unsigned)s_state.tnfs_ddelete_rmdir_attempts, s_state.tnfs_ddelete_last_reject_reason);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7J-correctie2: cwd-target-match/parent-update counters -- always
    // printed, even when every value is zero/empty.
    len = snprintf(line, sizeof(line),
                    "ddelete cwd target matches: %u\r\n"
                    "ddelete cwd parent updates: %u\r\n"
                    "ddelete last cwd before: %s\r\n"
                    "ddelete last cwd after: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_ddelete_cwd_target_matches, (unsigned)s_state.tnfs_ddelete_cwd_parent_updates,
                    s_state.tnfs_ddelete_last_cwd_before, s_state.tnfs_ddelete_last_cwd_after);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7K: Fwrite counters -- always printed, even when every value is
    // zero, independent of the eventlog budget (this phase logs almost no
    // events at all for Fwrite, see report -- these counters are the
    // primary diagnostic).
    len = snprintf(line, sizeof(line),
                    "fwrite calls: %u\r\n"
                    "fwrite ok: %u\r\n"
                    "fwrite errors: %u\r\n"
                    "fwrite requested bytes: %lu\r\n"
                    "fwrite written bytes: %lu\r\n"
                    "fwrite partial writes: %u\r\n"
                    "fwrite last handle: %u\r\n"
                    "fwrite last requested: %u\r\n"
                    "fwrite last written: %u\r\n"
                    "fwrite last tnfs rc: %u\r\n\r\n",
                    (unsigned)s_state.tnfs_fwrite_calls, (unsigned)s_state.tnfs_fwrite_ok,
                    (unsigned)s_state.tnfs_fwrite_errors, (unsigned long)s_state.tnfs_fwrite_requested_bytes,
                    (unsigned long)s_state.tnfs_fwrite_written_bytes, (unsigned)s_state.tnfs_fwrite_partial_writes,
                    (unsigned)s_state.tnfs_fwrite_last_handle, (unsigned)s_state.tnfs_fwrite_last_requested,
                    (unsigned)s_state.tnfs_fwrite_last_written, (unsigned)s_state.tnfs_fwrite_last_rc);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7L: Fattrib counters -- always printed, even when every value is
    // zero, independent of the eventlog budget (same compact-diagnostics
    // style as Fase 7K's Fwrite counters above).
    len = snprintf(line, sizeof(line),
                    "fattrib calls: %u\r\n"
                    "fattrib inquire calls: %u\r\n"
                    "fattrib set calls: %u\r\n"
                    "fattrib ok: %u\r\n"
                    "fattrib errors: %u\r\n"
                    "fattrib unsupported: %u\r\n"
                    "fattrib last wflag: %u\r\n"
                    "fattrib last requested: %u\r\n"
                    "fattrib last returned: %u\r\n"
                    "fattrib last tnfs rc: %u\r\n"
                    "fattrib last path: %s\r\n\r\n",
                    (unsigned)s_state.tnfs_fattrib_calls, (unsigned)s_state.tnfs_fattrib_inquire_calls,
                    (unsigned)s_state.tnfs_fattrib_set_calls, (unsigned)s_state.tnfs_fattrib_ok,
                    (unsigned)s_state.tnfs_fattrib_errors, (unsigned)s_state.tnfs_fattrib_unsupported,
                    (unsigned)s_state.tnfs_fattrib_last_wflag, (unsigned)s_state.tnfs_fattrib_last_requested,
                    (unsigned)s_state.tnfs_fattrib_last_returned, (unsigned)s_state.tnfs_fattrib_last_rc,
                    s_state.tnfs_fattrib_last_path);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    // Fase 7M: Fdatime counters -- always printed, even when every value is
    // zero, independent of the eventlog budget (same compact-diagnostics
    // style as the Fattrib counters above).
    len = snprintf(line, sizeof(line),
                    "fdatime count: %u\r\n"
                    "fdatime inquire count: %u\r\n"
                    "fdatime set count: %u\r\n"
                    "fdatime error count: %u\r\n"
                    "fdatime unsupported count: %u\r\n"
                    "fdatime last wflag: %u\r\n"
                    "fdatime last handle: %u\r\n"
                    "fdatime last path: %s\r\n"
                    "fdatime last tnfs rc: %u\r\n"
                    "fdatime last unix mtime: %lu\r\n"
                    "fdatime last gemdos date: %u\r\n"
                    "fdatime last gemdos time: %u\r\n\r\n",
                    (unsigned)s_state.tnfs_fdatime_count, (unsigned)s_state.tnfs_fdatime_inquire_count,
                    (unsigned)s_state.tnfs_fdatime_set_count, (unsigned)s_state.tnfs_fdatime_error_count,
                    (unsigned)s_state.tnfs_fdatime_unsupported_count, (unsigned)s_state.tnfs_fdatime_last_wflag,
                    (unsigned)s_state.tnfs_fdatime_last_handle, s_state.tnfs_fdatime_last_path,
                    (unsigned)s_state.tnfs_fdatime_last_tnfs_rc, (unsigned long)s_state.tnfs_fdatime_last_unix_mtime,
                    (unsigned)s_state.tnfs_fdatime_last_gemdos_date, (unsigned)s_state.tnfs_fdatime_last_gemdos_time);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

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

// Fase (BUGGYBGX/BULGX fix): true for exactly the duration of an active
// fslisting_wait_for() call -- see that function and
// tnfs_fslisting_recv_callback() below. Root cause (see report): every
// fslisting_send_*() shares one 8-bit s_readdirx_seq counter across every
// request type (OPENDIRX/READDIRX/CLOSEDIR/OPEN/READ/WRITE/...), and
// fslisting_wait_for() only ever validated cmd+seq -- with only 256
// possible values and "Show Information" issuing hundreds of round-trips
// per browse, the counter wraps multiple times per session. A response
// that arrives AFTER its own request's bounded wait already gave up
// (timeout) used to sit in s_fslisting_resp with response_ready=true
// until whatever future request happened to reuse that seq value came
// along and (wrongly) accepted it -- silently substituting a stale,
// unrelated directory entry into a completely different search. This
// flag closes that window: any packet arriving while nothing is actively
// waiting is now discarded immediately in the receive callback, so a
// late response for an already-abandoned request can never survive to
// be misattributed to a later, unrelated one.
static volatile bool s_fslisting_waiting = false;

// Fase 9E: this used to be "intentionally never removed once created" --
// true only while sidetnfs_send_mount_probe() was a genuine one-shot,
// once-per-Pico-boot action. It can now run again on every Atari reset
// (see sidetnfs_probe_reinit_active_server()), so sidetnfs_send_mount_probe()
// itself now removes any existing s_mount_pcb before creating a new one --
// still never removed from anywhere else, so no risk of removing it out
// from under an in-flight callback.
static struct udp_pcb *s_mount_pcb = NULL;

// Fase 2 (mount pending-slot fix): every MOUNT request (slot 0 and slot
// 1 alike) now uses the protocol-conformant sequence number 0 -- see
// s_mount_pending_slot above for how responses are attributed to a slot
// instead. The previous distinct-sequence-number scheme (0x40 for slot
// 1) has been removed entirely; it is no longer used anywhere.

// Fase 1: the server address each in-flight MOUNT request was actually
// sent to, captured at send time (not re-parsed from a hostname string
// on every incoming packet) -- tnfs_recv_callback() checks the response
// sender against these, so a spoofed/unexpected-source packet is never
// attributed to either slot regardless of its sequence number.
static ip_addr_t s_mount_expected_addr_slot0;
static ip_addr_t s_mount_expected_addr_slot1;

bool sidetnfs_udp_connect_test(void)
{
    ip_addr_t server_ip;
    if (!ipaddr_aton(s_active_host, &server_ip))
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

    bool connected = (udp_connect(pcb, &server_ip, s_active_port) == ERR_OK);

    udp_remove(pcb);
    cyw43_arch_lwip_end();

    return connected;
}

void sidetnfs_send_udp_probe(void)
{
    ip_addr_t server_ip;
    if (!ipaddr_aton(s_active_host, &server_ip))
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
    udp_sendto(pcb, p, &server_ip, s_active_port);

    pbuf_free(p);
    udp_remove(pcb);

    cyw43_arch_lwip_end();
}

// Fase 9D-R: strict, uppercase-only 8.3 name check. Investigation traced
// the missing "Atari.ST" root-listing entry to this lowercase rejection
// (mixed-case names are silently dropped by sidetnfs_normalize_dir_entry(),
// out->skipped = true, while an already-uppercase sibling like "DOS"
// passes) -- confirmed unrelated to directory position, "."/".." handling,
// or path normalization. Reverted back to rejecting lowercase on request:
// entries with lowercase letters are intentionally ignored -- rename them
// uppercase on the TNFS server instead of relaxing this check.
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
            return false; // lowercase unsupported -- entries must be renamed uppercase on the server
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
        // Fase 2 (mount pending-slot fix): attribution no longer depends
        // on the response's echoed sequence byte at all (see
        // s_mount_pending_slot's own comment above for why) -- only on
        // which single slot is the current pending one, defensively
        // confirmed against that slot's own configured server
        // address+port. At most one of slot 0/slot 1 can ever be pending
        // at a time (sidetnfs_probe_mount_runtime_slots() mounts strictly
        // sequentially), so this is unambiguous.
        uint8_t seq = buf[2]; // no longer used for routing -- captured into the diag snapshot only, see below
        (void)seq;
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        SidetnfsMountRejectReason reject_reason = SIDETNFS_MOUNT_REJECT_NO_PENDING_SLOT;
#endif
        bool accepted = false;
        if (s_mount_pending_slot == 0)
        {
            if (addr != NULL && ip_addr_cmp(addr, &s_mount_expected_addr_slot0))
            {
                if (port == s_active_port)
                {
                    s_state.sid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
                    s_state.mount_rc = n > 4 ? buf[4] : 0;
                    s_state.mount_response_received = true;
                    s_state.debug_dirty = true;
                    s_mount_pending_slot = -1;
                    accepted = true;
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
                    sidetnfs_uart_diag()->slot0_mount_response_received = true;
                    sidetnfs_uart_diag()->slot0_mount_rc = s_state.mount_rc;
                    sidetnfs_uart_diag()->slot0_sid = s_state.sid;
                    sidetnfs_uart_diag()->slot0_last_recv_seq = seq;
#endif
                }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
                else
                {
                    reject_reason = SIDETNFS_MOUNT_REJECT_PORT_MISMATCH;
                }
#endif
            }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
            else
            {
                reject_reason = SIDETNFS_MOUNT_REJECT_ADDR_MISMATCH;
            }
#endif
        }
        else if (s_mount_pending_slot == 1)
        {
            if (addr != NULL && ip_addr_cmp(addr, &s_mount_expected_addr_slot1))
            {
                if (port == s_slot_contexts[1].port)
                {
                    s_slot1_mount_sid = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
                    s_slot1_mount_rc = n > 4 ? buf[4] : 0;
                    s_slot1_mount_response_received = true;
                    s_mount_pending_slot = -1;
                    accepted = true;
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
                    sidetnfs_uart_diag()->slot1_mount_response_received = true;
                    sidetnfs_uart_diag()->slot1_mount_rc = s_slot1_mount_rc;
                    sidetnfs_uart_diag()->slot1_sid = s_slot1_mount_sid;
                    sidetnfs_uart_diag()->slot1_last_recv_seq = seq;
#endif
                }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
                else
                {
                    reject_reason = SIDETNFS_MOUNT_REJECT_PORT_MISMATCH;
                }
#endif
            }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
            else
            {
                reject_reason = SIDETNFS_MOUNT_REJECT_ADDR_MISMATCH;
            }
#endif
        }
        // else: s_mount_pending_slot == -1 -- nothing outstanding right
        // now (stray/duplicate/very-late packet); reject_reason stays
        // SIDETNFS_MOUNT_REJECT_NO_PENDING_SLOT.
        (void)accepted; // only read back via reject_reason below when diag is compiled in
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        if (!accepted)
        {
            sidetnfs_uart_diag()->mount_rejected_count++;
            sidetnfs_uart_diag()->mount_last_reject_reason = (uint8_t)reject_reason;
        }
        sidetnfs_uart_diag()->mount_pending_slot = s_mount_pending_slot;
#endif
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
// Fase 9D-R: canonical TNFS mount-path join. The task's own tie-break rule
// (prefer an empty internal root representation unless the TNFS client
// demonstrably requires "/") resolves to "/": AtariConfig's own UI already
// rejects an empty mount_path on save (buf_nonempty() check), so the
// client demonstrably requires a non-empty value -- "/" is therefore the
// one canonical representation, never "". Always emits exactly one
// leading '/' followed by mount_path with any redundant leading slash(es)
// stripped first, so "", "/", "Atari.ST" and "/Atari.ST" all produce
// well-formed wire paths and neither "" nor "/Atari.ST" ever becomes the
// double-slash "//Atari.ST" that mount_path == "/Atari.ST" used to send
// (the old code unconditionally prepended '/' in front of whatever
// mount_path already contained).
static void build_canonical_mount_path(const char *mount_path, char *out, size_t out_size)
{
    const char *p = (mount_path != NULL) ? mount_path : "";
    while (*p == '/')
    {
        p++;
    }
    snprintf(out, out_size, "/%s", p);
}

// callback for the (optional, asynchronous) reply. Fire-and-forget from the
// caller's point of view -- this function never waits, never retries, never
// logs, and always returns immediately regardless of whether a reply ever
// arrives. Must only be called after WiFi is confirmed connected.
//
// Fase 9E: this can now be called more than once per Pico boot (once per
// Atari reset, via sidetnfs_probe_reinit_active_server() below) -- the old
// s_mount_pcb, if any, is removed first so repeated resets never leak a
// PCB (the original one-PCB-for-the-firmware's-lifetime comment on
// s_mount_pcb above only held while this was truly a once-per-boot call).
void sidetnfs_send_mount_probe(void)
{
    // Mark as attempted regardless of what happens below -- this is a
    // fire-and-forget probe, "sent" means "we tried this boot".
    s_state.mount_probe_sent = true;
    s_state.debug_dirty = true;
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    sidetnfs_uart_diag()->slot0_mount_sent = true;
    snprintf(sidetnfs_uart_diag()->slot0_host, SIDETNFS_HOST_LEN, "%s", s_active_host);
    snprintf(sidetnfs_uart_diag()->slot0_mount_path, SIDETNFS_MOUNTPATH_LEN, "%s", s_active_mount_path);
    sidetnfs_uart_diag()->slot0_port = s_active_port;
#endif

    ip_addr_t server_ip;
    if (!ipaddr_aton(s_active_host, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    if (s_mount_pcb != NULL)
    {
        udp_remove(s_mount_pcb);
        s_mount_pcb = NULL;
    }

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
    // Payload: proto version (minor, major) + null-terminated canonical
    // mount path (see build_canonical_mount_path()) + empty user + empty
    // password.
    char canonical_mount_path[SIDETNFS_MOUNTPATH_LEN + 1]; // +1 for the always-present leading '/'
    build_canonical_mount_path(s_active_mount_path, canonical_mount_path, sizeof(canonical_mount_path));
    size_t mount_path_len = strlen(canonical_mount_path) + 1; // includes '\0'
    uint8_t buf[6 + SIDETNFS_MOUNTPATH_LEN + 1 + 2];
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00; // seq 0 for MOUNT
    buf[3] = TNFS_CMD_MOUNT;
    buf[4] = TNFS_PROTO_VER_MINOR;
    buf[5] = TNFS_PROTO_VER_MAJOR;
    memcpy(&buf[6], canonical_mount_path, mount_path_len); // includes leading '/' and '\0'
    size_t offset = 6 + mount_path_len;
    buf[offset++] = '\0'; // user: anonymous
    buf[offset++] = '\0'; // password: none

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!p)
    {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return;
    }

    // Fase 2 (mount pending-slot fix): mark slot 0 as the pending MOUNT
    // BEFORE the packet actually goes out, so a very fast response can
    // never race ahead of tnfs_recv_callback() being able to attribute
    // it (see s_mount_pending_slot's own comment above).
    s_mount_pending_slot = 0;
    memcpy(p->payload, buf, offset);
    udp_sendto(pcb, p, &server_ip, s_active_port);
    pbuf_free(p);

    // Keep the PCB alive (see s_mount_pcb comment above) so the callback
    // registered above can still fire for a reply that arrives later.
    s_mount_pcb = pcb;

    // Fase 1 (multi-drive slot routing, TNFS mount sequencing): record
    // the actual server this request went to, so tnfs_recv_callback()
    // can validate the response's sender -- see that function's own
    // comment. Purely additive: does not change any byte sent or any
    // existing state written by this function.
    s_mount_expected_addr_slot0 = server_ip;

    cyw43_arch_lwip_end();
}

// Fase 2 (mount pending-slot fix): sends slot 1's (O:) MOUNT request
// over the SAME s_mount_pcb slot 0's own mount already created above --
// never a second/parallel socket. Uses s_slot_contexts[1]'s
// host/mount_path (populated by sidetnfs_probe_set_slot_context() before
// this can ever be called) and the same protocol-conformant sequence
// number 0 slot 0 uses -- tnfs_recv_callback() now tells the response
// apart from slot 0's via s_mount_pending_slot, not via a distinct
// sequence number (see that variable's own comment for why). A no-op
// (s_slot1_mount_response_received stays false) if s_mount_pcb doesn't
// exist yet or the host doesn't parse -- never touches s_state (slot 0)
// either way.
static void send_slot1_mount_request(void)
{
    s_slot1_mount_response_received = false;
    s_slot1_mount_rc = 0xFF;
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    sidetnfs_uart_diag()->slot1_mount_sent = true;
    snprintf(sidetnfs_uart_diag()->slot1_host, SIDETNFS_HOST_LEN, "%s", s_slot_contexts[1].host);
    snprintf(sidetnfs_uart_diag()->slot1_mount_path, SIDETNFS_MOUNTPATH_LEN, "%s", s_slot_contexts[1].mount_path);
    sidetnfs_uart_diag()->slot1_port = s_slot_contexts[1].port;
#endif

    if (s_mount_pcb == NULL)
    {
        return;
    }

    ip_addr_t server_ip;
    if (!ipaddr_aton(s_slot_contexts[1].host, &server_ip))
    {
        return;
    }

    cyw43_arch_lwip_begin();

    // Same wire shape as slot 0's own MOUNT above (session=0x0000 --
    // slot 1 is its own, brand-new TNFS session, never a continuation of
    // slot 0's), only the sequence number and mount path differ.
    char canonical_mount_path[SIDETNFS_MOUNTPATH_LEN + 1];
    build_canonical_mount_path(s_slot_contexts[1].mount_path, canonical_mount_path, sizeof(canonical_mount_path));
    size_t mount_path_len = strlen(canonical_mount_path) + 1;
    uint8_t buf[6 + SIDETNFS_MOUNTPATH_LEN + 1 + 2];
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00; // seq 0 for MOUNT -- same protocol-conformant value slot 0 uses
    buf[3] = TNFS_CMD_MOUNT;
    buf[4] = TNFS_PROTO_VER_MINOR;
    buf[5] = TNFS_PROTO_VER_MAJOR;
    memcpy(&buf[6], canonical_mount_path, mount_path_len);
    size_t offset = 6 + mount_path_len;
    buf[offset++] = '\0'; // user: anonymous
    buf[offset++] = '\0'; // password: none

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return;
    }

    // Fase 2 (mount pending-slot fix): mark slot 1 as the pending MOUNT
    // BEFORE the packet actually goes out -- same reasoning as slot 0's
    // own send above.
    s_mount_pending_slot = 1;
    memcpy(p->payload, buf, offset);
    udp_sendto(s_mount_pcb, p, &server_ip, s_slot_contexts[1].port);
    pbuf_free(p);

    s_mount_expected_addr_slot1 = server_ip;

    cyw43_arch_lwip_end();
}

// Fase 1 (multi-drive slot routing, TNFS mount sequencing): bounded poll
// wait for a single MOUNT response flag -- same shape/timeout as
// fslisting_wait_for() (SIDETNFS_FS_WAIT_MAX_ITER * SIDETNFS_FS_WAIT_STEP_US
// = 200ms), the already-proven bound for one UDP round trip to the
// configured TNFS server on a LAN.
static bool wait_for_mount_response(const bool *response_flag)
{
    for (int i = 0; i < SIDETNFS_FS_WAIT_MAX_ITER; i++)
    {
        cyw43_arch_poll();
        if (*response_flag)
        {
            return true;
        }
        sleep_us(SIDETNFS_FS_WAIT_STEP_US);
    }
    return false; // bounded-wait timeout
}

// Fase 1 (multi-drive slot routing, TNFS mount sequencing): mounts every
// valid TNFS/UDP runtime slot strictly one at a time -- slot 0 (N:, via
// the existing, unchanged sidetnfs_send_mount_probe()) first, then,
// only once its response has arrived or its wait has timed out, slot 1
// (O:, if sidetnfs_probe_set_slot_context() marked it valid and
// TNFS/UDP) over the SAME s_mount_pcb -- never in parallel, never a
// second socket. A failed or timed-out slot 1 mount never touches slot
// 0's own state (s_state is only ever written by the seq==0 branch of
// tnfs_recv_callback()); a failed or timed-out slot 0 mount does not
// skip the slot 1 attempt either -- each slot's outcome is independent.
//
// Call this instead of sidetnfs_send_mount_probe() directly at the one
// call site in gemdrvemul.c that starts network-dependent setup;
// sidetnfs_send_mount_probe() itself is unchanged and still used as-is
// by sidetnfs_probe_reinit_active_server() (single-slot reinit, out of
// scope for this phase).
void sidetnfs_probe_mount_runtime_slots(void)
{
    sidetnfs_send_mount_probe();
    bool slot0_ok = wait_for_mount_response(&s_state.mount_response_received);
    // Fase 2 (mount pending-slot fix): clear the pending marker once this
    // slot's bounded wait is over, whether it actually got a response or
    // timed out -- an accepted response already cleared it to -1 inside
    // tnfs_recv_callback(), so this is then a no-op; on a timeout it's
    // the only place that clears it, so a late straggler arriving after
    // we've moved on (e.g. once slot 1 becomes pending below) can never
    // be mistaken for the slot that's pending next.
    if (s_mount_pending_slot == 0)
    {
        s_mount_pending_slot = -1;
    }
    DPRINTF("TNFS mount slot 0 (N:) path=%s: %s rc=%u sid=0x%04X\n",
            s_active_mount_path, slot0_ok ? "responded" : "TIMED OUT",
            s_state.mount_rc, s_state.sid);

    if (s_slot_contexts[1].valid && s_slot_contexts[1].backend_type == SIDETNFS_DRIVE_TNFS &&
        s_slot_contexts[1].transport == SIDETNFS_TRANSPORT_UDP)
    {
        send_slot1_mount_request();
        bool slot1_ok = wait_for_mount_response(&s_slot1_mount_response_received);
        if (s_mount_pending_slot == 1)
        {
            s_mount_pending_slot = -1;
        }
        DPRINTF("TNFS mount slot 1 (O:) path=%s: %s rc=%u sid=0x%04X\n",
                s_slot_contexts[1].mount_path, slot1_ok ? "responded" : "TIMED OUT",
                s_slot1_mount_rc, s_slot1_mount_sid);
    }
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
    if (!ipaddr_aton(s_active_host, &server_ip))
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
    udp_sendto(s_mount_pcb, p, &server_ip, s_active_port);
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
    if (!ipaddr_aton(s_active_host, &server_ip))
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
    udp_sendto(s_mount_pcb, p, &server_ip, s_active_port);
    pbuf_free(p);

    cyw43_arch_lwip_end();
}

// Sends at most one new network request per call: OPENDIRX once MOUNT
// succeeded, then one READDIRX round at a time once OPENDIRX succeeded,
// until EOF/error/round-cap. Safe to call every GEMDRIVE main-loop
// iteration -- all guards make it a cheap no-op otherwise.
//
// Fase 5P: while the TNFS listing backend is active (SIDETNFS_USE_TNFS_LISTING),
// this legacy root OPENDIRX/READDIRX probe is skipped entirely. Root-cause
// analysis of "root truncated to 1 entry" (see report) found that this
// probe and the (since-removed, Fase 6B) directory cache were both opening
// "/" under the SAME TNFS session id at/around boot time -- two
// concurrent, uncoordinated OPENDIRX/READDIRX conversations sharing one
// session is the most likely explanation for the truncation (either a
// shared server-side read cursor, or sequence-number collision between the
// two independent counters that existed before this fix). MOUNT alone
// (still done above this function) is sufficient for
// sidetnfs_tnfs_listing_ready().
void sidetnfs_probe_service(void)
{
#if SIDETNFS_USE_TNFS_LISTING
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
// completed -- but that probe no longer runs while SIDETNFS_USE_TNFS_LISTING
// (see sidetnfs_probe_service() and the report), so MOUNT success is now
// sufficient. Does NOT check WiFi/network-teardown state -- see
// sidetnfs_probe.h.
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

// Fase 7F-debugfix: called unconditionally from GEMDRVEMUL_FSEEK_CALL's
// TNFS branch in gemdrvemul.c, independent of whether the diagnostic
// eventlog itself still has room -- see SidetnfsDebugState comment.
void sidetnfs_note_tnfs_fseek(uint16_t mode, uint16_t fd, uint8_t rc, bool ok)
{
    s_state.tnfs_fseek_calls++;
    if (ok)
    {
        s_state.tnfs_fseek_ok++;
    }
    else
    {
        s_state.tnfs_fseek_errors++;
    }
    s_state.tnfs_fseek_last_mode = mode;
    s_state.tnfs_fseek_last_fd = fd;
    s_state.tnfs_fseek_last_rc = rc;
    s_state.debug_dirty = true;
}

// Fase 7G: called unconditionally from GEMDRVEMUL_FDELETE_CALL's TNFS
// branch in gemdrvemul.c, independent of whether the diagnostic eventlog
// itself still has room -- see SidetnfsDebugState comment.
void sidetnfs_note_tnfs_fdelete(const char *path, uint8_t rc, bool ok)
{
    s_state.tnfs_fdelete_calls++;
    if (ok)
    {
        s_state.tnfs_fdelete_ok++;
    }
    else
    {
        s_state.tnfs_fdelete_errors++;
    }
    s_state.tnfs_fdelete_last_rc = rc;
    s_state.tnfs_fdelete_last_path[0] = '\0';
    if (path != NULL)
    {
        strncpy(s_state.tnfs_fdelete_last_path, path, sizeof(s_state.tnfs_fdelete_last_path) - 1);
        s_state.tnfs_fdelete_last_path[sizeof(s_state.tnfs_fdelete_last_path) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7H: called unconditionally from GEMDRVEMUL_FRENAME_CALL's TNFS
// branch in gemdrvemul.c, independent of whether the diagnostic eventlog
// itself still has room -- see SidetnfsDebugState comment.
void sidetnfs_note_tnfs_frename(const char *old_path, const char *new_path, uint8_t rc, bool ok)
{
    s_state.tnfs_frename_calls++;
    if (ok)
    {
        s_state.tnfs_frename_ok++;
    }
    else
    {
        s_state.tnfs_frename_errors++;
    }
    s_state.tnfs_frename_last_rc = rc;
    s_state.tnfs_frename_last_old_path[0] = '\0';
    if (old_path != NULL)
    {
        strncpy(s_state.tnfs_frename_last_old_path, old_path, sizeof(s_state.tnfs_frename_last_old_path) - 1);
        s_state.tnfs_frename_last_old_path[sizeof(s_state.tnfs_frename_last_old_path) - 1] = '\0';
    }
    s_state.tnfs_frename_last_new_path[0] = '\0';
    if (new_path != NULL)
    {
        strncpy(s_state.tnfs_frename_last_new_path, new_path, sizeof(s_state.tnfs_frename_last_new_path) - 1);
        s_state.tnfs_frename_last_new_path[sizeof(s_state.tnfs_frename_last_new_path) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7I: called unconditionally from GEMDRVEMUL_DCREATE_CALL's TNFS
// branch in gemdrvemul.c, independent of whether the diagnostic eventlog
// itself still has room -- see SidetnfsDebugState comment.
void sidetnfs_note_tnfs_dcreate(const char *path, uint8_t rc, bool ok)
{
    s_state.tnfs_dcreate_calls++;
    if (ok)
    {
        s_state.tnfs_dcreate_ok++;
    }
    else
    {
        s_state.tnfs_dcreate_errors++;
    }
    s_state.tnfs_dcreate_last_rc = rc;
    s_state.tnfs_dcreate_last_path[0] = '\0';
    if (path != NULL)
    {
        strncpy(s_state.tnfs_dcreate_last_path, path, sizeof(s_state.tnfs_dcreate_last_path) - 1);
        s_state.tnfs_dcreate_last_path[sizeof(s_state.tnfs_dcreate_last_path) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7J: called unconditionally from GEMDRVEMUL_DDELETE_CALL's TNFS
// branch in gemdrvemul.c, independent of whether the diagnostic eventlog
// itself still has room -- see SidetnfsDebugState comment.
void sidetnfs_note_tnfs_ddelete(const char *path, uint8_t rc, bool ok)
{
    s_state.tnfs_ddelete_calls++;
    if (ok)
    {
        s_state.tnfs_ddelete_ok++;
    }
    else
    {
        s_state.tnfs_ddelete_errors++;
    }
    s_state.tnfs_ddelete_last_rc = rc;
    s_state.tnfs_ddelete_last_path[0] = '\0';
    if (path != NULL)
    {
        strncpy(s_state.tnfs_ddelete_last_path, path, sizeof(s_state.tnfs_ddelete_last_path) - 1);
        s_state.tnfs_ddelete_last_path[sizeof(s_state.tnfs_ddelete_last_path) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7J-correctie: called unconditionally from
// sidetnfs_tnfs_dta_close_by_path(), independent of whether the diagnostic
// eventlog itself still has room -- see SidetnfsDebugState comment.
// matches/closed/close_errors accumulate across calls; last_close_rc is a
// direct set (most recent CLOSEDIR wire rc, or 0xFF if none was sent).
void sidetnfs_note_tnfs_ddelete_dta(uint16_t matches, uint16_t closed, uint16_t close_errors, uint8_t last_close_rc)
{
    s_state.tnfs_ddelete_dta_matches += matches;
    s_state.tnfs_ddelete_dta_closed += closed;
    s_state.tnfs_ddelete_dta_close_errors += close_errors;
    s_state.tnfs_ddelete_last_close_rc = last_close_rc;
    s_state.debug_dirty = true;
}

// Fase 7J-correctie-diag: called unconditionally from GEMDRVEMUL_DDELETE_CALL's
// TNFS branch at each of its three possible outcomes (cwd/root-denied,
// DTA-close abort, or an actual RMDIR attempt), independent of whether the
// diagnostic eventlog itself still has room. cwd_reject/root_reject/
// rmdir_attempt each add at most 1 to their running counter when true;
// reason (if non-NULL) overwrites the last-reject-reason string -- pass
// NULL to leave it untouched (used right before sending RMDIR, since the
// actual outcome/reason isn't known yet at that point).
void sidetnfs_note_tnfs_ddelete_diag(bool cwd_reject, bool root_reject, bool rmdir_attempt, const char *reason)
{
    if (cwd_reject)
    {
        s_state.tnfs_ddelete_cwd_rejects++;
    }
    if (root_reject)
    {
        s_state.tnfs_ddelete_root_rejects++;
    }
    if (rmdir_attempt)
    {
        s_state.tnfs_ddelete_rmdir_attempts++;
    }
    if (reason != NULL)
    {
        strncpy(s_state.tnfs_ddelete_last_reject_reason, reason, sizeof(s_state.tnfs_ddelete_last_reject_reason) - 1);
        s_state.tnfs_ddelete_last_reject_reason[sizeof(s_state.tnfs_ddelete_last_reject_reason) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7J-correctie2: called unconditionally from GEMDRVEMUL_DDELETE_CALL's
// TNFS branch. target_match (target path == current TNFS CWD, i.e.
// dpath_string) and parent_update (dpath_string was actually rewritten to
// the parent directory, only ever after a confirmed TNFS_OK RMDIR of that
// same directory) each add at most 1 to their running counter when true.
// cwd_before/cwd_after (each nullable -- pass NULL to leave the
// corresponding stored string untouched) capture dpath_string's value
// right before the match was detected and right after the parent-path
// rewrite, respectively.
void sidetnfs_note_tnfs_ddelete_cwd(bool target_match, bool parent_update, const char *cwd_before,
                                     const char *cwd_after)
{
    if (target_match)
    {
        s_state.tnfs_ddelete_cwd_target_matches++;
    }
    if (parent_update)
    {
        s_state.tnfs_ddelete_cwd_parent_updates++;
    }
    if (cwd_before != NULL)
    {
        strncpy(s_state.tnfs_ddelete_last_cwd_before, cwd_before, sizeof(s_state.tnfs_ddelete_last_cwd_before) - 1);
        s_state.tnfs_ddelete_last_cwd_before[sizeof(s_state.tnfs_ddelete_last_cwd_before) - 1] = '\0';
    }
    if (cwd_after != NULL)
    {
        strncpy(s_state.tnfs_ddelete_last_cwd_after, cwd_after, sizeof(s_state.tnfs_ddelete_last_cwd_after) - 1);
        s_state.tnfs_ddelete_last_cwd_after[sizeof(s_state.tnfs_ddelete_last_cwd_after) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7K: called unconditionally from GEMDRVEMUL_WRITE_BUFF_CALL's TNFS
// branch, once per guest call (not per internal wire round-trip -- see
// sidetnfs_tnfs_file_write()), independent of whether the diagnostic
// eventlog itself still has room. requested/written accumulate into the
// running byte totals; partial increments when written < requested on an
// otherwise-successful call; last_handle/last_requested/last_written/last_rc
// are direct sets.
void sidetnfs_note_tnfs_fwrite(uint8_t handle, uint16_t requested, uint16_t written, uint8_t rc, bool ok,
                                bool partial)
{
    s_state.tnfs_fwrite_calls++;
    if (ok)
    {
        s_state.tnfs_fwrite_ok++;
    }
    else
    {
        s_state.tnfs_fwrite_errors++;
    }
    if (partial)
    {
        s_state.tnfs_fwrite_partial_writes++;
    }
    s_state.tnfs_fwrite_requested_bytes += requested;
    s_state.tnfs_fwrite_written_bytes += written;
    s_state.tnfs_fwrite_last_handle = handle;
    s_state.tnfs_fwrite_last_requested = requested;
    s_state.tnfs_fwrite_last_written = written;
    s_state.tnfs_fwrite_last_rc = rc;
    s_state.debug_dirty = true;
}

// Fase 7L: called unconditionally from GEMDRVEMUL_FATTRIB_CALL's TNFS
// branch, once per guest call, independent of whether the diagnostic
// eventlog itself still has room. wflag distinguishes inquire (0) vs set
// (1) for the inquire/set sub-counters; requested/returned/rc/path are
// direct sets (last-call snapshot, same style as the other Fase 7K/7L
// note functions).
void sidetnfs_note_tnfs_fattrib(uint16_t wflag, uint8_t requested, uint8_t returned, uint8_t rc, bool ok,
                                 bool unsupported, const char *path)
{
    s_state.tnfs_fattrib_calls++;
    if (wflag == 0)
    {
        // wflag 0 == GEMDOS Fattrib inquire, 1 == set (FATTRIB_INQUIRE/
        // FATTRIB_SET in gemdrvemul.h -- not included here to avoid pulling
        // in that header, same reasoning as scfs.h's own circular-include
        // note).
        s_state.tnfs_fattrib_inquire_calls++;
    }
    else
    {
        s_state.tnfs_fattrib_set_calls++;
    }
    if (ok)
    {
        s_state.tnfs_fattrib_ok++;
    }
    else
    {
        s_state.tnfs_fattrib_errors++;
    }
    if (unsupported)
    {
        s_state.tnfs_fattrib_unsupported++;
    }
    s_state.tnfs_fattrib_last_wflag = (uint8_t)wflag;
    s_state.tnfs_fattrib_last_requested = requested;
    s_state.tnfs_fattrib_last_returned = returned;
    s_state.tnfs_fattrib_last_rc = rc;
    if (path != NULL)
    {
        strncpy(s_state.tnfs_fattrib_last_path, path, sizeof(s_state.tnfs_fattrib_last_path) - 1);
        s_state.tnfs_fattrib_last_path[sizeof(s_state.tnfs_fattrib_last_path) - 1] = '\0';
    }
    s_state.debug_dirty = true;
}

// Fase 7M: called unconditionally from GEMDRVEMUL_FDATETIME_CALL's TNFS
// branch, once per guest call, independent of whether the diagnostic
// eventlog itself still has room. wflag distinguishes inquire (0) vs set
// (1) for the inquire/set sub-counters; the rest are direct sets (last-call
// snapshot, same style as sidetnfs_note_tnfs_fattrib() above).
void sidetnfs_note_tnfs_fdatime(uint16_t wflag, uint8_t handle, const char *path, uint8_t rc, bool ok,
                                 bool unsupported, uint32_t unix_mtime, uint16_t gemdos_date, uint16_t gemdos_time)
{
    s_state.tnfs_fdatime_count++;
    if (wflag == 0)
    {
        s_state.tnfs_fdatime_inquire_count++;
    }
    else
    {
        s_state.tnfs_fdatime_set_count++;
    }
    if (!ok)
    {
        s_state.tnfs_fdatime_error_count++;
    }
    if (unsupported)
    {
        s_state.tnfs_fdatime_unsupported_count++;
    }
    s_state.tnfs_fdatime_last_wflag = (uint8_t)wflag;
    s_state.tnfs_fdatime_last_handle = handle;
    if (path != NULL)
    {
        strncpy(s_state.tnfs_fdatime_last_path, path, sizeof(s_state.tnfs_fdatime_last_path) - 1);
        s_state.tnfs_fdatime_last_path[sizeof(s_state.tnfs_fdatime_last_path) - 1] = '\0';
    }
    s_state.tnfs_fdatime_last_tnfs_rc = rc;
    s_state.tnfs_fdatime_last_unix_mtime = unix_mtime;
    s_state.tnfs_fdatime_last_gemdos_date = gemdos_date;
    s_state.tnfs_fdatime_last_gemdos_time = gemdos_time;
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

    // Fase (BUGGYBGX/BULGX fix): nobody is currently waiting for anything
    // on this channel -- this is necessarily a stale response to an
    // already-abandoned (timed-out) request. Discard it immediately
    // rather than storing it, so it can never later be mismatched against
    // a future, unrelated request that happens to reuse the same 8-bit
    // seq value (see s_fslisting_waiting's own comment above).
    if (!s_fslisting_waiting)
    {
        pbuf_free(p);
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
// tnfs_fslisting_recv_callback() and consumed by the TNFS DTA-registry path
// (sidetnfs_tnfs_dta_start(), Fase 5Y). Wire pattern is always "*" (local
// filtering only -- see sidetnfs_probe.h).
//
// Fase 1 (multi-drive slot routing): host/port/session id now come from
// *ctx (the caller's already-resolved runtime slot -- see
// sidetnfs_probe_get_slot_context()), not the single-session
// s_active_host/s_active_port/s_state.sid globals. s_readdirx_seq is a
// single, ever-incrementing counter shared by every fslisting_send_*
// function regardless of slot, so every request+response pair still gets
// a globally unique sequence number -- fslisting_wait_for()'s existing
// cmd+seq correlation needs no other change to keep two slots' traffic
// apart, as long as (as required by this phase) at most one request is
// ever outstanding at a time.
// *out_seq receives the sequence number used, for cmd+seq response
// correlation (Fase 5R) -- only valid if this function returns true.
static bool fslisting_send_opendirx(const sidetnfs_slot_tnfs_context_t *ctx, const char *tnfs_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[8 + 1 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
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
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fire-and-forget READDIRX send for up to max_entries entries on an
// already-open handle. Same non-blocking contract as
// fslisting_send_opendirx(), including *out_seq and the *ctx-based
// host/port/session id (Fase 1, multi-drive slot routing).
static bool fslisting_send_readdirx(const sidetnfs_slot_tnfs_context_t *ctx, uint8_t dir_handle, uint8_t max_entries,
                                     uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[6];
    buf[0] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[1] = (uint8_t)(ctx->session_id >> 8);
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
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 5AA: fire-and-forget CLOSEDIR send for a handle obtained from
// OPENDIRX. Same non-blocking contract and wire shape as
// fslisting_send_readdirx() (header + single handle byte), including the
// *ctx-based host/port/session id (Fase 1, multi-drive slot routing).
static bool fslisting_send_closedir(const sidetnfs_slot_tnfs_context_t *ctx, uint8_t dir_handle, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[5];
    buf[0] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[1] = (uint8_t)(ctx->session_id >> 8);
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
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7D: fire-and-forget OPEN send over s_fslisting_pcb. flags is one of
// the TNFS_OPEN_* values above, OR-ed together. mode (creation mode, Unix
// permission-bit style) is only meaningful when TNFS_OPEN_CREAT is set in
// flags -- Fase 7K's sidetnfs_tnfs_file_create() passes 0644 (0x1A4);
// every other caller passes 0 (ignored by the server when O_CREAT isn't
// requested, same as before this phase).
//
// Fase 10 (slot-aware fix): ctx (host/port/session_id) now comes from the
// caller's own resolved runtime slot, never the slot-0-only
// s_active_host/s_active_port/s_state.sid globals this used to read
// directly.
static bool fslisting_send_open(const sidetnfs_slot_tnfs_context_t *ctx, const char *tnfs_path, uint16_t flags,
                                 uint16_t mode, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[4 + 4 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_OPEN;
    buf[offset++] = (uint8_t)(flags & 0xFFu);
    buf[offset++] = (uint8_t)(flags >> 8);
    buf[offset++] = (uint8_t)(mode & 0xFFu);
    buf[offset++] = (uint8_t)(mode >> 8);
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
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7G: fire-and-forget UNLINK send -- header + null-terminated path,
// no other fields (matches the general "path-only" shape already used by
// OPENDIRX's path portion, minus the pattern prefix).
static bool fslisting_send_unlink(const sidetnfs_slot_tnfs_context_t *ctx, const char *tnfs_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[4 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_UNLINK;
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
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7L: fire-and-forget STAT send -- header + null-terminated path, no
// other fields (same shape as fslisting_send_unlink(), different opcode).
static bool fslisting_send_stat(const sidetnfs_slot_tnfs_context_t *ctx, const char *tnfs_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[4 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_STAT;
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
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7Lb: fslisting_send_chmod() (the TNFS_CMD_CHMOD/0x27 sender) has
// been removed -- see the TNFS_CMD_CHMOD comment above: the actual
// server's tnfs_chmod() handler is confirmed empty (no payload parse, no
// chmod(), no response), so sending it would only ever time out. TNFS
// Fattrib set no longer sends any wire request at all (see
// sidetnfs_tnfs_set_attributes() below).

// Fase 7I: fire-and-forget MKDIR send -- header + null-terminated path,
// no other fields (same shape as fslisting_send_unlink(), different
// opcode).
static bool fslisting_send_mkdir(const sidetnfs_slot_tnfs_context_t *ctx, const char *tnfs_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[4 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_MKDIR;
    memcpy(&buf[offset], tnfs_path, path_len);
    offset += path_len;
    buf[offset++] = '\0';

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *mkdir_p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!mkdir_p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(mkdir_p->payload, buf, offset);
    udp_sendto(s_fslisting_pcb, mkdir_p, &server_ip, ctx->port);
    pbuf_free(mkdir_p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7J: fire-and-forget RMDIR send -- header + null-terminated path,
// no other fields (same shape as fslisting_send_mkdir(), different
// opcode). Never used as a fallback for UNLINK or vice versa.
static bool fslisting_send_rmdir(const sidetnfs_slot_tnfs_context_t *ctx, const char *tnfs_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t path_len = strnlen(tnfs_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[4 + MAX_FOLDER_LENGTH];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_RMDIR;
    memcpy(&buf[offset], tnfs_path, path_len);
    offset += path_len;
    buf[offset++] = '\0';

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *rmdir_p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!rmdir_p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(rmdir_p->payload, buf, offset);
    udp_sendto(s_fslisting_pcb, rmdir_p, &server_ip, ctx->port);
    pbuf_free(rmdir_p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7H: fire-and-forget RENAME send -- header + null-terminated old
// path + null-terminated new path, no other fields.
static bool fslisting_send_rename(const sidetnfs_slot_tnfs_context_t *ctx, const char *old_path,
                                   const char *new_path, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    size_t old_len = strnlen(old_path, MAX_FOLDER_LENGTH - 1);
    size_t new_len = strnlen(new_path, MAX_FOLDER_LENGTH - 1);
    uint8_t buf[4 + (MAX_FOLDER_LENGTH * 2)];
    size_t offset = 0;
    buf[offset++] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[offset++] = (uint8_t)(ctx->session_id >> 8);
    buf[offset++] = seq;
    buf[offset++] = TNFS_CMD_RENAME;
    memcpy(&buf[offset], old_path, old_len);
    offset += old_len;
    buf[offset++] = '\0';
    memcpy(&buf[offset], new_path, new_len);
    offset += new_len;
    buf[offset++] = '\0';

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *rename_p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (!rename_p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(rename_p->payload, buf, offset);
    udp_sendto(s_fslisting_pcb, rename_p, &server_ip, ctx->port);
    pbuf_free(rename_p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7D: fire-and-forget READ send for up to size bytes on an
// already-open TNFS file handle. Same non-blocking contract as the other
// fslisting_send_* helpers.
static bool fslisting_send_read(const sidetnfs_slot_tnfs_context_t *ctx, uint8_t tnfs_handle, uint16_t size,
                                 uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[7];
    buf[0] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[1] = (uint8_t)(ctx->session_id >> 8);
    buf[2] = seq;
    buf[3] = TNFS_CMD_READ;
    buf[4] = tnfs_handle;
    buf[5] = (uint8_t)(size & 0xFFu);
    buf[6] = (uint8_t)(size >> 8);

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7K: fire-and-forget WRITE send for a TNFS file handle. Mirrors
// fslisting_send_read()'s request shape (header + handle + size) but,
// unlike READ, the request itself carries the data being written (READ's
// data comes back in the *response* instead) -- size is both "how many
// bytes to write" and "how many data bytes follow in this packet". The
// chunk data is copied directly from the caller's buffer straight into the
// pbuf payload (no intermediate stack copy of the chunk itself -- only the
// small fixed header is ever built on the stack), per the "no large
// temporary stack buffer" requirement.
static bool fslisting_send_write(const sidetnfs_slot_tnfs_context_t *ctx, uint8_t tnfs_handle, const uint8_t *data,
                                  uint16_t size, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t header[7];
    header[0] = (uint8_t)(ctx->session_id & 0xFFu);
    header[1] = (uint8_t)(ctx->session_id >> 8);
    header[2] = seq;
    header[3] = TNFS_CMD_WRITE;
    header[4] = tnfs_handle;
    header[5] = (uint8_t)(size & 0xFFu);
    header[6] = (uint8_t)(size >> 8);

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(sizeof(header) + size), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, header, sizeof(header));
    if (size > 0)
    {
        memcpy((uint8_t *)p->payload + sizeof(header), data, size);
    }
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7D: fire-and-forget CLOSE send for a TNFS file handle obtained from
// OPEN. Same wire shape as fslisting_send_closedir() (header + single
// handle byte), different opcode/namespace (file handle, not dir handle).
static bool fslisting_send_close(const sidetnfs_slot_tnfs_context_t *ctx, uint8_t tnfs_handle, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[5];
    buf[0] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[1] = (uint8_t)(ctx->session_id >> 8);
    buf[2] = seq;
    buf[3] = TNFS_CMD_CLOSE;
    buf[4] = tnfs_handle;

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    *out_seq = seq;
    return true;
}

// Fase 7F: fire-and-forget SEEK send for an already-open TNFS file handle.
// whence is TNFS_SEEK_SET or TNFS_SEEK_END (see sidetnfs_tnfs_file_seek()).
// position is sent as a signed 32-bit LE value, matching the published
// TNFS LSEEK request shape (fd + whence + signed offset).
static bool fslisting_send_seek(const sidetnfs_slot_tnfs_context_t *ctx, uint8_t tnfs_handle, uint8_t whence,
                                 int32_t position, uint8_t *out_seq)
{
    if (!fslisting_ensure_pcb())
    {
        return false;
    }
    ip_addr_t server_ip;
    if (!ipaddr_aton(ctx->host, &server_ip))
    {
        return false;
    }

    uint8_t seq = s_readdirx_seq++;
    uint8_t buf[10];
    uint32_t position_u = (uint32_t)position;
    buf[0] = (uint8_t)(ctx->session_id & 0xFFu);
    buf[1] = (uint8_t)(ctx->session_id >> 8);
    buf[2] = seq;
    buf[3] = TNFS_CMD_SEEK;
    buf[4] = tnfs_handle;
    buf[5] = whence;
    buf[6] = (uint8_t)(position_u & 0xFFu);
    buf[7] = (uint8_t)((position_u >> 8) & 0xFFu);
    buf[8] = (uint8_t)((position_u >> 16) & 0xFFu);
    buf[9] = (uint8_t)((position_u >> 24) & 0xFFu);

    s_fslisting_resp.response_ready = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (!p)
    {
        cyw43_arch_lwip_end();
        return false;
    }
    memcpy(p->payload, buf, sizeof(buf));
    udp_sendto(s_fslisting_pcb, p, &server_ip, ctx->port);
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
                                       uint8_t max_entries, uint16_t *out_skipped,
                                       uint32_t trace_ndta, int32_t trace_runtime_slot)
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
        bool normalized_ok = sidetnfs_normalize_dir_entry(name, flags, size, mtime, &out_entries[count]);
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        {
            char raw_name_trace[14] = {0};
            size_t raw_copy_len = nlen < sizeof(raw_name_trace) - 1 ? nlen : sizeof(raw_name_trace) - 1;
            memcpy(raw_name_trace, name, raw_copy_len);
            raw_name_trace[raw_copy_len] = '\0';
            sidetnfs_name_trace_log(SIDETNFS_NAME_EVT_READDIRX_NORMALIZE, trace_ndta, trace_runtime_slot,
                                     raw_name_trace, normalized_ok ? out_entries[count].name : "", NULL, NULL);
        }
#endif
        if (normalized_ok)
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

// Fase 5Y: TNFS DTA registry -- lookupTnfsDTA()/insertTnfsDTA()/
// releaseTnfsDTA() mirror the shape (not the storage) of the FatFS
// lookupDTA()/insertDTA()/releaseDTA() hash table in gemdrvemul.c: keyed by
// ndta, insert on Fsfirst, lookup on Fsnext, release on EOF/error/repeated
// Fsfirst. See report -- the SD-baseline hardware test showed real
// GEMDRVEMUL_FSNEXT_CALL dispatch works once state is registered this way.
// Fase 5AA: defined below (after fslisting_wait_for()) -- forward-declared
// here so insertTnfsDTA()/releaseTnfsDTA() can call it. Fase 1
// (multi-drive slot routing): runtime_slot identifies which slot's
// session/host/port to resolve for the CLOSEDIR itself.
static void tnfs_dta_closedir(uint32_t ndta, uint8_t dir_handle, int runtime_slot);

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
        tnfs_dta_closedir(victim->ndta, victim->dir_handle, victim->runtime_slot);
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_HANDLE_RELEASE, victim->ndta, NULL, NULL, NULL, 0, victim->dir_handle, 0,
                           0);
        victim->handle_valid = false;
    }
    return victim;
}

// Register/replace ndta's TNFS DTA-registry entry -- a repeated Fsfirst for
// the same ndta reuses (and overwrites) its existing slot, exactly like the
// SD/FatFS backend's insertDTA() replacing an existing DTANode (see
// gemdrvemul.c, and report: a repeated Fsfirst always starts fresh in this
// model -- the Fase 5U/5V repeat-continuation workaround was removed in
// Fase 6B). Only called after OPENDIRX has already succeeded for
// dir_handle. Fase 5AA: if
// the reused slot still has an open handle from the search it's replacing
// (no EOF/release happened in between), that handle is CLOSEDIR'd first --
// otherwise a repeated Fsfirst/refresh cycle leaks one handle per repeat.
static SidetnfsTnfsDtaSearch *insertTnfsDTA(uint32_t ndta, const char *path, const char *pattern, uint8_t attribs,
                                             uint8_t dir_handle, int runtime_slot)
{
    SidetnfsTnfsDtaSearch *slot = lookupTnfsDTA(ndta);
    if (slot)
    {
        if (slot->handle_valid)
        {
            // Fase 1: the OLD search's own runtime slot, read before the
            // memset() below overwrites it -- the entry being replaced
            // may belong to a different slot than the new one being
            // inserted.
            tnfs_dta_closedir(ndta, slot->dir_handle, slot->runtime_slot);
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
    // Fase 1 (multi-drive slot routing): the slot this search's own
    // OPENDIRX just succeeded against -- read back by
    // tnfs_dta_find_next_match()/releaseTnfsDTA()/tnfs_dta_closedir() for
    // every subsequent READDIRX/CLOSEDIR this search issues.
    slot->runtime_slot = runtime_slot;
    // Fase 7J-correctie-diag: fires on every ordinary Fsfirst, not just a
    // Ddelete's own directory -- gated the same way as the READDIRX detail
    // events (see SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL) so routine Desktop
    // browsing doesn't crowd a genuine Ddelete sequence out of the fixed
    // eventlog budget. Logging only, no control-flow change.
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_INSERT, ndta, path, pattern, NULL, 0, 0, 0, attribs);
#endif
    return slot;
}

// Release ndta's TNFS DTA-registry entry, if any (no-op otherwise). Fase
// 5AA: also sends a real TNFS CLOSEDIR for the entry's dir_handle first,
// if it's still open (handle_valid). handle_valid is cleared regardless of
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
            tnfs_dta_closedir(ndta, slot->dir_handle, slot->runtime_slot);
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_HANDLE_RELEASE, ndta, NULL, NULL, NULL, 0, slot->dir_handle, 0, 0);
            slot->handle_valid = false;
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_RELEASE, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
        slot->active = false;
    }
}

// Fase 9E: bulk-release every active TNFS DTA-registry search slot (each
// with a real CLOSEDIR via releaseTnfsDTA(), same as a single-slot
// release) -- used by sidetnfs_probe_reinit_active_server() so no
// directory handle from the OLD server/session is left open when a newly
// saved config is adopted after an Atari reset.
void sidetnfs_tnfs_dta_release_all(void)
{
    for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (s_tnfs_dta_searches[i].active)
        {
            releaseTnfsDTA(s_tnfs_dta_searches[i].ndta);
        }
    }
}

// Bounded-wait for a response matching expect_cmd+expect_seq. Any
// stray/mismatched response is discarded (never misread as the answer to
// a later request) -- see report on sequence/callback correlation.
//
// Fase (BUGGYBGX/BULGX fix): s_fslisting_waiting is true for exactly the
// duration of this call, so tnfs_fslisting_recv_callback() only ever
// stores a response while someone is genuinely waiting for one -- a
// response for a request THIS function already gave up on (timed out)
// arriving later, outside any wait_for call, is discarded at the
// callback instead of lingering to be misattributed to a future,
// unrelated request with a reused seq value. Set back to false on every
// return path.
static bool fslisting_wait_for(uint8_t expect_cmd, uint8_t expect_seq)
{
    s_fslisting_waiting = true;
    for (int i = 0; i < SIDETNFS_FS_WAIT_MAX_ITER; i++)
    {
        cyw43_arch_poll();
        if (s_fslisting_resp.response_ready)
        {
            if (s_fslisting_resp.cmd == expect_cmd && s_fslisting_resp.seq == expect_seq)
            {
                s_fslisting_waiting = false;
                return true;
            }
            s_fslisting_resp.response_ready = false; // stray/late response -- discard
        }
        sleep_us(SIDETNFS_FS_WAIT_STEP_US);
    }
    s_fslisting_waiting = false;
    return false; // bounded-wait timeout
}

// Fase 5AA/6D: send CLOSEDIR for dir_handle and wait (bounded -- same
// SIDETNFS_FS_WAIT_MAX_ITER/STEP_US timeout and cmd+seq validation as
// OPENDIRX/READDIRX, via fslisting_wait_for()) for the response. Never
// blocks indefinitely and never retries -- a failed/timed-out CLOSEDIR is
// logged and otherwise ignored; the caller (insertTnfsDTA()/
// releaseTnfsDTA()) always proceeds with local cleanup regardless of the
// outcome (see report: cleanup must never hang or crash, even if this
// server doesn't support/recognize CLOSEDIR). Always sent -- hardware
// testing confirmed leaked, un-CLOSEDIR'd directory handles caused
// listings to go empty after repeated refreshes (see report), so there is
// no longer a "local-only release" fallback mode.
static void tnfs_dta_closedir(uint32_t ndta, uint8_t dir_handle, int runtime_slot)
{
    // Fase 1 (multi-drive slot routing): resolve the search's own slot's
    // host/port/session id -- a slot that's become invalid or lost its
    // session between OPENDIRX and now is treated exactly like any other
    // send failure below (logged, local cleanup still proceeds in every
    // caller regardless).
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_CLOSEDIR_ERROR, ndta, NULL, NULL, NULL, 0, dir_handle, 0, 0);
        return;
    }
    uint8_t seq = 0;
    if (!fslisting_send_closedir(&ctx, dir_handle, &seq))
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
    // Fase 1 (multi-drive slot routing): this search's own slot's
    // host/port/session id -- read once per call (Fsnext calls this
    // again for every entry), never re-resolved mid-round. A slot that's
    // become invalid/lost its session is treated as a plain send/round
    // failure, same as any other network error this loop already handles.
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(search->runtime_slot, &ctx))
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }

    for (uint32_t round = 0; round < SIDETNFS_TNFS_DTA_MAX_ROUNDS; round++)
    {
        if (search->eof)
        {
            return SIDETNFS_DIR_SEARCH_NOT_FOUND;
        }

        uint8_t seq = 0;
        if (!fslisting_send_readdirx(&ctx, search->dir_handle, (uint8_t)SIDETNFS_READDIRX_MAX_ENTRIES, &seq))
        {
            return SIDETNFS_DIR_SEARCH_ERROR;
        }
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_ONE, search->ndta, search->path, NULL, NULL, (uint16_t)round, 0,
                           0, 0);
#endif
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
            // Fase 7D4: rate-limited -- see s_last_readdirx_eof_ndta
            // comment. Fase 7F-debugfix: also fully suppressed under either
            // focus mode (see SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL).
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
            if (!s_last_readdirx_eof_ndta_valid || s_last_readdirx_eof_ndta != search->ndta)
            {
                sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_EOF, search->ndta, search->path, NULL, NULL, 0, 0, 0, 0);
                s_last_readdirx_eof_ndta = search->ndta;
                s_last_readdirx_eof_ndta_valid = true;
            }
#endif
            continue; // next loop iteration sees search->eof and returns NOT_FOUND
        }

        SidetnfsAtariDirEntry entry;
        uint16_t skipped = 0;
        uint8_t got = fslisting_parse_batch(batch, &entry, 1, &skipped, search->ndta, (int32_t)search->runtime_slot);
        if (got == 0)
        {
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_SKIP, search->ndta, search->path, NULL, NULL, 0, 0, 0, 0);
#endif
            continue;
        }
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_ENTRY, search->ndta, search->path, NULL, entry.name, 0, 0, 0,
                           entry.attr);
#endif
        if (sidetnfs_gemdos_pattern_match(entry.name, search->pattern) &&
            sidetnfs_gemdos_attr_match(entry.attr, search->attribs))
        {
            *out_entry = entry;
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
            sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_READDIRX_MATCH, search->ndta, search->path, NULL, entry.name, 0, 0,
                               0, entry.attr);
#endif
            return SIDETNFS_DIR_SEARCH_FOUND;
        }
        // no match -- loop and read the next entry
    }
    // Round cap reached without a definitive answer.
    return SIDETNFS_DIR_SEARCH_ERROR;
}

SidetnfsDirSearchResult sidetnfs_tnfs_dta_start(uint32_t ndta, int slot, const char *path,
                                                  const char *pattern, uint8_t attribs,
                                                  SidetnfsAtariDirEntry *out_entry)
{
    // Fase 1 (multi-drive slot routing): `slot` is trusted here -- the
    // caller (GEMDRVEMUL_FSFIRST_CALL in gemdrvemul.c) has already
    // validated range/g_drive_count/runtime-config/session_established
    // before ever reaching this function. Resolved once, up front; a
    // slot that somehow fails to resolve here anyway is treated as a
    // plain OPENDIRX failure, same as any other network error below.
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(slot, &ctx))
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }

    // Fase 5Y: OPENDIRX first -- the registry entry is only inserted
    // (insertTnfsDTA()) once we actually have a dir_handle, exactly like
    // the SD/FatFS backend only calls insertDTA() after f_findfirst()
    // already produced a match (see gemdrvemul.c). A repeated Fsfirst for
    // the same ndta always starts fresh, deliberately (the Fase 5U/5V
    // repeat-continuation workaround was removed in Fase 6B -- see report).
    uint8_t seq = 0;
    if (!fslisting_send_opendirx(&ctx, path, &seq))
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    // Fase 7J-correctie-diag: fires on every ordinary Fsfirst/directory
    // open, not just a Ddelete's own directory -- same gating reasoning as
    // TNFS_DTA_INSERT above. Logging only, no control-flow change.
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_OPENDIRX, ndta, path, pattern, NULL, 0, 0, 0, attribs);
#endif
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
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_OPENDIRX_OK, ndta, path, NULL, NULL, 0, handle, 0, 0);
#endif

    SidetnfsTnfsDtaSearch *search = insertTnfsDTA(ndta, path, pattern, attribs, handle, slot);

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
        // Fase 7J-correctie-diag: LOOKUP_FAIL is a genuine error and stays
        // unconditional -- only the routine per-call LOOKUP_OK below (fires
        // once per Fsnext, i.e. once per directory entry during ordinary
        // browsing) is gated.
        sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_LOOKUP_FAIL, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
#if !SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL
    sidetnfs_diag_log(SIDETNFS_DIAG_TNFS_DTA_LOOKUP_OK, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
#endif
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

// Fase 7J: targeted release, used by Ddelete -- scans the fixed
// SIDETNFS_TNFS_DTA_SLOTS array (never any broader reset) and releases
// (CLOSEDIR + mark inactive, via the same releaseTnfsDTA() every other
// DTA-registry release path already uses) only the slot(s) whose stored
// path exactly matches tnfs_path. Ordinarily at most one slot can match
// (one active search per ndta, and Ddelete only ever targets a single
// directory), but every matching slot is released defensively in case
// more than one concurrent search happens to be open on it.
void sidetnfs_tnfs_dta_release_by_path(const char *tnfs_path)
{
    if (tnfs_path == NULL)
    {
        return;
    }
    for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (s_tnfs_dta_searches[i].active && strcmp(s_tnfs_dta_searches[i].path, tnfs_path) == 0)
        {
            releaseTnfsDTA(s_tnfs_dta_searches[i].ndta);
        }
    }
}

// Fase 7J-correctie: targeted pre-RMDIR close, used by Ddelete instead of
// sidetnfs_tnfs_dta_release_by_path() above. A still-open OPENDIRX handle
// on the exact directory RMDIR targets (e.g. Desktop's own enumeration of
// that folder, not yet closed) can make the server refuse RMDIR, so every
// matching slot's handle must be confirmed CLOSEDIR'd -- not just
// fire-and-forgotten like releaseTnfsDTA()'s tnfs_dta_closedir() -- before
// RMDIR is ever attempted. A slot's local state (handle_valid/active) is
// only cleared once its CLOSEDIR is confirmed successful; a failed/timed
// out CLOSEDIR leaves the slot exactly as-is (still active, still
// handle_valid) so no local state silently drifts out of sync with an
// unconfirmed server-side handle. Only exact path matches are touched --
// never a broader reset.
bool sidetnfs_tnfs_dta_close_by_path(const char *tnfs_path, uint16_t *out_matches, uint8_t *out_close_rc)
{
    uint16_t matches = 0;
    uint16_t closed = 0;
    uint16_t close_errors = 0;
    uint8_t last_rc = 0xFFu;
    bool all_ok = true;

    if (tnfs_path != NULL)
    {
        for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
        {
            SidetnfsTnfsDtaSearch *slot = &s_tnfs_dta_searches[i];
            if (!slot->active || strcmp(slot->path, tnfs_path) != 0)
            {
                continue;
            }
            matches++;
            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_MATCH, slot->ndta, tnfs_path, NULL, NULL, 0,
                               slot->dir_handle, 0, 0);

            if (!slot->handle_valid)
            {
                // Nothing open on the server for this slot -- safe to drop
                // the local registry entry outright.
                releaseTnfsDTA(slot->ndta);
                closed++;
                continue;
            }

            // Fase 1 (multi-drive slot routing): this registry entry's
            // own slot (set by insertTnfsDTA() when Fsfirst opened it) --
            // a resolution failure is folded into the same "send failed"
            // path below, same as any other network error here.
            sidetnfs_slot_tnfs_context_t ddelete_close_ctx;
            uint8_t seq = 0;
            if (!sidetnfs_probe_get_slot_context(slot->runtime_slot, &ddelete_close_ctx) ||
                !fslisting_send_closedir(&ddelete_close_ctx, slot->dir_handle, &seq))
            {
                sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_CLOSE_RC, slot->ndta, tnfs_path, NULL, NULL, 0,
                                   slot->dir_handle, 0xFFu, 0);
                last_rc = 0xFFu;
                close_errors++;
                all_ok = false;
                continue; // send failed -- leave slot untouched, server state unknown
            }
            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_CLOSE, slot->ndta, tnfs_path, NULL, NULL, 0,
                               slot->dir_handle, 0, 0);
            if (!fslisting_wait_for(TNFS_CMD_CLOSEDIR, seq))
            {
                sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_CLOSE_RC, slot->ndta, tnfs_path, NULL, NULL, 0,
                                   slot->dir_handle, 0xFFu, 0);
                last_rc = 0xFFu;
                close_errors++;
                all_ok = false;
                continue; // timed out -- leave slot untouched, server state unknown
            }
            uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
            s_fslisting_resp.response_ready = false;
            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_CLOSE_RC, slot->ndta, tnfs_path, NULL, NULL, 0,
                               slot->dir_handle, rc, 0);
            last_rc = rc;
            if (rc == TNFS_OK)
            {
                // Confirmed closed server-side -- now safe to drop local
                // state. handle_valid is cleared first so releaseTnfsDTA()'s
                // own tnfs_dta_closedir() call is skipped (never CLOSEDIR
                // twice for the handle we just closed).
                slot->handle_valid = false;
                releaseTnfsDTA(slot->ndta);
                closed++;
            }
            else
            {
                close_errors++;
                all_ok = false; // server refused/failed the close -- leave slot untouched
            }
        }
    }

    sidetnfs_note_tnfs_ddelete_dta(matches, closed, close_errors, last_rc);
    if (out_matches)
    {
        *out_matches = matches;
    }
    if (out_close_rc)
    {
        *out_close_rc = last_rc;
    }
    return all_ok;
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

// Fase 7D/7K: shared TNFS OPEN wire logic -- send with the given raw flags
// (and mode, only meaningful together with TNFS_OPEN_CREAT), wait, parse.
// Used by both sidetnfs_tnfs_file_open() (Fopen, never creates) and
// sidetnfs_tnfs_file_create() (Fcreate, always CREAT|TRUNC) below, so the
// wire-level behavior/logging is identical regardless of which GEMDOS call
// triggered it. See TNFS_CMD_OPEN comment above for the opcode
// disclosure/risk note. ndta is not used for file ops (0) -- path is the
// identifying field in the log until a guest handle exists.
//
// Fase 10 (slot-aware fix): runtime_slot resolves this OPEN's own
// host/port/session_id via sidetnfs_probe_get_slot_context() (bounds-checked
// there, never indexes s_slot_contexts[] out of range) -- an invalid slot
// is treated as a plain OPEN-send failure (SIDETNFS_FILE_OPEN_ERROR), same
// as any other network-level failure this function already handles.
static SidetnfsFileOpenResult tnfs_open_with_flags(int runtime_slot, const char *tnfs_path, uint16_t flags,
                                                    uint16_t mode, uint8_t *out_handle)
{
    bool is_create = (flags & TNFS_OPEN_CREAT) != 0;
    s_state.tnfs_fopen_calls++;
    s_state.debug_dirty = true;

    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_ERROR, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_FILE_OPEN_ERROR;
    }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    SidetnfsUartDiagSnapshot *diag = sidetnfs_uart_diag();
    if (is_create)
    {
        diag->fcreate_last_session_id = ctx.session_id;
    }
    else
    {
        diag->fopen_last_session_id = ctx.session_id;
    }
#endif

    uint8_t seq = 0;
    if (!fslisting_send_open(&ctx, tnfs_path, flags, mode, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_ERROR, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_FILE_OPEN_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_OPEN, 0, tnfs_path, NULL, NULL, 0, 0, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_OPEN, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_ERROR, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        if (is_create) { diag->fcreate_last_tnfs_rc = 0xFFu; } else { diag->fopen_last_tnfs_rc = 0xFFu; }
#endif
        return SIDETNFS_FILE_OPEN_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    uint8_t handle = s_fslisting_resp.len > 5 ? s_fslisting_resp.buf[5] : 0;
    s_fslisting_resp.response_ready = false;
    // Fase 7D-debug: unconditional -- the exact wire rc byte for this OPEN,
    // whatever it turns out to be, regardless of success/failure.
    sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, rc, 0);
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    if (is_create) { diag->fcreate_last_tnfs_rc = rc; } else { diag->fopen_last_tnfs_rc = rc; }
#endif
    if (rc == TNFS_OK)
    {
        *out_handle = handle;
        s_state.tnfs_fopen_ok++;
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_OK, handle, tnfs_path, NULL, NULL, 0, 0, 0, 0);
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_HANDLE, handle, tnfs_path, NULL, NULL, handle, 0, 0, 0);
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        if (is_create) { diag->fcreate_last_tnfs_handle = handle; } else { diag->fopen_last_tnfs_handle = handle; }
#endif
        return SIDETNFS_FILE_OPEN_OK;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_ERROR, 0, tnfs_path, NULL, NULL, 0, 0, rc, 0);
    return (rc == TNFS_ENOENT) ? SIDETNFS_FILE_OPEN_NOT_FOUND : SIDETNFS_FILE_OPEN_ERROR;
}

// Fase 7D/7K: TNFS OPEN for GEMDOS Fopen. gemdos_mode is the raw Fopen mode
// (0=read-only, 1=write-only, 2=read/write -- the only three GEMDOS
// defines; gemdrive_backend_fopen() in gemdrvemul.c already denies
// anything else before this is ever called). Never sets TNFS_OPEN_CREAT --
// Fopen never creates a file, matching the SD/FatFS route's own
// mode-to-FA_* mapping (FA_READ / FA_WRITE / FA_READ|FA_WRITE, no
// FA_CREATE_ALWAYS).
SidetnfsFileOpenResult sidetnfs_tnfs_file_open(int runtime_slot, const char *tnfs_path, uint16_t gemdos_mode,
                                                 uint8_t *out_handle)
{
    uint16_t flags;
    switch (gemdos_mode)
    {
    case 1: // write-only
        flags = TNFS_OPEN_WRITE;
        break;
    case 2: // read/write
        flags = TNFS_OPEN_RDONLY | TNFS_OPEN_WRITE;
        break;
    case 0: // read-only
    default:
        flags = TNFS_OPEN_RDONLY;
        break;
    }
    return tnfs_open_with_flags(runtime_slot, tnfs_path, flags, 0, out_handle);
}

// Fase 7K: TNFS OPEN for GEMDOS Fcreate -- always creates the file if it
// doesn't exist and truncates it to zero length if it does (GEMDOS Fcreate
// never appends to or preserves an existing file's contents), opened
// read/write, matching the SD/FatFS route's own
// FA_READ|FA_WRITE|FA_CREATE_ALWAYS unconditionally. mode 0644 (0x1A4) is
// sent as the TNFS creation mode -- a conventional Unix "rw-r--r--"
// default, since GEMDOS Fcreate's own fattr parameter is an Atari
// attribute byte (hidden/system/etc.), not a Unix permission mode, and
// nothing here has verified how (or whether) this server maps one to the
// other.
SidetnfsFileOpenResult sidetnfs_tnfs_file_create(int runtime_slot, const char *tnfs_path, uint8_t *out_handle)
{
    return tnfs_open_with_flags(runtime_slot, tnfs_path,
                                 TNFS_OPEN_RDONLY | TNFS_OPEN_WRITE | TNFS_OPEN_CREAT | TNFS_OPEN_TRUNC, 0x1A4u,
                                 out_handle);
}

// Fase 7D5: TNFS READ. Writes directly into the caller's buffer (the guest
// shared-memory read area, see gemdrive_backend_fread() in gemdrvemul.c) --
// no intermediate large stack buffer, only the existing fixed
// s_fslisting_resp.buf[SIDETNFS_RX_BUF_SIZE] response scratch area.
//
// Loops internally over SIDETNFS_TNFS_READ_CHUNK_MAX-sized wire round-trips
// (bounded by SIDETNFS_TNFS_READ_MAX_ROUNDS) until `requested` bytes have
// been collected or the source signals no more data (actual==0 or
// rc==TNFS_EOF on some round) -- matching the SD/FatFS route's f_read()
// contract exactly: fill the whole requested amount, short only at real
// EOF. Any wire error mid-loop discards this call's progress entirely and
// returns false, same as f_read() reporting FR_* failure regardless of
// bytes already read internally by FatFS.
bool sidetnfs_tnfs_file_read(uint32_t guest_fd, uint8_t tnfs_handle, int runtime_slot, uint8_t *out_buf,
                              uint16_t requested, uint16_t *out_actual)
{
    s_state.tnfs_fread_calls++;
    s_state.debug_dirty = true;

    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0xFFu, 0);
        return false;
    }

    uint16_t total = 0;
    uint8_t last_rc = TNFS_OK;
    for (uint32_t round = 0; round < SIDETNFS_TNFS_READ_MAX_ROUNDS; round++)
    {
        uint16_t remaining = (uint16_t)(requested - total);
        if (remaining == 0)
        {
            break;
        }
        uint16_t chunk = remaining > SIDETNFS_TNFS_READ_CHUNK_MAX ? (uint16_t)SIDETNFS_TNFS_READ_CHUNK_MAX : remaining;
        uint8_t seq = 0;
        if (!fslisting_send_read(&ctx, tnfs_handle, chunk, &seq))
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk, 0xFFu, 0);
            return false;
        }
#if SIDETNFS_DEBUG_FOCUS_FILE_IO
        sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_READ, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk, 0,
                           (uint8_t)round);
#endif
        if (!fslisting_wait_for(TNFS_CMD_READ, seq))
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk, 0xFFu, 0);
            return false;
        }
        uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
        last_rc = rc;
#if SIDETNFS_DEBUG_FOCUS_FILE_IO
        sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk, rc,
                           (uint8_t)round);
#endif
        uint16_t actual = 0;
        if (s_fslisting_resp.len > 6)
        {
            actual = (uint16_t)s_fslisting_resp.buf[5] | ((uint16_t)s_fslisting_resp.buf[6] << 8);
        }
        // Never trust the wire-reported size beyond what the response
        // frame actually carries or what was requested this round --
        // clamp defensively before touching out_buf.
        uint16_t avail = s_fslisting_resp.len > 7 ? (uint16_t)(s_fslisting_resp.len - 7) : 0;
        if (actual > avail)
        {
            actual = avail;
        }
        if (actual > chunk)
        {
            actual = chunk;
        }
        if (rc != TNFS_OK && rc != TNFS_EOF)
        {
            s_fslisting_resp.response_ready = false;
            sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk, rc, 0);
            return false;
        }
        if (actual > 0)
        {
            memcpy(out_buf + total, &s_fslisting_resp.buf[7], actual);
        }
        s_fslisting_resp.response_ready = false;
        total += actual;
        s_state.tnfs_fread_bytes += actual;
        if (actual == 0 || rc == TNFS_EOF)
        {
            break; // no more data available from this handle right now
        }
    }
    *out_actual = total;
    // Fase 7D5: one call-level summary regardless of how many internal
    // rounds it took, so the default (non-focus) event budget stays at
    // "one entry per guest READ_BUFF_CALL" (unchanged from before this
    // phase) -- the per-round detail above only fires under
    // SIDETNFS_DEBUG_FOCUS_FILE_IO.
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, total, last_rc, 0);
    if (total == 0)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_EOF, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0, 0);
    }
    else
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_TNFS_OK, guest_fd, NULL, NULL, NULL, tnfs_handle, total, 0, 0);
    }
    return true;
}

// Fase 7K: TNFS WRITE. Sends data (the guest's shared-memory write buffer,
// already byte-order-converted by the caller -- see
// GEMDRVEMUL_WRITE_BUFF_CALL's TNFS branch in gemdrvemul.c) in
// SIDETNFS_TNFS_WRITE_CHUNK_MAX-sized wire round-trips (bounded by
// SIDETNFS_TNFS_WRITE_MAX_ROUNDS) until `requested` bytes have been sent or
// the server accepts fewer bytes than a given round asked for. Diagnostics
// are deliberately minimal this phase (compact counters only, no per-round
// event) -- see report: the write ACK handshake is timing-sensitive, and a
// past bug elsewhere in this codebase involved an ACK handler clearing a
// byte count too early, so this phase avoids adding any per-chunk logging
// that could perturb it.
bool sidetnfs_tnfs_file_write(uint32_t guest_fd, uint8_t tnfs_handle, int runtime_slot, const uint8_t *data,
                               uint16_t requested, uint16_t *out_actual, uint8_t *out_rc)
{
    // Fase 7K: no low-level counter bumped here -- the call-level counters
    // (fwrite calls/ok/errors/... , see SidetnfsDebugState) are recorded
    // once per GEMDRVEMUL_WRITE_BUFF_CALL by sidetnfs_note_tnfs_fwrite(),
    // called from gemdrvemul.c, matching the compact/call-level-only
    // diagnostics this phase asks for.
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_TRANSPORT_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0xFFu, 0);
        if (out_actual)
        {
            *out_actual = 0;
        }
        if (out_rc)
        {
            *out_rc = 0xFFu;
        }
        return false;
    }

    uint16_t total = 0;
    uint8_t last_rc = TNFS_OK;
    for (uint32_t round = 0; round < SIDETNFS_TNFS_WRITE_MAX_ROUNDS; round++)
    {
        uint16_t remaining = (uint16_t)(requested - total);
        if (remaining == 0)
        {
            break;
        }
        uint16_t chunk =
            remaining > SIDETNFS_TNFS_WRITE_CHUNK_MAX ? (uint16_t)SIDETNFS_TNFS_WRITE_CHUNK_MAX : remaining;
        uint8_t seq = 0;
        if (!fslisting_send_write(&ctx, tnfs_handle, data + total, chunk, &seq))
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_TRANSPORT_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk,
                               0xFFu, 0);
            if (out_actual)
            {
                *out_actual = total;
            }
            if (out_rc)
            {
                *out_rc = 0xFFu;
            }
            return false;
        }
        if (!fslisting_wait_for(TNFS_CMD_WRITE, seq))
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_TRANSPORT_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk,
                               0xFFu, 0);
            if (out_actual)
            {
                *out_actual = total;
            }
            if (out_rc)
            {
                *out_rc = 0xFFu;
            }
            return false;
        }
        uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
        if (rc != TNFS_OK)
        {
            s_fslisting_resp.response_ready = false;
            sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_SERVER_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, chunk, rc,
                               0);
            if (out_actual)
            {
                *out_actual = total;
            }
            if (out_rc)
            {
                *out_rc = rc;
            }
            return false;
        }
        uint16_t actual = 0;
        if (s_fslisting_resp.len > 6)
        {
            actual = (uint16_t)s_fslisting_resp.buf[5] | ((uint16_t)s_fslisting_resp.buf[6] << 8);
        }
        s_fslisting_resp.response_ready = false;
        // Never trust the wire-reported size beyond what was requested this
        // round -- clamp defensively.
        if (actual > chunk)
        {
            actual = chunk;
        }
        total += actual;
        last_rc = rc;
        if (actual < chunk)
        {
            // Genuine short/partial write from the server (e.g. disk full)
            // -- not an error. Stop here rather than looping further, so a
            // real partial-write condition is never masked behind
            // additional chunks that would silently pad the guest's byte
            // count (see report).
            break;
        }
    }
    if (out_actual)
    {
        *out_actual = total;
    }
    if (out_rc)
    {
        *out_rc = last_rc;
    }
    return true;
}

// Fase 7D: TNFS CLOSE. Always logs the outcome; never reports failure to
// the caller (see header comment) -- the local file descriptor must always
// be released regardless, same "cleanup can't hang or be retried" contract
// as tnfs_dta_closedir() for directory handles.
void sidetnfs_tnfs_file_close(uint32_t guest_fd, uint8_t tnfs_handle, int runtime_slot)
{
    s_state.tnfs_fclose_calls++;
    s_state.debug_dirty = true;
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        // Same "local descriptor always released regardless" contract as
        // every other failure path here -- an unresolvable slot just means
        // the TNFS CLOSE is never sent; the caller still deletes its own
        // tracking entry right after this call either way.
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0xFFu, 0);
        return;
    }
    uint8_t seq = 0;
    if (!fslisting_send_close(&ctx, tnfs_handle, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0xFFu, 0);
        return;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_CLOSE, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_CLOSE, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, 0xFFu, 0);
        return;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    s_fslisting_resp.response_ready = false;
    // Fase 7D-debug: unconditional -- the exact wire rc byte for this CLOSE.
    sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, rc, 0);
    if (rc == TNFS_OK)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_OK, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, rc, 0);
    }
    else
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_TNFS_ERROR, guest_fd, NULL, NULL, NULL, tnfs_handle, 0, rc, 0);
    }
}

// Fase 7E: one-shot TNFS directory-existence probe for
// GEMDRVEMUL_DSETPATH_CALL. Deliberately does NOT go through the TNFS
// DTA-registry (sidetnfs_tnfs_dta_start()/next()) or the fake no-network
// search table -- this is purely an existence check, not a listing
// session, so it must never register/overwrite a registry slot for some
// ndta or leave one behind. Always attempts to CLOSEDIR the handle it
// opens (best-effort, same "cleanup can't hang or be retried" contract as
// tnfs_dta_closedir()) before returning.
//
// Fase 1 (multi-drive slot routing): fslisting_send_opendirx()/
// _closedir() now take an explicit context -- this function is only
// ever called by Dsetpath, which is out of scope for this phase and
// stays exactly as before (implicitly slot 0's session), so slot 0's
// context is resolved here directly rather than threading a slot
// parameter through a call chain this phase doesn't otherwise touch.
bool sidetnfs_tnfs_directory_exists(int runtime_slot, const char *tnfs_path, uint8_t *out_rc)
{
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    // Reset this call's record up front so a bail-out below still leaves a
    // consistent, fully-overwritten snapshot rather than mixing fields
    // from a previous call. Purely additive -- no functional change below.
    SidetnfsUartDiagSnapshot *diag = sidetnfs_uart_diag();
    diag->dsetpath_exists_calls++;
    diag->dsetpath_exists_runtime_slot = runtime_slot;
    diag->dsetpath_exists_session_id = 0;
    diag->dsetpath_exists_host[0] = '\0';
    diag->dsetpath_exists_port = 0;
    snprintf(diag->dsetpath_exists_tnfs_path, MAX_FOLDER_LENGTH, "%s", tnfs_path ? tnfs_path : "");
    diag->dsetpath_exists_opendirx_seq = 0;
    diag->dsetpath_exists_opendirx_response_received = false;
    diag->dsetpath_exists_opendirx_rc = 0xFFu;
    diag->dsetpath_exists_dir_handle = 0;
    diag->dsetpath_exists_closedir_sent = false;
    diag->dsetpath_exists_closedir_response_received = false;
    diag->dsetpath_exists_closedir_rc = 0xFFu;
#endif

    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        *out_rc = 0xFFu;
        return false;
    }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    diag->dsetpath_exists_session_id = ctx.session_id;
    snprintf(diag->dsetpath_exists_host, SIDETNFS_HOST_LEN, "%s", ctx.host);
    diag->dsetpath_exists_port = ctx.port;
#endif

    uint8_t seq = 0;
    if (!fslisting_send_opendirx(&ctx, tnfs_path, &seq))
    {
        *out_rc = 0xFFu;
        return false;
    }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    diag->dsetpath_exists_opendirx_seq = seq;
#endif
    if (!fslisting_wait_for(TNFS_CMD_OPENDIRX, seq))
    {
        *out_rc = 0xFFu;
        return false;
    }
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    diag->dsetpath_exists_opendirx_response_received = true;
#endif
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    uint8_t handle = s_fslisting_resp.len > 5 ? s_fslisting_resp.buf[5] : 0;
    s_fslisting_resp.response_ready = false;
    *out_rc = rc;
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
    diag->dsetpath_exists_opendirx_rc = rc;
    diag->dsetpath_exists_dir_handle = handle;
#endif
    if (rc != TNFS_OK)
    {
        return false;
    }
    // Directory exists and is now open on the server -- this was only ever
    // an existence probe, so close it immediately. Best-effort: a
    // failed/timed-out CLOSEDIR here still must not change the existence
    // result already established above, nor block/retry.
    uint8_t close_seq = 0;
    if (fslisting_send_closedir(&ctx, handle, &close_seq))
    {
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        diag->dsetpath_exists_closedir_sent = true;
#endif
        bool closedir_responded = fslisting_wait_for(TNFS_CMD_CLOSEDIR, close_seq);
#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
        diag->dsetpath_exists_closedir_response_received = closedir_responded;
        if (closedir_responded)
        {
            diag->dsetpath_exists_closedir_rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
        }
#else
        (void)closedir_responded;
#endif
        s_fslisting_resp.response_ready = false;
    }
    return true;
}

// Fase 7F: TNFS SEEK -- see header comment for the SEEK_SET-vs-SEEK_END
// split rationale. Fase 7F-debugfix: out_rc (nullable) is a purely
// additive diagnostic parameter -- the raw wire rc byte, for
// sidetnfs_note_tnfs_fseek()'s "fseek last rc" counter in gemdrvemul.c.
// Does not change the seek logic, payload, or return value contract.
bool sidetnfs_tnfs_file_seek(uint32_t guest_fd, uint8_t tnfs_handle, int runtime_slot, bool seek_from_end,
                              int32_t offset, uint32_t *out_new_offset, uint8_t *out_rc)
{
    uint8_t whence = seek_from_end ? TNFS_SEEK_END : TNFS_SEEK_SET;
    if (out_rc)
    {
        *out_rc = 0xFFu;
    }
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, whence, 0xFFu, 0);
        return false;
    }
    uint8_t seq = 0;
    if (!fslisting_send_seek(&ctx, tnfs_handle, whence, offset, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, whence, 0xFFu, 0);
        return false;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_TNFS_SEEK, guest_fd, NULL, NULL, NULL, tnfs_handle, whence, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_SEEK, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, whence, 0xFFu, 0);
        return false;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    if (out_rc)
    {
        *out_rc = rc;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_TNFS_RC, guest_fd, NULL, NULL, NULL, tnfs_handle, whence, rc, 0);
    if (rc != TNFS_OK)
    {
        s_fslisting_resp.response_ready = false;
        return false;
    }
    if (seek_from_end)
    {
        // Only SEEK_END needs the server-reported resulting position --
        // for SEEK_SET the caller already computed the target itself.
        if (s_fslisting_resp.len < 9)
        {
            // Response doesn't carry the expected 4-byte position field --
            // can't determine the resulting offset for SEEK_END without
            // it. Would need a separate TNFS_CMD_STAT call instead, not
            // implemented this phase (see report).
            s_fslisting_resp.response_ready = false;
            return false;
        }
        *out_new_offset = (uint32_t)s_fslisting_resp.buf[5] | ((uint32_t)s_fslisting_resp.buf[6] << 8) |
                          ((uint32_t)s_fslisting_resp.buf[7] << 16) | ((uint32_t)s_fslisting_resp.buf[8] << 24);
    }
    else
    {
        *out_new_offset = (uint32_t)offset;
    }
    s_fslisting_resp.response_ready = false;
    return true;
}

// Fase 7G: TNFS UNLINK for a file. Relies on the server's own unlink()
// refusing to remove a directory (standard POSIX semantics -- TNFS servers
// generally proxy straight to the host OS's unlink() syscall) rather than
// doing a separate TNFS_CMD_STAT type-check first: any such refusal comes
// back as a non-OK rc, which this function (and its caller in
// gemdrvemul.c) always treats as "not deleted", never as success -- so a
// directory can never be silently reported as deleted regardless of the
// exact rc the server happens to return for that case.
SidetnfsFileDeleteResult sidetnfs_tnfs_file_delete(int runtime_slot, const char *tnfs_path, uint8_t *out_rc)
{
    if (out_rc)
    {
        *out_rc = 0xFFu;
    }
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        return SIDETNFS_FILE_DELETE_ERROR;
    }
    uint8_t seq = 0;
    if (!fslisting_send_unlink(&ctx, tnfs_path, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_FILE_DELETE_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_TNFS_UNLINK, 0, tnfs_path, NULL, NULL, 0, 0, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_UNLINK, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_FILE_DELETE_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    if (out_rc)
    {
        *out_rc = rc;
    }
    s_fslisting_resp.response_ready = false;
    sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, rc, 0);
    if (rc == TNFS_OK)
    {
        return SIDETNFS_FILE_DELETE_OK;
    }
    if (rc == TNFS_ENOENT)
    {
        return SIDETNFS_FILE_DELETE_NOT_FOUND;
    }
    if (rc == TNFS_EACCES || rc == TNFS_EISDIR)
    {
        // EISDIR (server refused to unlink a directory) is deliberately
        // folded into the same "access denied" result as EACCES -- never
        // treated as success, see function comment above.
        return SIDETNFS_FILE_DELETE_ACCESS_DENIED;
    }
    return SIDETNFS_FILE_DELETE_ERROR;
}

// Fase 7H: TNFS RENAME. No pre-check of source/destination existence --
// the wire rc alone determines the result, so a rename can never be
// reported as successful unless the server actually performed it. If the
// server refuses because the destination already exists (assumed to
// surface as EEXIST, unverified against this specific server), that is
// folded into ACCESS_DENIED rather than silently overwriting or inventing
// a delete-then-rename fallback.
SidetnfsFileRenameResult sidetnfs_tnfs_file_rename(int runtime_slot, const char *old_path, const char *new_path,
                                                    uint8_t *out_rc)
{
    if (out_rc)
    {
        *out_rc = 0xFFu;
    }
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        return SIDETNFS_FILE_RENAME_ERROR;
    }
    uint8_t seq = 0;
    if (!fslisting_send_rename(&ctx, old_path, new_path, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_TNFS_RC, 0, old_path, NULL, new_path, 0, 0, 0xFFu, 0);
        return SIDETNFS_FILE_RENAME_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_TNFS_RENAME, 0, old_path, NULL, new_path, 0, 0, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_RENAME, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_TNFS_RC, 0, old_path, NULL, new_path, 0, 0, 0xFFu, 0);
        return SIDETNFS_FILE_RENAME_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    if (out_rc)
    {
        *out_rc = rc;
    }
    s_fslisting_resp.response_ready = false;
    sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_TNFS_RC, 0, old_path, NULL, new_path, 0, 0, rc, 0);
    if (rc == TNFS_OK)
    {
        return SIDETNFS_FILE_RENAME_OK;
    }
    if (rc == TNFS_ENOENT)
    {
        // TNFS's single ENOENT doesn't distinguish "source not found" from
        // "destination directory not found" -- same limitation already
        // noted for Fopen/Dsetpath/Fdelete. Mapped to EFILNF by the caller
        // (matches the Fdelete precedent), not EPTHNF.
        return SIDETNFS_FILE_RENAME_NOT_FOUND;
    }
    if (rc == TNFS_EACCES || rc == TNFS_EEXIST || rc == TNFS_EISDIR)
    {
        return SIDETNFS_FILE_RENAME_ACCESS_DENIED;
    }
    return SIDETNFS_FILE_RENAME_ERROR;
}

// Fase 7I: TNFS MKDIR. No pre-check of existence -- no directory listing,
// no separate stat. The wire rc alone determines the result.
SidetnfsDirCreateResult sidetnfs_tnfs_directory_create(int runtime_slot, const char *tnfs_path, uint8_t *out_rc)
{
    if (out_rc)
    {
        *out_rc = 0xFFu;
    }
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        return SIDETNFS_DIR_CREATE_ERROR;
    }
    uint8_t seq = 0;
    if (!fslisting_send_mkdir(&ctx, tnfs_path, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_DIR_CREATE_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_TNFS_MKDIR, 0, tnfs_path, NULL, NULL, 0, 0, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_MKDIR, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_DIR_CREATE_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    if (out_rc)
    {
        *out_rc = rc;
    }
    s_fslisting_resp.response_ready = false;
    sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, rc, 0);
    if (rc == TNFS_OK)
    {
        return SIDETNFS_DIR_CREATE_OK;
    }
    if (rc == TNFS_ENOENT)
    {
        // Unambiguous here (unlike Fopen/Fdelete/Frename): a parent path
        // component doesn't exist. The target itself not existing is the
        // expected precondition, not this error.
        return SIDETNFS_DIR_CREATE_PATH_NOT_FOUND;
    }
    if (rc == TNFS_EACCES || rc == TNFS_EEXIST || rc == TNFS_EISDIR)
    {
        // EEXIST (assumed wire code when the target already exists,
        // unverified against this specific server) folded into
        // ACCESS_DENIED, matching the Frename precedent.
        return SIDETNFS_DIR_CREATE_ACCESS_DENIED;
    }
    return SIDETNFS_DIR_CREATE_ERROR;
}

// Fase 7J: TNFS RMDIR. No pre-check, no directory enumeration to see
// whether the directory is empty -- the server must bear that
// responsibility atomically. Never falls back to TNFS_CMD_UNLINK.
SidetnfsDirDeleteResult sidetnfs_tnfs_directory_delete(int runtime_slot, const char *tnfs_path, uint8_t *out_rc)
{
    if (out_rc)
    {
        *out_rc = 0xFFu;
    }
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        return SIDETNFS_DIR_DELETE_ERROR;
    }
    uint8_t seq = 0;
    if (!fslisting_send_rmdir(&ctx, tnfs_path, &seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_DIR_DELETE_ERROR;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_TNFS_RMDIR, 0, tnfs_path, NULL, NULL, 0, 0, 0, 0);
    if (!fslisting_wait_for(TNFS_CMD_RMDIR, seq))
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, 0xFFu, 0);
        return SIDETNFS_DIR_DELETE_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    if (out_rc)
    {
        *out_rc = rc;
    }
    s_fslisting_resp.response_ready = false;
    sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_TNFS_RC, 0, tnfs_path, NULL, NULL, 0, 0, rc, 0);
    if (rc == TNFS_OK)
    {
        return SIDETNFS_DIR_DELETE_OK;
    }
    if (rc == TNFS_ENOENT || rc == TNFS_ENOTDIR)
    {
        // ENOTDIR ("not a directory") mapped to the same PATH_NOT_FOUND
        // result as ENOENT ("doesn't exist") -- both ultimately map to
        // GEMDOS_EPTHNF upstream, per the report's explicit choice.
        return SIDETNFS_DIR_DELETE_PATH_NOT_FOUND;
    }
    if (rc == TNFS_EACCES || rc == TNFS_ENOTEMPTY)
    {
        // ENOTEMPTY (assumed wire code for a non-empty directory,
        // unverified against this specific server) is the case that
        // matters most here: never treated as success, always denied.
        return SIDETNFS_DIR_DELETE_ACCESS_DENIED;
    }
    return SIDETNFS_DIR_DELETE_ERROR;
}

// Fase 7L: raw TNFS STAT result -- kept private to this file. gemdrvemul.c
// only ever sees the already-DOS-mapped SidetnfsAttrResult/FS_ST_* shape
// below via sidetnfs_tnfs_get_attributes()/sidetnfs_tnfs_set_attributes(),
// never this raw mode/size pair, matching the established "keep raw TNFS
// wire details out of gemdrvemul.c" layering used throughout this file.
typedef struct
{
    uint16_t mode; // best-guess POSIX-style mode bits, see TNFS_CMD_STAT comment
    uint32_t size;
    uint32_t mtime; // Fase 7M: Unix epoch seconds, UTC -- see TNFS_CMD_STAT comment for the confirmed offset
} SidetnfsTnfsRawStat;

typedef enum
{
    SIDETNFS_TNFS_STAT_OK = 0,
    SIDETNFS_TNFS_STAT_NOT_FOUND,      // TNFS_ENOENT
    SIDETNFS_TNFS_STAT_PATH_NOT_FOUND, // TNFS_ENOTDIR
    SIDETNFS_TNFS_STAT_ACCESS_DENIED,  // TNFS_EACCES
    SIDETNFS_TNFS_STAT_ERROR
} SidetnfsTnfsStatResult;

// POSIX mode bits this file actually cares about -- read-only, for Fattrib
// inquire (see tnfs_mode_to_st_attribs() below). No write-mask constant
// here anymore -- Fase 7Lb removed the CHMOD-based set path entirely (see
// TNFS_CMD_CHMOD comment above), so nothing in this file ever computes a
// new mode to send.
#define TNFS_MODE_IFDIR 0x4000u // S_IFDIR (0040000 octal)
#define TNFS_MODE_IWUSR 0x0080u // S_IWUSR (0200 octal) -- owner-write

// Fase 7Lb/7M: confirmed against the actual server source (tnfsd
// 24.0522.1) -- TNFS_STATFILE (0x24) response payload is exactly 0x16 (22)
// bytes after header(4)+rc(1): mode(2 LE) at offset 0x00, uid(2) at 0x02,
// gid(2) at 0x04, size(4 LE) at 0x06, atime(4) at 0x0A, mtime(4 LE) at
// 0x0E, ctime(4) at 0x12. mode/size/mtime are read here (uid/gid/atime/
// ctime aren't needed for GEMDOS attributes or Fdatime). A response
// shorter than the full 22 bytes is treated as malformed/untrustworthy,
// not partially parsed.
#define SIDETNFS_TNFS_STAT_PAYLOAD_LEN 0x16u
#define SIDETNFS_TNFS_STAT_MTIME_OFFSET 0x0Eu

// Fase 7L/7M: send TNFS STAT and parse its response into a raw mode/size/
// mtime triple.
static SidetnfsTnfsStatResult tnfs_stat_raw(int runtime_slot, const char *tnfs_path, SidetnfsTnfsRawStat *out_stat,
                                             uint8_t *out_rc)
{
    if (out_rc)
    {
        *out_rc = 0xFFu;
    }
    sidetnfs_slot_tnfs_context_t ctx;
    if (!sidetnfs_probe_get_slot_context(runtime_slot, &ctx))
    {
        return SIDETNFS_TNFS_STAT_ERROR;
    }
    uint8_t seq = 0;
    if (!fslisting_send_stat(&ctx, tnfs_path, &seq))
    {
        return SIDETNFS_TNFS_STAT_ERROR;
    }
    if (!fslisting_wait_for(TNFS_CMD_STAT, seq))
    {
        return SIDETNFS_TNFS_STAT_ERROR;
    }
    uint8_t rc = s_fslisting_resp.len > 4 ? s_fslisting_resp.buf[4] : 0xFFu;
    if (out_rc)
    {
        *out_rc = rc;
    }
    if (rc != TNFS_OK)
    {
        s_fslisting_resp.response_ready = false;
        if (rc == TNFS_ENOENT)
        {
            return SIDETNFS_TNFS_STAT_NOT_FOUND;
        }
        if (rc == TNFS_ENOTDIR)
        {
            return SIDETNFS_TNFS_STAT_PATH_NOT_FOUND;
        }
        if (rc == TNFS_EACCES)
        {
            return SIDETNFS_TNFS_STAT_ACCESS_DENIED;
        }
        return SIDETNFS_TNFS_STAT_ERROR;
    }
    // Confirmed payload length is header(4)+rc(1)+22 = 27 bytes -- a short
    // response is malformed/untrustworthy, not partially parsed (see
    // SIDETNFS_TNFS_STAT_PAYLOAD_LEN comment above).
    if (s_fslisting_resp.len < 5 + SIDETNFS_TNFS_STAT_PAYLOAD_LEN)
    {
        s_fslisting_resp.response_ready = false;
        return SIDETNFS_TNFS_STAT_ERROR;
    }
    uint16_t mode = (uint16_t)s_fslisting_resp.buf[5] | ((uint16_t)s_fslisting_resp.buf[6] << 8);
    uint32_t size = (uint32_t)s_fslisting_resp.buf[11] | ((uint32_t)s_fslisting_resp.buf[12] << 8) |
                    ((uint32_t)s_fslisting_resp.buf[13] << 16) | ((uint32_t)s_fslisting_resp.buf[14] << 24);
    size_t mtime_off = 5 + SIDETNFS_TNFS_STAT_MTIME_OFFSET;
    uint32_t mtime = (uint32_t)s_fslisting_resp.buf[mtime_off] | ((uint32_t)s_fslisting_resp.buf[mtime_off + 1] << 8) |
                      ((uint32_t)s_fslisting_resp.buf[mtime_off + 2] << 16) |
                      ((uint32_t)s_fslisting_resp.buf[mtime_off + 3] << 24);
    s_fslisting_resp.response_ready = false;
    if (out_stat)
    {
        out_stat->mode = mode;
        out_stat->size = size;
        out_stat->mtime = mtime;
    }
    return SIDETNFS_TNFS_STAT_OK;
}

static SidetnfsAttrResult tnfs_stat_result_to_attr_result(SidetnfsTnfsStatResult r)
{
    switch (r)
    {
    case SIDETNFS_TNFS_STAT_OK:
        return SIDETNFS_ATTR_OK;
    case SIDETNFS_TNFS_STAT_NOT_FOUND:
        return SIDETNFS_ATTR_NOT_FOUND;
    case SIDETNFS_TNFS_STAT_PATH_NOT_FOUND:
        return SIDETNFS_ATTR_PATH_NOT_FOUND;
    case SIDETNFS_TNFS_STAT_ACCESS_DENIED:
        return SIDETNFS_ATTR_ACCESS_DENIED;
    default:
        return SIDETNFS_ATTR_ERROR;
    }
}

// Fase 7L: derive GEMDOS/DOS-style (FS_ST_*) attributes from a raw POSIX
// mode. Only FS_ST_FOLDER and FS_ST_READONLY are ever set -- hidden/
// system/archive/label have no POSIX-mode equivalent and this server
// exposes no other attribute channel this project knows of, so they
// always read back as off (never a false "yes, persisted"). Group/other
// write bits are deliberately not consulted for the read-only bit --
// owner-write is this project's single source of truth, both for reading
// it back here and for the fixed restore policy in
// sidetnfs_tnfs_set_attributes() below.
static uint8_t tnfs_mode_to_st_attribs(uint16_t mode)
{
    uint8_t attribs = 0;
    if (mode & TNFS_MODE_IFDIR)
    {
        attribs |= FS_ST_FOLDER;
    }
    if ((mode & TNFS_MODE_IWUSR) == 0)
    {
        attribs |= FS_ST_READONLY;
    }
    return attribs;
}

// Fase 7L: TNFS STAT -> GEMDOS/DOS-style attributes, for Fattrib inquire.
SidetnfsAttrResult sidetnfs_tnfs_get_attributes(int runtime_slot, const char *tnfs_path, uint8_t *out_st_attribs,
                                                 uint8_t *out_rc)
{
    SidetnfsTnfsRawStat stat = {0};
    SidetnfsTnfsStatResult result = tnfs_stat_raw(runtime_slot, tnfs_path, &stat, out_rc);
    if (result != SIDETNFS_TNFS_STAT_OK)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FATTRIB_STAT_ERROR, 0, tnfs_path, NULL, NULL, 0, 0,
                           out_rc ? *out_rc : 0xFFu, 0);
        return tnfs_stat_result_to_attr_result(result);
    }
    if (out_st_attribs)
    {
        *out_st_attribs = tnfs_mode_to_st_attribs(stat.mode);
    }
    return SIDETNFS_ATTR_OK;
}

// Fase 7Lb: TNFS Fattrib set is unconditionally unsupported on this server.
//
// tnfsd 24.0522.1 registers command 0x27 but its tnfs_chmod() handler is
// empty and sends no response. TNFS Fattrib set is therefore reported as
// unsupported. STAT-based inquiry remains supported.
//
// No STAT is sent either (nothing to check -- no attribute here is ever
// going to be changed regardless of the file's current mode), no CHMOD is
// sent, no local attribute cache is written. *out_result_attribs is always
// 0 -- the caller (GEMDRVEMUL_FATTRIB_CALL in gemdrvemul.c) must map this
// result to GEMDOS_EACCDN and must not surface *out_result_attribs as if
// anything were actually changed. requested_attribs/mask are still
// accepted (and logged) so the diagnostic snapshot in DEBUG.TXT shows what
// was actually asked for.
SidetnfsAttrResult sidetnfs_tnfs_set_attributes(const char *tnfs_path, uint8_t requested_attribs, uint8_t mask,
                                                 uint8_t *out_result_attribs, uint8_t *out_rc, bool *out_unsupported)
{
    (void)requested_attribs;
    if (out_result_attribs)
    {
        *out_result_attribs = 0;
    }
    if (out_rc)
    {
        *out_rc = 0xFFu; // no wire traffic at all -- nothing to report
    }
    if (out_unsupported)
    {
        *out_unsupported = true;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FATTRIB_SET_UNSUPPORTED, 0, tnfs_path, NULL, NULL, requested_attribs, mask, 0, 0);
    return SIDETNFS_ATTR_ACCESS_DENIED;
}

// Fase 7M: Unix epoch seconds (UTC, as STAT's mtime field is confirmed to
// be) -> GEMDOS date/time, using this project's own established local-time
// policy (rtcemul.c's NTP handler adds get_utc_offset_seconds() to the NTP
// UTC timestamp before deriving the RTC's broken-down fields -- the RTC,
// and therefore every SD/FatFS file timestamp written via it, is kept in
// LOCAL time, not UTC). The same offset is applied here so a TNFS file's
// reported mtime lands on the same wall-clock value TOS/the SD route would
// show for an equivalent local timestamp -- never structurally off by the
// configured UTC offset. GEMDOS date/time bit layout matches FAT's exactly
// (see the existing GEMDRVEMUL_FDATETIME_CALL SD/FatFS route in
// gemdrvemul.c, which already copies FatFS's fdate/ftime straight through
// with no conversion for exactly this reason) -- no second, divergent
// conversion helper is introduced; this is the only place in the codebase
// that ever had to go from a Unix timestamp to this bit layout, since
// rtcemul.c only ever goes from Unix time to a struct tm (for the RTC
// peripheral), never to FAT/GEMDOS date/time words.
static void tnfs_unix_time_to_gemdos(uint32_t unix_time, uint16_t *out_date, uint16_t *out_time)
{
    time_t local_sec = (time_t)unix_time + (time_t)get_utc_offset_seconds();
    struct tm *local = gmtime(&local_sec);
    if (local == NULL)
    {
        *out_date = 0;
        *out_time = 0;
        return;
    }
    // GEMDOS date year field is 7 bits (0-127, i.e. 1980-2107) -- clamp
    // rather than error, since an out-of-range file timestamp is far more
    // likely to be bogus/edge-case server data than something GEMDOS/TOS
    // could ever act on differently anyway.
    int year = local->tm_year + 1900 - 1980;
    if (year < 0)
    {
        year = 0;
    }
    if (year > 127)
    {
        year = 127;
    }
    uint16_t date = (uint16_t)((year << 9) | (((unsigned)local->tm_mon + 1) << 5) | (unsigned)local->tm_mday);
    uint16_t time_ = (uint16_t)(((unsigned)local->tm_hour << 11) | ((unsigned)local->tm_min << 5) |
                                 ((unsigned)local->tm_sec / 2));
    *out_date = date;
    *out_time = time_;
}

// Fase 7M: TNFS STAT -> GEMDOS date/time, for Fdatime inquire (wflag 0).
// Reuses the same STAT call/response as sidetnfs_tnfs_get_attributes()
// above, reading mtime instead of mode. On any non-OK result,
// *out_gemdos_date/*out_gemdos_time/*out_unix_mtime are left untouched
// (see GEMDRVEMUL_FDATETIME_CALL in gemdrvemul.c, which never writes a
// partial date/time to shared memory on error). *out_unix_mtime (nullable)
// is the raw value STAT reported, for diagnostic logging upstream.
SidetnfsAttrResult sidetnfs_tnfs_get_datetime(int runtime_slot, const char *tnfs_path, uint16_t *out_gemdos_date,
                                               uint16_t *out_gemdos_time, uint32_t *out_unix_mtime, uint8_t *out_rc)
{
    SidetnfsTnfsRawStat stat = {0};
    SidetnfsTnfsStatResult result = tnfs_stat_raw(runtime_slot, tnfs_path, &stat, out_rc);
    if (result != SIDETNFS_TNFS_STAT_OK)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FDATIME_INQUIRE_ERR, 0, tnfs_path, NULL, NULL, 0, 0,
                           out_rc ? *out_rc : 0xFFu, 0);
        return tnfs_stat_result_to_attr_result(result);
    }
    uint16_t gemdos_date = 0;
    uint16_t gemdos_time = 0;
    tnfs_unix_time_to_gemdos(stat.mtime, &gemdos_date, &gemdos_time);
    if (out_gemdos_date)
    {
        *out_gemdos_date = gemdos_date;
    }
    if (out_gemdos_time)
    {
        *out_gemdos_time = gemdos_time;
    }
    if (out_unix_mtime)
    {
        *out_unix_mtime = stat.mtime;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FDATIME_INQUIRE_OK, 0, tnfs_path, NULL, NULL, gemdos_date, gemdos_time, 0, 0);
    return SIDETNFS_ATTR_OK;
}

// Fase 7M: TNFS Fdatime set (wflag 1) is unconditionally unsupported.
//
// The actual server's protocol header (tnfs.h) defines no UTIME/SETTIME
// command at all -- not even a reserved-but-empty opcode like
// TNFS_CHMODFILE (Fase 7Lb). tnfs_file.c likewise contains no function
// that calls any POSIX time-setting call (utime/utimes/utimensat/
// futimens) for any command. There is therefore no protocol-level
// mechanism whatsoever to persist a file's modification time via TNFS on
// this server -- this is a stronger case than Fattrib's CHMOD gap (which
// at least has a registered opcode), so no wire request is ever sent, no
// local time cache is kept, and no open/close or delete/recreate trick is
// used to fake it. The caller (GEMDRVEMUL_FDATETIME_CALL in gemdrvemul.c)
// must map this to a GEMDOS error and never surface the requested
// date/time as if it were actually persisted.
void sidetnfs_tnfs_set_datetime_unsupported(const char *tnfs_path, uint16_t requested_date, uint16_t requested_time)
{
    sidetnfs_diag_log(SIDETNFS_DIAG_FDATIME_SET_UNSUPPORTED, 0, tnfs_path, NULL, NULL, requested_date, requested_time,
                       0, 0);
}

// Fase 5O/5Q/6B: find_cache_slot()/alloc_cache_slot()/
// dir_cache_advance_one_step()/sidetnfs_dir_cache_service()/
// sidetnfs_dir_cache_is_ready()/sidetnfs_dir_cache_request()/
// sidetnfs_dir_cache_wait_ready() -- the RAM directory-cache build state
// machine -- have been removed. See
// SIDETNFS_PHASE5_DIRECTORY_LISTING.md/report.

static SidetnfsFakeSearchSlot *find_search_slot(uint32_t ndta)
{
    for (int i = 0; i < (int)SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (s_fake_searches[i].active && s_fake_searches[i].ndta == ndta)
        {
            return &s_fake_searches[i];
        }
    }
    return NULL;
}

static SidetnfsFakeSearchSlot *alloc_search_slot(void)
{
    for (int i = 0; i < (int)SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (!s_fake_searches[i].active)
        {
            return &s_fake_searches[i];
        }
    }
    return &s_fake_searches[0]; // extremely unlikely: evict the first slot
}

// Fase 5Q/6B: advance a fake no-network search -- exactly one synthetic
// entry ("NO_NETW.TXT") for root, nothing for any other path (see
// sidetnfs_fake_search_start()). Pure RAM access -- no network, ever.
static SidetnfsDirSearchResult fake_search_advance(SidetnfsFakeSearchSlot *search, SidetnfsAtariDirEntry *out_entry)
{
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
        sidetnfs_diag_log(SIDETNFS_DIAG_FAKE_FOUND, search->ndta, search->path, search->pattern, fake_entry.name, 0,
                           0, 0, fake_entry.attr);
        return SIDETNFS_DIR_SEARCH_FOUND;
    }
    search->active = false;
    sidetnfs_diag_log(SIDETNFS_DIAG_FAKE_NOT_FOUND, search->ndta, search->path, search->pattern, NULL, 0, 0, 0, 0);
    return SIDETNFS_DIR_SEARCH_NOT_FOUND;
}

// If search is currently active for a DIFFERENT ndta than the one about to
// claim it, log the overwrite. Fixed-size event struct has no room for
// both old+new ndta and old+new path in one event: old ndta/path go in the
// ndta/path fields, new path in the pattern field -- the new ndta itself
// shows up in the FSFIRST_ENTER event logged right after this one.
static void diag_log_search_overwrite_if_needed(const SidetnfsFakeSearchSlot *search, uint32_t new_ndta,
                                                  const char *new_path)
{
    if (search->active && search->ndta != new_ndta)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_SEARCH_OVERWRITE, search->ndta, search->path, new_path, NULL, 0, 0, 0, 0);
    }
}

SidetnfsDirSearchResult sidetnfs_fake_search_start(uint32_t ndta, const char *path,
                                                     const char *pattern, uint8_t attribs,
                                                     SidetnfsAtariDirEntry *out_entry)
{
    sidetnfs_diag_log(SIDETNFS_DIAG_FAKE_SEARCH_START, ndta, path, pattern, NULL, 0, 0, 0, attribs);

    SidetnfsFakeSearchSlot *search = find_search_slot(ndta);
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

    return fake_search_advance(search, out_entry);
}

SidetnfsDirSearchResult sidetnfs_fake_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry)
{
    SidetnfsFakeSearchSlot *search = find_search_slot(ndta);
    if (!search)
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    return fake_search_advance(search, out_entry);
}

bool sidetnfs_fake_search_is_active(uint32_t ndta)
{
    return find_search_slot(ndta) != NULL;
}

void sidetnfs_fake_search_close(uint32_t ndta)
{
    SidetnfsFakeSearchSlot *search = find_search_slot(ndta);
    if (search)
    {
        search->active = false;
    }
}

// Fase 9E: clear every active fake (no-network) directory search slot.
// Pure RAM, no network -- companion to sidetnfs_tnfs_dta_release_all()
// for the offline/no-network fallback listing path, used by
// sidetnfs_probe_reinit_active_server().
void sidetnfs_fake_search_close_all(void)
{
    for (unsigned i = 0; i < SIDETNFS_SEARCH_SLOTS; i++)
    {
        s_fake_searches[i].active = false;
    }
}

uint16_t sidetnfs_fake_search_count_active(void)
{
    uint16_t count = 0;
    for (unsigned i = 0; i < SIDETNFS_SEARCH_SLOTS; i++)
    {
        if (s_fake_searches[i].active)
        {
            count++;
        }
    }
    return count;
}

// Fase 9E: re-activate the SideTNFS drive-list config at the proven
// Atari-reset boundary -- see gemdrvemul.c, where this is called from
// GEMDRVEMUL_PING, but only the first PING after the very first one this
// Pico boot, and only when sidetnfs_config_is_pending() is true. Never
// blocks: the new mount probe below is the same fire-and-forget,
// non-blocking call used at Pico cold boot -- if the new server never
// responds, sidetnfs_tnfs_listing_ready() simply stays false and GEMDRIVE
// falls back to the existing no-network (fake NO_NETW.TXT) listing, same
// as any other offline boot; the command dispatch loop is never held up
// waiting for a response. Does not touch WiFi/cyw43 itself -- an Atari
// reset does not affect the Pico's own WiFi/NTP state, so wifi_connected
// must be supplied by the caller (already known there as
// sidetnfs_network_ok, determined once at Pico cold boot).
void sidetnfs_probe_reinit_active_server(bool wifi_connected)
{
    // Close every old TNFS directory search (a real CLOSEDIR per open
    // handle) and clear the offline fallback table -- neither may survive
    // into the new session.
    sidetnfs_tnfs_dta_release_all();
    sidetnfs_fake_search_close_all();

    // Reset mount/session identity only -- never the cumulative DEBUG.TXT
    // diagnostic counters elsewhere in s_state (those intentionally persist
    // across a reinit for continuity of the SELECT-button dump).
    s_state.mount_probe_sent = false;
    s_state.mount_response_received = false;
    s_state.sid = 0;
    s_state.mount_rc = 0;
    s_state.network_skipped = false;
    s_state.opendir_sent = false;
    s_state.opendir_response_received = false;
    s_state.debug_dirty = true;

    // Reload the persistent config and pick the active server/drive letter
    // from it, exactly like Pico cold boot (main.c) -- one shared path, no
    // second implementation.
    sidetnfs_config_init();
    sidetnfs_probe_load_active_server();

    if (!wifi_connected)
    {
        // No WiFi this boot -- same as the existing offline path; never
        // attempt a network call with no network.
        sidetnfs_mark_network_skipped();
        return;
    }

    if (!s_active_server_configured)
    {
        // No usable TNFS/UDP drive in the (re)loaded config -- nothing to
        // mount. network_skipped stays false (WiFi IS up), but with no
        // server to probe, mount_response_received simply stays false, so
        // sidetnfs_tnfs_listing_ready() stays false too and GEMDRIVE falls
        // back to the no-network listing -- never blocking.
        return;
    }

    // Fire the same non-blocking probe sequence used at Pico cold boot.
    // Never waits for a response here.
    sidetnfs_udp_connect_test();
    sidetnfs_send_mount_probe();
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

    // Fase 5N: short summary only, never a per-entry line -- independent of
    // the readdirx/sd-scan sections above since this is a separate,
    // backend-routed feature (see SIDETNFS_USE_TNFS_LISTING).
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

#if SIDETNFS_UART_DIAG_DUMP_ON_SELECT
// Temporary diagnostic build only (see report). Zero-initialized static
// instance -- all counters/last-values start at 0/empty and are only ever
// updated by plain scalar/array stores from gemdrvemul.c and the mount
// send/receive code below. No malloc, no ring buffer, no wraparound logic.
static SidetnfsUartDiagSnapshot s_uart_diag = {0};

SidetnfsUartDiagSnapshot *sidetnfs_uart_diag(void)
{
    return &s_uart_diag;
}

// Fase (BUGGYBGX/BULGX investigation): fixed 16-entry RING buffer -- see
// SidetnfsNameTraceEvent's own doc comment in sidetnfs_probe.h. Unlike
// s_diag_events above (stops at its cap, keeps the earliest events), this
// always overwrites the OLDEST entry once full, so a dump taken right
// after a corruption/crash shows the last 16 name events leading up to
// it.
static SidetnfsNameTraceEvent s_name_trace[SIDETNFS_NAME_TRACE_MAX_EVENTS];
static uint8_t s_name_trace_next = 0;
static uint8_t s_name_trace_filled = 0; // number of valid entries, caps at SIDETNFS_NAME_TRACE_MAX_EVENTS

static void copy_trace_name(char *dst, const char *src)
{
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, 14, "%s", src);
}

void sidetnfs_name_trace_log(SidetnfsNameTraceEventType event_type, uint32_t ndta, int32_t runtime_slot,
                              const char *raw_name, const char *converted_name, const char *dta_written_name,
                              const char *fsnext_returned_name)
{
    SidetnfsNameTraceEvent *e = &s_name_trace[s_name_trace_next];
    e->event_type = (uint8_t)event_type;
    e->ndta = ndta;
    e->runtime_slot = runtime_slot;
    copy_trace_name(e->raw_name, raw_name);
    copy_trace_name(e->converted_name, converted_name);
    copy_trace_name(e->dta_written_name, dta_written_name);
    copy_trace_name(e->fsnext_returned_name, fsnext_returned_name);

    s_name_trace_next = (uint8_t)((s_name_trace_next + 1) % SIDETNFS_NAME_TRACE_MAX_EVENTS);
    if (s_name_trace_filled < SIDETNFS_NAME_TRACE_MAX_EVENTS)
    {
        s_name_trace_filled++;
    }
}

int sidetnfs_tnfs_dta_get_runtime_slot(uint32_t ndta)
{
    SidetnfsTnfsDtaSearch *slot = lookupTnfsDTA(ndta);
    return slot ? slot->runtime_slot : -1;
}

// Fase (temporary diagnostic build): find the s_tnfs_dta_searches[] array
// index actually holding ndta's active TNFS DTA search, purely for the
// UART snapshot's "gevonden DTA-slot" field. Read-only -- does not affect
// lookupTnfsDTA()/alloc_tnfs_dta_slot()/any functional DTA-registry
// behavior at all.
int sidetnfs_uart_diag_find_dta_slot(uint32_t ndta)
{
    for (int i = 0; i < (int)SIDETNFS_TNFS_DTA_SLOTS; i++)
    {
        if (s_tnfs_dta_searches[i].active && s_tnfs_dta_searches[i].ndta == ndta)
        {
            return i;
        }
    }
    return -1;
}

void sidetnfs_uart_diag_dump(void)
{
    const SidetnfsUartDiagSnapshot *d = &s_uart_diag;

    printf("\r\n===== SIDETNFS UART DIAG SNAPSHOT =====\r\n");

    printf("drive_count: %lu\r\n", (unsigned long)d->drive_count);
    printf("drive_number_table:");
    for (uint32_t i = 0; i < (uint32_t)(SIDETNFS_MAX_DRIVES + 1); i++)
    {
        printf(" [%lu]=0x%08lx", (unsigned long)i, (unsigned long)d->drive_number_table[i]);
    }
    printf("\r\n");

    printf("slot0 mount: sent=%d response_received=%d rc=%u sid=0x%04x recv_seq=%u host=%s path=%s port=%u\r\n",
           (int)d->slot0_mount_sent, (int)d->slot0_mount_response_received,
           (unsigned)d->slot0_mount_rc, (unsigned)d->slot0_sid, (unsigned)d->slot0_last_recv_seq,
           d->slot0_host, d->slot0_mount_path, (unsigned)d->slot0_port);
    printf("slot1 mount: sent=%d response_received=%d rc=%u sid=0x%04x recv_seq=%u host=%s path=%s port=%u\r\n",
           (int)d->slot1_mount_sent, (int)d->slot1_mount_response_received,
           (unsigned)d->slot1_mount_rc, (unsigned)d->slot1_sid, (unsigned)d->slot1_last_recv_seq,
           d->slot1_host, d->slot1_mount_path, (unsigned)d->slot1_port);
    printf("mount pending_slot=%ld rejected_count=%lu last_reject_reason=%u\r\n",
           (long)d->mount_pending_slot, (unsigned long)d->mount_rejected_count,
           (unsigned)d->mount_last_reject_reason);

    printf("Dsetpath: calls=%lu last_slot=%ld last_input=\"%s\" last_normalized=\"%s\" last_result=0x%04x\r\n",
           (unsigned long)d->dsetpath_calls, (long)d->dsetpath_last_slot,
           d->dsetpath_last_input_path, d->dsetpath_last_normalized_path,
           (unsigned)d->dsetpath_last_result);

    printf("Dsetpath directory_exists: calls=%lu runtime_slot=%ld session_id=0x%04x host=%s port=%u "
           "tnfs_path=\"%s\" opendirx_seq=%u opendirx_response_received=%d opendirx_rc=0x%02x dir_handle=%u "
           "closedir_sent=%d closedir_response_received=%d closedir_rc=0x%02x\r\n",
           (unsigned long)d->dsetpath_exists_calls, (long)d->dsetpath_exists_runtime_slot,
           (unsigned)d->dsetpath_exists_session_id, d->dsetpath_exists_host, (unsigned)d->dsetpath_exists_port,
           d->dsetpath_exists_tnfs_path, (unsigned)d->dsetpath_exists_opendirx_seq,
           (int)d->dsetpath_exists_opendirx_response_received, (unsigned)d->dsetpath_exists_opendirx_rc,
           (unsigned)d->dsetpath_exists_dir_handle, (int)d->dsetpath_exists_closedir_sent,
           (int)d->dsetpath_exists_closedir_response_received, (unsigned)d->dsetpath_exists_closedir_rc);

    printf("Dfree: calls=%lu last_drive=%lu last_slot=%ld last_status=0x%08lx\r\n",
           (unsigned long)d->dfree_calls, (unsigned long)d->dfree_last_drive_number,
           (long)d->dfree_last_slot, (unsigned long)d->dfree_last_status);
    printf("Dfree geometry: free_clusters=%lu total_clusters=%lu bytes_per_sector=%lu "
           "sectors_per_cluster=%lu capacity_bytes=%llu\r\n",
           (unsigned long)d->dfree_last_free_clusters, (unsigned long)d->dfree_last_total_clusters,
           (unsigned long)d->dfree_last_bytes_per_sector, (unsigned long)d->dfree_last_sectors_per_cluster,
           (unsigned long long)d->dfree_last_capacity_bytes);
    printf("Dfree buffer: base=0x%08lx phase=%u bytes_written=%lu\r\n",
           (unsigned long)d->dfree_last_buffer_address, (unsigned)d->dfree_last_handler_phase,
           (unsigned long)d->dfree_last_bytes_written);
    printf("Dfree swapped (post-WRITE_AND_SWAP_LONGWORD, as stored): free=0x%08lx@0x%08lx "
           "total=0x%08lx@0x%08lx bytes_per_sector=0x%08lx@0x%08lx sectors_per_cluster=0x%08lx@0x%08lx\r\n",
           (unsigned long)d->dfree_last_swapped_free_clusters, (unsigned long)d->dfree_last_write_addr_free,
           (unsigned long)d->dfree_last_swapped_total_clusters, (unsigned long)d->dfree_last_write_addr_total,
           (unsigned long)d->dfree_last_swapped_bytes_per_sector,
           (unsigned long)d->dfree_last_write_addr_bytes_per_sector,
           (unsigned long)d->dfree_last_swapped_sectors_per_cluster,
           (unsigned long)d->dfree_last_write_addr_sectors_per_cluster);

    printf("Fopen: calls=%lu input=\"%s\" normalized=\"%s\" drive=%c rom_slot=%ld prefix_slot=%ld "
           "consistency_ok=%u session_id=0x%04x tnfs_rc=0x%02x tnfs_handle=%u gemdos_handle=%u "
           "stored_backend=%u stored_slot=%ld result=0x%04x\r\n",
           (unsigned long)d->fopen_calls, d->fopen_last_input_path, d->fopen_last_normalized_path,
           d->fopen_last_drive_letter ? d->fopen_last_drive_letter : '-', (long)d->fopen_last_slot,
           (long)d->fopen_last_prefix_slot, (unsigned)d->fopen_last_consistency_ok,
           (unsigned)d->fopen_last_session_id, (unsigned)d->fopen_last_tnfs_rc, (unsigned)d->fopen_last_tnfs_handle,
           (unsigned)d->fopen_last_gemdos_handle, (unsigned)d->fopen_last_stored_backend,
           (long)d->fopen_last_stored_slot, (unsigned)d->fopen_last_result);
    printf("Fcreate: calls=%lu input=\"%s\" normalized=\"%s\" drive=%c rom_slot=%ld prefix_slot=%ld "
           "consistency_ok=%u session_id=0x%04x tnfs_rc=0x%02x tnfs_handle=%u gemdos_handle=%u "
           "stored_backend=%u stored_slot=%ld result=0x%04x\r\n",
           (unsigned long)d->fcreate_calls, d->fcreate_last_input_path, d->fcreate_last_normalized_path,
           d->fcreate_last_drive_letter ? d->fcreate_last_drive_letter : '-', (long)d->fcreate_last_slot,
           (long)d->fcreate_last_prefix_slot, (unsigned)d->fcreate_last_consistency_ok,
           (unsigned)d->fcreate_last_session_id, (unsigned)d->fcreate_last_tnfs_rc,
           (unsigned)d->fcreate_last_tnfs_handle, (unsigned)d->fcreate_last_gemdos_handle,
           (unsigned)d->fcreate_last_stored_backend, (long)d->fcreate_last_stored_slot,
           (unsigned)d->fcreate_last_result);

    printf("Dgetpath: calls=%lu last_drive=%lu last_slot=%ld last_path=\"%s\"\r\n",
           (unsigned long)d->dgetpath_calls, (unsigned long)d->dgetpath_last_drive_number,
           (long)d->dgetpath_last_slot, d->dgetpath_last_path);

    printf("Fsfirst: calls=%lu last_slot=%ld last_attribs=0x%02x last_searchpath=\"%s\" "
           "last_validation_phase=%u last_result=0x%04x\r\n",
           (unsigned long)d->fsfirst_calls, (long)d->fsfirst_last_slot,
           (unsigned)d->fsfirst_last_attribs, d->fsfirst_last_searchpath,
           (unsigned)d->fsfirst_last_validation_phase, (unsigned)d->fsfirst_last_result);

    printf("Fsnext: calls=%lu last_dta_slot=%ld last_result=0x%04x\r\n",
           (unsigned long)d->fsnext_calls, (long)d->fsnext_last_dta_slot,
           (unsigned)d->fsnext_last_result);

    printf("Fread: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u "
           "requested=%u actual=%u result=0x%04x\r\n",
           (unsigned long)d->fread_calls, (unsigned)d->fread_last_gemdos_handle, (unsigned)d->fread_last_found,
           (unsigned)d->fread_last_backend, (long)d->fread_last_stored_slot, (unsigned)d->fread_last_tnfs_handle,
           (unsigned)d->fread_last_requested, (unsigned)d->fread_last_actual, (unsigned)d->fread_last_result);

    printf("Fwrite: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u "
           "requested=%u actual=%u tnfs_rc=0x%02x result=0x%04x\r\n",
           (unsigned long)d->fwrite_calls, (unsigned)d->fwrite_last_gemdos_handle, (unsigned)d->fwrite_last_found,
           (unsigned)d->fwrite_last_backend, (long)d->fwrite_last_stored_slot, (unsigned)d->fwrite_last_tnfs_handle,
           (unsigned)d->fwrite_last_requested, (unsigned)d->fwrite_last_actual, (unsigned)d->fwrite_last_tnfs_rc,
           (unsigned)d->fwrite_last_result);

    printf("Fseek: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u mode=%u "
           "offset_in=%ld offset_out=%lu tnfs_rc=0x%02x result=0x%04x\r\n",
           (unsigned long)d->fseek_calls, (unsigned)d->fseek_last_gemdos_handle, (unsigned)d->fseek_last_found,
           (unsigned)d->fseek_last_backend, (long)d->fseek_last_stored_slot, (unsigned)d->fseek_last_tnfs_handle,
           (unsigned)d->fseek_last_mode, (long)d->fseek_last_offset_in, (unsigned long)d->fseek_last_offset_out,
           (unsigned)d->fseek_last_tnfs_rc, (unsigned)d->fseek_last_result);

    printf("Fclose: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u result=0x%04x\r\n",
           (unsigned long)d->fclose_calls, (unsigned)d->fclose_last_gemdos_handle, (unsigned)d->fclose_last_found,
           (unsigned)d->fclose_last_backend, (long)d->fclose_last_stored_slot, (unsigned)d->fclose_last_tnfs_handle,
           (unsigned)d->fclose_last_result);

    printf("Fdatime: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld flag=%u tnfs_rc=0x%02x "
           "result=0x%04x\r\n",
           (unsigned long)d->fdatime_calls, (unsigned)d->fdatime_last_gemdos_handle, (unsigned)d->fdatime_last_found,
           (unsigned)d->fdatime_last_backend, (long)d->fdatime_last_stored_slot, (unsigned)d->fdatime_last_flag,
           (unsigned)d->fdatime_last_tnfs_rc, (unsigned)d->fdatime_last_result);

    printf("Dcreate: calls=%lu rom_slot=%ld result=0x%04x\r\n", (unsigned long)d->dcreate_calls,
           (long)d->dcreate_last_rom_slot, (unsigned)d->dcreate_last_result);

    printf("Ddelete: calls=%lu rom_slot=%ld result=0x%04x\r\n", (unsigned long)d->ddelete_calls,
           (long)d->ddelete_last_rom_slot, (unsigned)d->ddelete_last_result);

    printf("Fdelete: calls=%lu rom_slot=%ld result=0x%04x\r\n", (unsigned long)d->fdelete_calls,
           (long)d->fdelete_last_rom_slot, (unsigned)d->fdelete_last_result);

    printf("Frename: calls=%lu rom_slot_src=%ld rom_slot_dst=%ld prefix_slot_src=%ld prefix_slot_dst=%ld "
           "result=0x%04x\r\n",
           (unsigned long)d->frename_calls, (long)d->frename_rom_slot_src, (long)d->frename_rom_slot_dst,
           (long)d->frename_prefix_slot_src, (long)d->frename_prefix_slot_dst, (unsigned)d->frename_last_result);

    printf("Fattrib: calls=%lu rom_slot=%ld prefix_slot=%ld result=0x%04x\r\n", (unsigned long)d->fattrib_calls,
           (long)d->fattrib_last_rom_slot, (long)d->fattrib_last_prefix_slot, (unsigned)d->fattrib_last_result);

    printf("===== END SNAPSHOT =====\r\n");
}

// Same content/format as sidetnfs_uart_diag_dump() above, written to
// <hd_folder>/DEBUG.TXT instead of UART -- see report (hardware's serial
// port wasn't usable). Same f_open/f_write/f_close shape as
// sidetnfs_diag_dump_on_select() elsewhere in this file.
void sidetnfs_uart_diag_dump_to_file(const char *hd_folder)
{
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

    const SidetnfsUartDiagSnapshot *d = &s_uart_diag;
    char line[384];
    UINT written;

    int len = snprintf(line, sizeof(line), "===== SIDETNFS UART DIAG SNAPSHOT =====\r\n");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "drive_count: %lu\r\ndrive_number_table:",
                    (unsigned long)d->drive_count);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    for (uint32_t i = 0; i < (uint32_t)(SIDETNFS_MAX_DRIVES + 1); i++)
    {
        len = snprintf(line, sizeof(line), " [%lu]=0x%08lx", (unsigned long)i,
                        (unsigned long)d->drive_number_table[i]);
        if (len > 0)
        {
            f_write(&file, line, (UINT)len, &written);
        }
    }
    len = snprintf(line, sizeof(line), "\r\n");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "slot0 mount: sent=%d response_received=%d rc=%u sid=0x%04x recv_seq=%u host=%s path=%s port=%u\r\n",
                    (int)d->slot0_mount_sent, (int)d->slot0_mount_response_received,
                    (unsigned)d->slot0_mount_rc, (unsigned)d->slot0_sid, (unsigned)d->slot0_last_recv_seq,
                    d->slot0_host, d->slot0_mount_path, (unsigned)d->slot0_port);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line),
                    "slot1 mount: sent=%d response_received=%d rc=%u sid=0x%04x recv_seq=%u host=%s path=%s port=%u\r\n",
                    (int)d->slot1_mount_sent, (int)d->slot1_mount_response_received,
                    (unsigned)d->slot1_mount_rc, (unsigned)d->slot1_sid, (unsigned)d->slot1_last_recv_seq,
                    d->slot1_host, d->slot1_mount_path, (unsigned)d->slot1_port);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line), "mount pending_slot=%ld rejected_count=%lu last_reject_reason=%u\r\n",
                    (long)d->mount_pending_slot, (unsigned long)d->mount_rejected_count,
                    (unsigned)d->mount_last_reject_reason);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Dsetpath: calls=%lu last_slot=%ld last_input=\"%s\" last_normalized=\"%s\" last_result=0x%04x\r\n",
                    (unsigned long)d->dsetpath_calls, (long)d->dsetpath_last_slot,
                    d->dsetpath_last_input_path, d->dsetpath_last_normalized_path,
                    (unsigned)d->dsetpath_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Dsetpath directory_exists: calls=%lu runtime_slot=%ld session_id=0x%04x host=%s port=%u "
                    "tnfs_path=\"%s\" opendirx_seq=%u opendirx_response_received=%d opendirx_rc=0x%02x "
                    "dir_handle=%u closedir_sent=%d closedir_response_received=%d closedir_rc=0x%02x\r\n",
                    (unsigned long)d->dsetpath_exists_calls, (long)d->dsetpath_exists_runtime_slot,
                    (unsigned)d->dsetpath_exists_session_id, d->dsetpath_exists_host,
                    (unsigned)d->dsetpath_exists_port, d->dsetpath_exists_tnfs_path,
                    (unsigned)d->dsetpath_exists_opendirx_seq, (int)d->dsetpath_exists_opendirx_response_received,
                    (unsigned)d->dsetpath_exists_opendirx_rc, (unsigned)d->dsetpath_exists_dir_handle,
                    (int)d->dsetpath_exists_closedir_sent, (int)d->dsetpath_exists_closedir_response_received,
                    (unsigned)d->dsetpath_exists_closedir_rc);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Dfree: calls=%lu last_drive=%lu last_slot=%ld last_status=0x%08lx\r\n",
                    (unsigned long)d->dfree_calls, (unsigned long)d->dfree_last_drive_number,
                    (long)d->dfree_last_slot, (unsigned long)d->dfree_last_status);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line),
                    "Dfree geometry: free_clusters=%lu total_clusters=%lu bytes_per_sector=%lu "
                    "sectors_per_cluster=%lu capacity_bytes=%llu\r\n",
                    (unsigned long)d->dfree_last_free_clusters, (unsigned long)d->dfree_last_total_clusters,
                    (unsigned long)d->dfree_last_bytes_per_sector, (unsigned long)d->dfree_last_sectors_per_cluster,
                    (unsigned long long)d->dfree_last_capacity_bytes);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line), "Dfree buffer: base=0x%08lx phase=%u bytes_written=%lu\r\n",
                    (unsigned long)d->dfree_last_buffer_address, (unsigned)d->dfree_last_handler_phase,
                    (unsigned long)d->dfree_last_bytes_written);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line),
                    "Dfree swapped (post-WRITE_AND_SWAP_LONGWORD, as stored): free=0x%08lx@0x%08lx "
                    "total=0x%08lx@0x%08lx bytes_per_sector=0x%08lx@0x%08lx sectors_per_cluster=0x%08lx@0x%08lx\r\n",
                    (unsigned long)d->dfree_last_swapped_free_clusters, (unsigned long)d->dfree_last_write_addr_free,
                    (unsigned long)d->dfree_last_swapped_total_clusters, (unsigned long)d->dfree_last_write_addr_total,
                    (unsigned long)d->dfree_last_swapped_bytes_per_sector,
                    (unsigned long)d->dfree_last_write_addr_bytes_per_sector,
                    (unsigned long)d->dfree_last_swapped_sectors_per_cluster,
                    (unsigned long)d->dfree_last_write_addr_sectors_per_cluster);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line),
                    "Fopen: calls=%lu input=\"%s\" normalized=\"%s\" drive=%c rom_slot=%ld prefix_slot=%ld "
                    "consistency_ok=%u session_id=0x%04x tnfs_rc=0x%02x tnfs_handle=%u gemdos_handle=%u "
                    "stored_backend=%u stored_slot=%ld result=0x%04x\r\n",
                    (unsigned long)d->fopen_calls, d->fopen_last_input_path, d->fopen_last_normalized_path,
                    d->fopen_last_drive_letter ? d->fopen_last_drive_letter : '-', (long)d->fopen_last_slot,
                    (long)d->fopen_last_prefix_slot, (unsigned)d->fopen_last_consistency_ok,
                    (unsigned)d->fopen_last_session_id, (unsigned)d->fopen_last_tnfs_rc,
                    (unsigned)d->fopen_last_tnfs_handle, (unsigned)d->fopen_last_gemdos_handle,
                    (unsigned)d->fopen_last_stored_backend, (long)d->fopen_last_stored_slot,
                    (unsigned)d->fopen_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    len = snprintf(line, sizeof(line),
                    "Fcreate: calls=%lu input=\"%s\" normalized=\"%s\" drive=%c rom_slot=%ld prefix_slot=%ld "
                    "consistency_ok=%u session_id=0x%04x tnfs_rc=0x%02x tnfs_handle=%u gemdos_handle=%u "
                    "stored_backend=%u stored_slot=%ld result=0x%04x\r\n",
                    (unsigned long)d->fcreate_calls, d->fcreate_last_input_path, d->fcreate_last_normalized_path,
                    d->fcreate_last_drive_letter ? d->fcreate_last_drive_letter : '-', (long)d->fcreate_last_slot,
                    (long)d->fcreate_last_prefix_slot, (unsigned)d->fcreate_last_consistency_ok,
                    (unsigned)d->fcreate_last_session_id, (unsigned)d->fcreate_last_tnfs_rc,
                    (unsigned)d->fcreate_last_tnfs_handle, (unsigned)d->fcreate_last_gemdos_handle,
                    (unsigned)d->fcreate_last_stored_backend, (long)d->fcreate_last_stored_slot,
                    (unsigned)d->fcreate_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Dgetpath: calls=%lu last_drive=%lu last_slot=%ld last_path=\"%s\"\r\n",
                    (unsigned long)d->dgetpath_calls, (unsigned long)d->dgetpath_last_drive_number,
                    (long)d->dgetpath_last_slot, d->dgetpath_last_path);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Fsfirst: calls=%lu last_slot=%ld last_attribs=0x%02x last_searchpath=\"%s\" "
                    "last_validation_phase=%u last_result=0x%04x\r\n",
                    (unsigned long)d->fsfirst_calls, (long)d->fsfirst_last_slot,
                    (unsigned)d->fsfirst_last_attribs, d->fsfirst_last_searchpath,
                    (unsigned)d->fsfirst_last_validation_phase, (unsigned)d->fsfirst_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Fsnext: calls=%lu last_dta_slot=%ld last_result=0x%04x\r\n",
                    (unsigned long)d->fsnext_calls, (long)d->fsnext_last_dta_slot,
                    (unsigned)d->fsnext_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Fread: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u "
                    "requested=%u actual=%u result=0x%04x\r\n",
                    (unsigned long)d->fread_calls, (unsigned)d->fread_last_gemdos_handle,
                    (unsigned)d->fread_last_found, (unsigned)d->fread_last_backend, (long)d->fread_last_stored_slot,
                    (unsigned)d->fread_last_tnfs_handle, (unsigned)d->fread_last_requested,
                    (unsigned)d->fread_last_actual, (unsigned)d->fread_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Fwrite: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u "
                    "requested=%u actual=%u tnfs_rc=0x%02x result=0x%04x\r\n",
                    (unsigned long)d->fwrite_calls, (unsigned)d->fwrite_last_gemdos_handle,
                    (unsigned)d->fwrite_last_found, (unsigned)d->fwrite_last_backend,
                    (long)d->fwrite_last_stored_slot, (unsigned)d->fwrite_last_tnfs_handle,
                    (unsigned)d->fwrite_last_requested, (unsigned)d->fwrite_last_actual,
                    (unsigned)d->fwrite_last_tnfs_rc, (unsigned)d->fwrite_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Fseek: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u mode=%u "
                    "offset_in=%ld offset_out=%lu tnfs_rc=0x%02x result=0x%04x\r\n",
                    (unsigned long)d->fseek_calls, (unsigned)d->fseek_last_gemdos_handle,
                    (unsigned)d->fseek_last_found, (unsigned)d->fseek_last_backend, (long)d->fseek_last_stored_slot,
                    (unsigned)d->fseek_last_tnfs_handle, (unsigned)d->fseek_last_mode,
                    (long)d->fseek_last_offset_in, (unsigned long)d->fseek_last_offset_out,
                    (unsigned)d->fseek_last_tnfs_rc, (unsigned)d->fseek_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Fclose: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld tnfs_handle=%u "
                    "result=0x%04x\r\n",
                    (unsigned long)d->fclose_calls, (unsigned)d->fclose_last_gemdos_handle,
                    (unsigned)d->fclose_last_found, (unsigned)d->fclose_last_backend,
                    (long)d->fclose_last_stored_slot, (unsigned)d->fclose_last_tnfs_handle,
                    (unsigned)d->fclose_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Fdatime: calls=%lu gemdos_handle=%u found=%u backend=%u stored_slot=%ld flag=%u tnfs_rc=0x%02x "
                    "result=0x%04x\r\n",
                    (unsigned long)d->fdatime_calls, (unsigned)d->fdatime_last_gemdos_handle,
                    (unsigned)d->fdatime_last_found, (unsigned)d->fdatime_last_backend,
                    (long)d->fdatime_last_stored_slot, (unsigned)d->fdatime_last_flag,
                    (unsigned)d->fdatime_last_tnfs_rc, (unsigned)d->fdatime_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Dcreate: calls=%lu rom_slot=%ld result=0x%04x\r\n",
                    (unsigned long)d->dcreate_calls, (long)d->dcreate_last_rom_slot,
                    (unsigned)d->dcreate_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Ddelete: calls=%lu rom_slot=%ld result=0x%04x\r\n",
                    (unsigned long)d->ddelete_calls, (long)d->ddelete_last_rom_slot,
                    (unsigned)d->ddelete_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Fdelete: calls=%lu rom_slot=%ld result=0x%04x\r\n",
                    (unsigned long)d->fdelete_calls, (long)d->fdelete_last_rom_slot,
                    (unsigned)d->fdelete_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line),
                    "Frename: calls=%lu rom_slot_src=%ld rom_slot_dst=%ld prefix_slot_src=%ld prefix_slot_dst=%ld "
                    "result=0x%04x\r\n",
                    (unsigned long)d->frename_calls, (long)d->frename_rom_slot_src, (long)d->frename_rom_slot_dst,
                    (long)d->frename_prefix_slot_src, (long)d->frename_prefix_slot_dst,
                    (unsigned)d->frename_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "Fattrib: calls=%lu rom_slot=%ld prefix_slot=%ld result=0x%04x\r\n",
                    (unsigned long)d->fattrib_calls, (long)d->fattrib_last_rom_slot,
                    (long)d->fattrib_last_prefix_slot, (unsigned)d->fattrib_last_result);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    len = snprintf(line, sizeof(line), "===== END SNAPSHOT =====\r\n");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    // Fase (BUGGYBGX/BULGX investigation): the 16-entry name-trace ring
    // buffer, oldest-first (so the last line printed is the most recent
    // event -- the one closest to any corruption/crash). SD only, never
    // UART -- see sidetnfs_name_trace_log()'s own comment.
    len = snprintf(line, sizeof(line), "\r\n===== NAME TRACE (last %u events, oldest first) =====\r\n",
                    (unsigned)s_name_trace_filled);
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }
    uint8_t trace_start = (s_name_trace_filled < SIDETNFS_NAME_TRACE_MAX_EVENTS)
                               ? 0
                               : s_name_trace_next; // oldest slot once the ring has wrapped
    for (uint8_t i = 0; i < s_name_trace_filled; i++)
    {
        uint8_t idx = (uint8_t)((trace_start + i) % SIDETNFS_NAME_TRACE_MAX_EVENTS);
        const SidetnfsNameTraceEvent *ev = &s_name_trace[idx];
        len = snprintf(line, sizeof(line),
                        "[%u] event=%u ndta=0x%08lx runtime_slot=%ld raw=\"%s\" converted=\"%s\" "
                        "dta_written=\"%s\" fsnext_returned=\"%s\"\r\n",
                        (unsigned)i, (unsigned)ev->event_type, (unsigned long)ev->ndta, (long)ev->runtime_slot,
                        ev->raw_name, ev->converted_name, ev->dta_written_name, ev->fsnext_returned_name);
        if (len > 0)
        {
            f_write(&file, line, (UINT)len, &written);
        }
    }
    len = snprintf(line, sizeof(line), "===== END NAME TRACE =====\r\n");
    if (len > 0)
    {
        f_write(&file, line, (UINT)len, &written);
    }

    f_close(&file);
}
#endif // SIDETNFS_UART_DIAG_DUMP_ON_SELECT
