#ifndef DEBUG_H
#define DEBUG_H

// ─── Debug configuration ──────────────────────────────────────────────────────
//
// Set by CMake option -DSIDETNFS_DEBUG=ON (default) / OFF.
//
//   DEBUG build   (-DSIDETNFS_DEBUG=ON):
//     LOG(...)    → printf() — all status/protocol messages visible over USB
//     DPRINTF(...)→ fprintf(stderr, ...) with file:line:func prefix
//     USB serial enabled, 3-second USB enumeration wait in main()
//
//   PRODUCTION build (-DSIDETNFS_DEBUG=OFF):
//     LOG(...)    → (nothing)  — compiled out entirely
//     DPRINTF(...)→ (nothing)
//     USB serial disabled, no wait — ROM emulator starts immediately

#if defined(SIDETNFS_DEBUG) && (SIDETNFS_DEBUG != 0)

#include <stdio.h>
#include <string.h>

// General status/info messages (always shown in debug builds).
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)

// Verbose protocol tracing with file:line:function prefix.
#define DPRINTF(fmt, ...)                                                           \
    do {                                                                            \
        const char *_f = strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1       \
                                                : __FILE__;                         \
        fprintf(stderr, "%s:%d:%s(): " fmt, _f, __LINE__, __func__, ##__VA_ARGS__);\
    } while (0)

#define DPRINTFRAW(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

#else // PRODUCTION

#define LOG(fmt, ...)
#define DPRINTF(fmt, ...)
#define DPRINTFRAW(fmt, ...)

#endif // SIDETNFS_DEBUG

#endif // DEBUG_H
