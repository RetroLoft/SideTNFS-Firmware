#include "include/fs_backend.h"
#include "include/gemdrvemul.h"
#include <string.h>

// ─── Memory backend ───────────────────────────────────────────────────────────
//   Read-only, root directory only.
//   Implements the fs_backend.h interface for a single hardcoded file in RAM.
//   Replace or extend this file to add a real storage backend (TNFS, SD, …).

static const char readme_content[] =
    "SIDETNFS - Atari ST TNFS GEMDOS Network Drive\r\n"
    "Milestone 1 - Fake in-memory read-only filesystem\r\n"
    "This file is served from Raspberry Pi Pico W RAM.\r\n"
    "Future milestones will add TNFS network access.\r\n";
#define README_SIZE ((uint32_t)(sizeof(readme_content) - 1))

// FAT date 2024-01-01: year-1980=44 → (44<<9)|(1<<5)|1 = 0x5821
// FAT time 12:00:00:  (12<<11)                         = 0x6000
#define README_DATE 0x5821
#define README_TIME 0x6000
#define README_ATTR 0x20   // archive

static const FsEntry mem_root_entries[] = {
    { "README.TXT", README_SIZE, README_DATE, README_TIME, README_ATTR },
};
#define MEM_ENTRY_COUNT ((int)(sizeof(mem_root_entries) / sizeof(mem_root_entries[0])))

// ── Open handle pool (avoids dynamic allocation on the Pico) ──────────────────

struct FsHandle {
    uint32_t offset;
};

#define MEM_MAX_OPEN 16
static struct FsHandle mem_handles[MEM_MAX_OPEN];
static bool            mem_handle_used[MEM_MAX_OPEN];

// ── Internal helpers ──────────────────────────────────────────────────────────

static bool mem_wildmatch(const char *pat, const char *str)
{
    if (*pat == '*' && *(pat + 1) == '\0') return true;
    if (*pat == '\0' && *str == '\0')      return true;
    if (*pat == '\0' || *str == '\0')      return false;
    if (*pat == '?' || *pat == *str)       return mem_wildmatch(pat + 1, str + 1);
    if (*pat == '*')
        return mem_wildmatch(pat + 1, str) || mem_wildmatch(pat, str + 1);
    return false;
}

// ── Backend API ───────────────────────────────────────────────────────────────

void fs_init(void)
{
    memset(mem_handle_used, 0, sizeof(mem_handle_used));
}

bool fs_list_dir(const char *dir, const char *pat, int index, FsEntry *out)
{
    // Only root directory exists in the memory backend.
    bool is_root = (dir[0] == '\\' && (dir[1] == '\0' || (dir[1] == '\\' && dir[2] == '\0')));
    if (!is_root) return false;

    int match_count = 0;
    for (int i = 0; i < MEM_ENTRY_COUNT; i++) {
        if (mem_wildmatch(pat, mem_root_entries[i].name)) {
            if (match_count == index) {
                *out = mem_root_entries[i];
                return true;
            }
            match_count++;
        }
    }
    return false;
}

bool fs_stat(const char *path, FsEntry *out)
{
    for (int i = 0; i < MEM_ENTRY_COUNT; i++) {
        if (strcmp(path, mem_root_entries[i].name) == 0) {
            *out = mem_root_entries[i];
            return true;
        }
    }
    return false;
}

FsHandle *fs_open(const char *path, int16_t *gemdos_err)
{
    for (int i = 0; i < MEM_ENTRY_COUNT; i++) {
        if (strcmp(path, mem_root_entries[i].name) == 0) {
            for (int j = 0; j < MEM_MAX_OPEN; j++) {
                if (!mem_handle_used[j]) {
                    mem_handle_used[j]    = true;
                    mem_handles[j].offset = 0;
                    return &mem_handles[j];
                }
            }
            *gemdos_err = GEMDOS_ENHNDL;
            return NULL;
        }
    }
    *gemdos_err = GEMDOS_EFILNF;
    return NULL;
}

uint32_t fs_read(FsHandle *h, void *buf, uint32_t len)
{
    uint32_t avail = (h->offset < README_SIZE) ? (README_SIZE - h->offset) : 0;
    uint32_t n     = (len < avail) ? len : avail;
    if (n) {
        memcpy(buf, readme_content + h->offset, n);
        h->offset += n;
    }
    return n;
}

int32_t fs_seek(FsHandle *h, int32_t offset, int whence)
{
    int32_t new_pos;
    if      (whence == 0) new_pos = offset;
    else if (whence == 1) new_pos = (int32_t)h->offset + offset;
    else                  new_pos = (int32_t)README_SIZE + offset;

    if (new_pos < 0)                     new_pos = 0;
    if ((uint32_t)new_pos > README_SIZE) new_pos = (int32_t)README_SIZE;
    h->offset = (uint32_t)new_pos;
    return new_pos;
}

void fs_close(FsHandle *h)
{
    int slot = (int)(h - mem_handles);
    if (slot >= 0 && slot < MEM_MAX_OPEN)
        mem_handle_used[slot] = false;
}
