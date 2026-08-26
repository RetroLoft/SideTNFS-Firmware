#ifndef DEBUG_H
#define DEBUG_H

/**
 * @brief A macro to print debug
 *
 * @param fmt The format string for the debug message, similar to printf.
 * @param ... Variadic arguments corresponding to the format specifiers in the fmt parameter.
 */
#if defined(_DEBUG) && (_DEBUG != 0)
#include <string.h>
// Named __dprintf_srcfile, not "file": many call sites pass a local
// variable literally named `file` (e.g. FileDescriptors *file) as one of
// the variadic arguments below, and a plainer name here would shadow it
// for the rest of this expansion -- silently rebinding `file->x` in the
// caller's own argument list to this macro-local `const char *` instead.
#define DPRINTF(fmt, ...)                                                                              \
    do                                                                                                 \
    {                                                                                                  \
        const char *__dprintf_srcfile = strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__; \
        fprintf(stderr, "%s:%d:%s(): " fmt "", __dprintf_srcfile,                                       \
                __LINE__, __func__, ##__VA_ARGS__);                                                     \
    } while (0)
#define DPRINTFRAW(fmt, ...)                 \
    do                                       \
    {                                        \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
    } while (0)

#else
#define DPRINTF(fmt, ...)
#define DPRINTFRAW(fmt, ...)
#endif

/**
 * Diagnostic tuning knobs for the SELECT-button dump (EVENTLOG.TXT/
 * SNAPSHOT.TXT, see sidetnfs_probe.h/.c). Every macro below only changes
 * WHAT gets recorded into the fixed-size RAM eventlog, never any control
 * flow -- and the eventlog is only ever written to a file in the debug
 * build (SIDETNFS_ENABLE_DEBUG=1, see build.sh/CMakeLists.txt), so in a
 * Production build these knobs have no observable effect at all.
 *
 * SIDETNFS_DIAG_MAX_EVENTS is a hard budget: once the eventlog fills up it
 * stops recording and keeps the earliest events. The six FOCUS_* switches
 * below all trade the same scarce budget one way -- narrowing
 * GEMDRVEMUL_COMMAND_ENTER logging to a specific command whitelist and/or
 * suppressing per-entry directory-listing detail (TNFS_READDIRX_ONE/ENTRY/
 * SKIP/MATCH/EOF, TNFS_OPENDIRX_OK) -- so whichever operation you're
 * actually chasing gets to keep more of the 256 slots instead of losing
 * them to an unrelated directory refresh.
 */

// Fixed-size RAM eventlog capacity (sidetnfs_diag_log(), sidetnfs_probe.c).
// Stops recording once full and keeps the earliest events -- not a ring
// buffer -- so raising this is the only way to still see a bug that only
// shows up after a long session.
#ifndef SIDETNFS_DIAG_MAX_EVENTS
#define SIDETNFS_DIAG_MAX_EVENTS 256
#endif

// File-I/O focus: full per-round TNFS READ detail
// (SIDETNFS_DIAG_FREAD_TNFS_READ/READ_BUFF_TNFS_RC inside the internal
// chunk-loop), at the cost of suppressing directory-listing detail events.
// Off while chasing the v1.0.3 TNFS-reliability-fix regression: two real
// captures now show this per-round detail clean (rc=0 every round) across
// two full files, each burning ~200 of the 256-slot budget on 200-byte-
// chunk rounds that never disagree with each other -- the crash itself
// happens later than the budget can still reach with this on. Re-enable
// if a read failure (not the bomb crash) needs the per-round detail again.
#ifndef SIDETNFS_DEBUG_FOCUS_FILE_IO
#define SIDETNFS_DEBUG_FOCUS_FILE_IO 0
#endif

// Fseek focus: narrows GEMDRVEMUL_COMMAND_ENTER logging to
// Fopen/Fread/Fclose/Fseek/Fattrib/Fdatetime/Dgetpath/Dsetpath, and
// suppresses directory-listing detail the same way as FILE_IO above.
#ifndef SIDETNFS_DEBUG_FOCUS_FSEEK
#define SIDETNFS_DEBUG_FOCUS_FSEEK 1
#endif

// Fdelete focus: adds GEMDRVEMUL_FDELETE_CALL to the COMMAND_ENTER
// whitelist (composed with FSEEK's own -- either focus mode being on is
// enough to show its own commands) and suppresses directory-listing
// detail the same way. Off while chasing the v1.0.3 TNFS-reliability-fix
// regression (Fdelete never fires during a Pexec/.PRG load) -- re-enable
// if that changes.
#ifndef SIDETNFS_DEBUG_FOCUS_FDELETE
#define SIDETNFS_DEBUG_FOCUS_FDELETE 0
#endif

// Frename focus: same contract as FSEEK/FDELETE, for FRENAME. Off, same
// reason as FDELETE above.
#ifndef SIDETNFS_DEBUG_FOCUS_FRENAME
#define SIDETNFS_DEBUG_FOCUS_FRENAME 0
#endif

// Dcreate focus: same contract as FSEEK/FDELETE/FRENAME, for DCREATE. Off,
// same reason as FDELETE above.
#ifndef SIDETNFS_DEBUG_FOCUS_DCREATE
#define SIDETNFS_DEBUG_FOCUS_DCREATE 0
#endif

// Ddelete focus: same contract as FSEEK/FDELETE/FRENAME/DCREATE, for
// DDELETE. Off, same reason as FDELETE above.
#ifndef SIDETNFS_DEBUG_FOCUS_DDELETE
#define SIDETNFS_DEBUG_FOCUS_DDELETE 0
#endif

// True when ANY focus mode above is on -- directory-listing detail events
// are suppressed as soon as one of them wants the room. See each flag's
// own comment for what it adds back in exchange.
#define SIDETNFS_DEBUG_SUPPRESS_DIR_DETAIL                                                                        \
    (SIDETNFS_DEBUG_FOCUS_FILE_IO || SIDETNFS_DEBUG_FOCUS_FSEEK || SIDETNFS_DEBUG_FOCUS_FDELETE ||                \
     SIDETNFS_DEBUG_FOCUS_FRENAME || SIDETNFS_DEBUG_FOCUS_DCREATE || SIDETNFS_DEBUG_FOCUS_DDELETE)

#endif // DEBUG_H