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

#endif // SIDETNFS_PROBE_H
