#ifndef FS_BACKEND_H_
#define FS_BACKEND_H_

// ─── Filesystem backend interface ────────────────────────────────────────────
//
//   Atari GEMDRIVE protocol layer  (gemdrvemul.c)
//       → filesystem backend interface  (fs_backend.h)
//           → memory backend            (fs_memory.c)   ← active now
//           → TNFS backend              (fs_tnfs.c)     ← future
//
// The protocol layer calls only these functions. It does not know whether
// files come from memory, SD card, TNFS, flash, or anything else.

#include <stdint.h>
#include <stdbool.h>

// Filesystem entry descriptor returned by fs_list_dir() and fs_stat().
typedef struct {
    char     name[14];   // 8.3 filename, uppercase, null-terminated
    uint32_t size;       // file size in bytes
    uint16_t date;       // FAT date: bits[15:9]=year-1980, [8:5]=month, [4:0]=day
    uint16_t time;       // FAT time: bits[15:11]=hour, [10:5]=min, [4:0]=sec/2
    uint8_t  attr;       // GEMDOS file attribute byte (0x20=archive, 0x10=dir, …)
} FsEntry;

// Opaque file handle; each backend defines its own struct FsHandle body.
typedef struct FsHandle FsHandle;

// Initialise the active backend.
void fs_init(void);

// Enumerate directory entries whose name matches pat (uppercase wildcard, e.g. "*.TXT").
// dir:   backslash-separated directory path relative to drive root (e.g. "\").
// index: 0-based iteration counter; caller increments between successive calls.
// out:   filled on success.
// Returns true and fills *out when a match is found; false when enumeration is exhausted.
bool fs_list_dir(const char *dir, const char *pat, int index, FsEntry *out);

// Stat a single file. path has no drive letter and no leading backslash.
// Returns true and fills *out on success.
bool fs_stat(const char *path, FsEntry *out);

// Open path (no drive letter, no leading backslash) for reading.
// On failure sets *gemdos_err to a GEMDOS error code and returns NULL.
FsHandle *fs_open(const char *path, int16_t *gemdos_err);

// Read up to len bytes into buf from the current file position.
// Returns the number of bytes actually read (0 = EOF).
uint32_t fs_read(FsHandle *h, void *buf, uint32_t len);

// Seek within an open file.  whence: 0 = SET, 1 = CUR, 2 = END.
// Returns the new byte offset on success, or a negative GEMDOS error code.
int32_t fs_seek(FsHandle *h, int32_t offset, int whence);

// Close handle and release any resources held by the backend.
void fs_close(FsHandle *h);

#endif // FS_BACKEND_H_
