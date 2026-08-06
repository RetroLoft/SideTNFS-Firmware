/**
 * File: sidetnfs_update_check.h
 * Description: Firmware-update version check, triggered on demand by
 * SIDETNFS.PRG (GEMDRVEMUL_SIDETNFS_CHECK_UPDATE, commands.h). Fetches
 * https://raw.githubusercontent.com/RetroLoft/SideTNFS-Firmware/refs/heads/main/version.txt
 * over TLS and compares it against this firmware's own RELEASE_VERSION
 * (baked in at build time from version.txt -- see CMakeLists.txt).
 */
#ifndef SIDETNFS_UPDATE_CHECK_H
#define SIDETNFS_UPDATE_CHECK_H

#include <stddef.h>
#include <stdint.h>

// "v255.255.255" (12 chars) + NUL, rounded up to an even length for
// CHANGE_ENDIANESS_BLOCK16 -- comfortably more than any real version
// string this project uses.
#define SIDETNFS_UPDATE_VERSION_LEN 16

// GEMDRVEMUL_SIDETNFS_UPDATE_STATUS values (gemdrvemul.h).
#define SIDETNFS_UPDATE_STATUS_UP_TO_DATE 0u
#define SIDETNFS_UPDATE_STATUS_AVAILABLE 1u
#define SIDETNFS_UPDATE_STATUS_ERROR 2u

// Runs the whole check synchronously: DNS resolve, TLS connect, HTTP GET,
// response parse, version compare -- bounded by
// SIDETNFS_UPDATE_CHECK_TIMEOUT_MS total (sidetnfs_update_check.c), same
// "blocks the caller for its own fixed budget" contract already used by
// the boot-time NTP wait and SAVE_CONFIG. Calls cyw43_arch_poll()
// internally throughout the wait, exactly like every other network wait
// loop in this codebase -- safe to call from the main GEMDOS command
// dispatch loop.
//
// out_latest_version (may be NULL) receives the version string actually
// read from version.txt (e.g. "v1.0.2"), NUL-terminated, truncated to fit
// out_size if necessary; left as an empty string on
// SIDETNFS_UPDATE_STATUS_ERROR. Returns one of the
// SIDETNFS_UPDATE_STATUS_* values above.
uint32_t sidetnfs_update_check_run(char *out_latest_version, size_t out_size);

#endif // SIDETNFS_UPDATE_CHECK_H
