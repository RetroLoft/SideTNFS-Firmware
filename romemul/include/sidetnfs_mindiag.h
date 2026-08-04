/**
 * File: sidetnfs_mindiag.h
 * Description: Minimal, fixed-size diagnostic state for the reproducible
 * "An error occurred while reading the source file" copy failure
 * (N:\...\EPSLQ\XELITE.IT). Replaces the earlier sidetnfs_copytrace module,
 * which used a 32-entry ring buffer and full per-call path copies and
 * proved unstable on real hardware.
 *
 * Every field here is a single plain static variable, updated only by a
 * simple assignment or increment -- no ring buffer, no malloc, no
 * snprintf/file-I/O/UART during a GEMDOS call. The only place any of this
 * is formatted or written to SD is sidetnfs_mindiag_dump_to_file(), called
 * exclusively from the SELECT-button edge-handler, after the failure has
 * already happened.
 *
 * Gated by SIDETNFS_ENABLE_MINDIAG, independent of SIDETNFS_ENABLE_DEBUG
 * and SIDETNFS_ENABLE_DIAG. When the switch is off, every function below
 * is a static inline no-op declared right here -- same technique as
 * sidetnfs_diag_log()'s SIDETNFS_ENABLE_DIAG gate in sidetnfs_probe.h --
 * so a plain Production build (SIDETNFS_ENABLE_MINDIAG=0 by default)
 * compiles every one of these call sites away under optimization: no
 * state, no function bodies, no per-call argument setup left in the
 * Fopen/Fread/Fclose hot paths.
 */
#ifndef SIDETNFS_MINDIAG_H
#define SIDETNFS_MINDIAG_H

#include <stdbool.h>
#include <stdint.h>

// Values for the "last operation" field -- deliberately not an enum typedef
// shared with anything else, this module is meant to be fully self
// contained and removable.
#define SIDETNFS_MINDIAG_OP_NONE 0u
#define SIDETNFS_MINDIAG_OP_FOPEN 1u
#define SIDETNFS_MINDIAG_OP_FREAD 2u
#define SIDETNFS_MINDIAG_OP_FCLOSE 3u

#if SIDETNFS_ENABLE_MINDIAG

/**
 * Increment the earliest-possible dispatch counter for `op` (one of the
 * SIDETNFS_MINDIAG_OP_* values above). Call as the literal first statement
 * of the FOPEN/READ_BUFF/FCLOSE command cases in the main command switch,
 * before any existing validation -- comparing this against the
 * corresponding handler_* counter below distinguishes "the Pico's command
 * loop never saw this command at all" from "it saw the command but the
 * backend handler rejected it before reaching its own logic".
 */
void sidetnfs_mindiag_note_dispatch(uint8_t op);

/**
 * Record one Fopen outcome: increments handler_fopen_count, updates the
 * last_* fields, and adjusts active_bindings (+1 on a successful open,
 * i.e. pico_result == 0). tnfs_handle/requested/actual are whatever is
 * known at this call site -- pass 0 for anything not applicable to Fopen.
 */
void sidetnfs_mindiag_note_fopen(uint16_t gemdos_handle, uint8_t tnfs_handle, uint8_t tnfs_rc, int32_t pico_result);

/**
 * Record one Fread outcome: increments handler_fread_count and updates the
 * last_* fields (including requested/actual byte counts and the TNFS rc).
 */
void sidetnfs_mindiag_note_fread(uint16_t gemdos_handle, uint8_t tnfs_handle, uint32_t requested_bytes,
                                    uint32_t actual_bytes, uint8_t tnfs_rc, int32_t pico_result);

/**
 * Record one Fclose outcome: increments handler_fclose_count, updates the
 * last_* fields, and adjusts active_bindings (-1 on a successful close,
 * i.e. pico_result == 0 and the binding actually existed).
 */
void sidetnfs_mindiag_note_fclose(uint16_t gemdos_handle, uint8_t tnfs_handle, int32_t pico_result,
                                     bool binding_existed);

/**
 * At most one bounded comparison: if `name` (a GEMDOS 8.3 DTA entry name,
 * e.g. SidetnfsAtariDirEntry.name) matches "XELITE.IT" and this is the
 * first time it's been seen this boot, records its advertised DTA size.
 * Safe to call for every directory entry GEMDOS is given -- a silent no-op
 * for every name that isn't XELITE.IT, and a no-op after the first match
 * (never overwrites once recorded). No path copying, no loop.
 */
void sidetnfs_mindiag_note_dta_entry(const char *name, uint32_t size);

/**
 * Write the fixed set of counters/last-values to
 * <hd_folder>/COPYTRACE.TXT. Every line is built with a compile-time-sized
 * stack buffer and a snprintf() call whose maximum possible length is
 * proven to fit that buffer; the returned length is always clamped to the
 * buffer size before f_write() regardless, so this can never read past the
 * buffer even if that budget is ever miscalculated later. Silently does
 * nothing if hd_folder is NULL or the SD write fails -- never crashes.
 * Call only from the SELECT-button edge-handler, after the failure has
 * already occurred.
 */
void sidetnfs_mindiag_dump_to_file(const char *hd_folder);

#else // !SIDETNFS_ENABLE_MINDIAG

static inline void sidetnfs_mindiag_note_dispatch(uint8_t op)
{
    (void)op;
}

static inline void sidetnfs_mindiag_note_fopen(uint16_t gemdos_handle, uint8_t tnfs_handle, uint8_t tnfs_rc,
                                                 int32_t pico_result)
{
    (void)gemdos_handle;
    (void)tnfs_handle;
    (void)tnfs_rc;
    (void)pico_result;
}

static inline void sidetnfs_mindiag_note_fread(uint16_t gemdos_handle, uint8_t tnfs_handle, uint32_t requested_bytes,
                                                 uint32_t actual_bytes, uint8_t tnfs_rc, int32_t pico_result)
{
    (void)gemdos_handle;
    (void)tnfs_handle;
    (void)requested_bytes;
    (void)actual_bytes;
    (void)tnfs_rc;
    (void)pico_result;
}

static inline void sidetnfs_mindiag_note_fclose(uint16_t gemdos_handle, uint8_t tnfs_handle, int32_t pico_result,
                                                  bool binding_existed)
{
    (void)gemdos_handle;
    (void)tnfs_handle;
    (void)pico_result;
    (void)binding_existed;
}

static inline void sidetnfs_mindiag_note_dta_entry(const char *name, uint32_t size)
{
    (void)name;
    (void)size;
}

static inline void sidetnfs_mindiag_dump_to_file(const char *hd_folder)
{
    (void)hd_folder;
}

#endif // SIDETNFS_ENABLE_MINDIAG

#endif // SIDETNFS_MINDIAG_H
