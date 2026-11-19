/**
 * File: sidetnfs_charset.h
 * Description: File names between the Atari ST character set (GEMDOS) and
 * UTF-8 (what a TNFS server on a modern machine keeps).
 *
 * Pure functions, no Pico SDK or lwIP, so they can be tested on a host.
 * Ported from TheHulk (esp32/main/atari_charset.c).
 */
#ifndef SIDETNFS_CHARSET_H
#define SIDETNFS_CHARSET_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Converts an Atari character set string to UTF-8. Every Atari character
 * has a Unicode counterpart; a character that would not fit in `out`
 * (size bytes, NUL included) ends the string there.
 */
void sidetnfs_atari_to_utf8(const char *in, char *out, size_t size);

/**
 * Converts a UTF-8 string to the Atari character set. Returns false when a
 * character has no Atari counterpart, the input is not valid UTF-8 (or
 * uses 4-byte sequences), or `out` (size bytes, NUL included) is too small.
 */
bool sidetnfs_utf8_to_atari(const char *in, char *out, size_t size);

/** True when s contains a byte >= 0x80, i.e. needs converting at all. */
bool sidetnfs_charset_has_high_bytes(const char *s);

#endif // SIDETNFS_CHARSET_H
