/**
 * File: sidetnfs_sd_service.c
 * (SD-service en SD_ERROR.TXT) -- see sidetnfs_sd_service.h for the
 * full design comment. Only touches the SD card/FatFS here at boot, at
 * most once (sidetnfs_sd_service_run()'s own guard) -- never a real
 * GEMDOS file/directory backend .
 */
#include "include/sidetnfs_sd_service.h"

#include <string.h>
#include <stdio.h>

#include "sd_card.h"
#include "f_util.h"          // ff.h (FRESULT/FATFS/FILINFO/AM_DIR), FRESULT_str()
#include "include/filesys.h" // MAX_FOLDER_LENGTH

static bool s_service_ran = false;
static sidetnfs_sd_status_t s_global_status = SIDETNFS_SD_STATUS_ABSENT;
static uint8_t s_global_fresult = 0xFFu;
static FATFS s_fatfs;

static sidetnfs_sd_drive_status_t s_slots[SIDETNFS_SD_SERVICE_MAX_SLOTS];

void sidetnfs_sd_service_set_slot_path(int slot, char driveletter, const char *sd_path)
{
    if (slot < 0 || slot >= SIDETNFS_SD_SERVICE_MAX_SLOTS)
    {
        return;
    }
    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    s_slots[slot].valid = true;
    s_slots[slot].runtime_slot = slot;
    s_slots[slot].driveletter = driveletter;
    if (sd_path != NULL)
    {
        strncpy(s_slots[slot].sd_path, sd_path, sizeof(s_slots[slot].sd_path) - 1);
    }
    // Provisional -- overwritten by sidetnfs_sd_service_run(). If that
    // never runs this boot (shouldn't happen -- called unconditionally
    // once from gemdrvemul.c), a registered-but-never-checked slot reads
    // as ABSENT rather than a false READY.
    s_slots[slot].status = SIDETNFS_SD_STATUS_ABSENT;
    s_slots[slot].fresult_raw = 0xFFu;
}

void sidetnfs_sd_service_run(void)
{
    if (s_service_ran)
    {
        return;
    }
    s_service_ran = true;

    // Step 1+2: driver init + (best-effort) detection. sd_init_driver
    // only ever fails on a genuine driver/hardware-level problem (SPI bus
    // init) -- never "no card" on this board (SIDETNFS_ENABLE_SD_SUPPORT's
    // hw_config.c has use_card_detect=false, so there is no dedicated
    // card-detect GPIO; "no card" can only be observed once f_mount()
    // actually tries to talk to it, via FR_NOT_READY below).
    if (!sd_init_driver())
    {
        s_global_status = SIDETNFS_SD_STATUS_INIT_FAILED;
        s_global_fresult = 0xFFu;
    }
    else
    {
        // Step 3: single, immediate (opt=1) mount attempt -- never
        // retried. FR_NOT_READY is FatFS's own code for "the physical
        // drive cannot work" (no medium, or the media couldn't be
        // brought up) -- the closest honest, non-invented "no card"
        // signal this driver/FatFS combination offers. FR_NO_FILESYSTEM
        // is FatFS's own "no valid FAT volume" code -- a genuinely
        // corrupt/foreign filesystem, distinct from a missing card.
        FRESULT fr = f_mount(&s_fatfs, "0:", 1);
        s_global_fresult = (uint8_t)fr;
        if (fr == FR_OK)
        {
            s_global_status = SIDETNFS_SD_STATUS_READY;
        }
        else if (fr == FR_NOT_READY)
        {
            s_global_status = SIDETNFS_SD_STATUS_ABSENT;
        }
        else if (fr == FR_NO_FILESYSTEM)
        {
            s_global_status = SIDETNFS_SD_STATUS_FILESYSTEM_ERROR;
        }
        else
        {
            s_global_status = SIDETNFS_SD_STATUS_MOUNT_FAILED;
        }
    }

    // Step 4: per-ENABLED-SD-drive directory check -- only reached
    // (and only meaningful) once the shared card/mount is READY; every
    // registered slot otherwise mirrors the same global status (a card
    // problem is never slot-specific -- see the header's own comment).
    for (int i = 0; i < SIDETNFS_SD_SERVICE_MAX_SLOTS; i++)
    {
        if (!s_slots[i].valid)
        {
            continue;
        }
        if (s_global_status != SIDETNFS_SD_STATUS_READY)
        {
            s_slots[i].status = s_global_status;
            s_slots[i].fresult_raw = s_global_fresult;
            continue;
        }
        FILINFO fno;
        FRESULT fr = f_stat(s_slots[i].sd_path, &fno);
        s_slots[i].fresult_raw = (uint8_t)fr;
        if (fr == FR_OK && (fno.fattrib & AM_DIR))
        {
            s_slots[i].status = SIDETNFS_SD_STATUS_READY;
        }
        else if (fr == FR_OK)
        {
            // Exists, but is a plain file, not a directory.
            s_slots[i].status = SIDETNFS_SD_STATUS_NOT_A_DIRECTORY;
        }
        else
        {
            // FR_NO_FILE/FR_NO_PATH (the expected "doesn't exist" codes)
            // and any other rare f_stat() failure both mean the same
            // thing from the Atari's point of view: the configured path
            // could not be found/verified.
            s_slots[i].status = SIDETNFS_SD_STATUS_DIRECTORY_NOT_FOUND;
        }
    }
}

bool sidetnfs_sd_service_has_run(void)
{
    return s_service_ran;
}

sidetnfs_sd_status_t sidetnfs_sd_global_status(void)
{
    return s_global_status;
}

uint8_t sidetnfs_sd_global_fresult(void)
{
    return s_global_fresult;
}

bool sidetnfs_sd_get_drive_status(int slot, sidetnfs_sd_drive_status_t *out)
{
    if (slot < 0 || slot >= SIDETNFS_SD_SERVICE_MAX_SLOTS || out == NULL)
    {
        return false;
    }
    if (!s_slots[slot].valid)
    {
        return false;
    }
    *out = s_slots[slot];
    return true;
}

const char *sidetnfs_sd_status_text(sidetnfs_sd_status_t status)
{
    switch (status)
    {
    case SIDETNFS_SD_STATUS_READY:
        return "Ready";
    case SIDETNFS_SD_STATUS_ABSENT:
        return "No SD card inserted";
    case SIDETNFS_SD_STATUS_INIT_FAILED:
        return "SD card initialization failed";
    case SIDETNFS_SD_STATUS_MOUNT_FAILED:
        return "Unable to mount the SD card filesystem";
    case SIDETNFS_SD_STATUS_FILESYSTEM_ERROR:
        return "The SD card filesystem could not be read";
    case SIDETNFS_SD_STATUS_DIRECTORY_NOT_FOUND:
        return "Directory does not exist";
    case SIDETNFS_SD_STATUS_NOT_A_DIRECTORY:
    default:
        return "Configured SD path is not a directory";
    }
}

const char *sidetnfs_sd_status_hint(sidetnfs_sd_status_t status)
{
    switch (status)
    {
    case SIDETNFS_SD_STATUS_READY:
        return "";
    case SIDETNFS_SD_STATUS_ABSENT:
        return "Insert an SD card.";
    case SIDETNFS_SD_STATUS_INIT_FAILED:
        return "Check the SD card and reboot.";
    case SIDETNFS_SD_STATUS_MOUNT_FAILED:
        return "Check the SD card and its FAT filesystem.";
    case SIDETNFS_SD_STATUS_FILESYSTEM_ERROR:
        return "Check the SD card and its FAT filesystem.";
    case SIDETNFS_SD_STATUS_DIRECTORY_NOT_FOUND:
        return "Create the configured directory, or correct the SD path.";
    case SIDETNFS_SD_STATUS_NOT_A_DIRECTORY:
    default:
        return "Correct the SD path.";
    }
}

size_t sidetnfs_build_sd_error_text(int slot, char driveletter, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0)
    {
        return 0;
    }
    sidetnfs_sd_drive_status_t st;
    bool have = sidetnfs_sd_get_drive_status(slot, &st);
    sidetnfs_sd_status_t status = have ? st.status : sidetnfs_sd_global_status();
    const char *path = have ? st.sd_path : "";
    uint8_t fr = have ? st.fresult_raw : sidetnfs_sd_global_fresult();

    char fresult_str[16];
    if (fr == 0xFFu)
    {
        snprintf(fresult_str, sizeof(fresult_str), "N/A");
    }
    else
    {
        snprintf(fresult_str, sizeof(fresult_str), "%u", (unsigned)fr);
    }

    int n = snprintf(out, out_size,
                      "SideTNFS SD drive error\r\n"
                      "\r\n"
                      "Drive: %c:\r\n"
                      "Status: %s\r\n"
                      "SD path: %s\r\n"
                      "FatFS result: %s\r\n"
                      "\r\n"
                      "%s\r\n"
                      "Use SIDETNFS.PRG on the SETTINGS disk to change the configuration.\r\n",
                      driveletter, sidetnfs_sd_status_text(status), path, fresult_str,
                      sidetnfs_sd_status_hint(status));
    if (n < 0)
    {
        out[0] = '\0';
        return 0;
    }
    if ((size_t)n >= out_size)
    {
        return out_size - 1; // snprintf truncated -- already NUL-terminated at out_size-1
    }
    return (size_t)n;
}

// Dedicated, memory-only search-slot table for SD_ERROR.TXT --
// entirely separate from sidetnfs_probe.c's own fake/NET_ERR listing
// tables (see this file's own header comment: "Routeer SD nooit via de
// TNFS-foutbackend"). Same "4 concurrent searches" sizing rationale as
// sidetnfs_probe.c's SIDETNFS_SEARCH_SLOTS.
#define SIDETNFS_SD_ERROR_SEARCH_SLOTS 4u

typedef struct
{
    bool active;
    uint32_t ndta;
    char path[MAX_FOLDER_LENGTH];
    char pattern[13];
    uint8_t attribs;
    uint16_t next_index;
    int slot;
    char driveletter;
} SidetnfsSdErrorSearchSlot;

static SidetnfsSdErrorSearchSlot s_sd_error_searches[SIDETNFS_SD_ERROR_SEARCH_SLOTS] = {0};

static SidetnfsSdErrorSearchSlot *find_sd_error_search_slot(uint32_t ndta)
{
    for (int i = 0; i < (int)SIDETNFS_SD_ERROR_SEARCH_SLOTS; i++)
    {
        if (s_sd_error_searches[i].active && s_sd_error_searches[i].ndta == ndta)
        {
            return &s_sd_error_searches[i];
        }
    }
    return NULL;
}

static SidetnfsSdErrorSearchSlot *alloc_sd_error_search_slot(void)
{
    for (int i = 0; i < (int)SIDETNFS_SD_ERROR_SEARCH_SLOTS; i++)
    {
        if (!s_sd_error_searches[i].active)
        {
            return &s_sd_error_searches[i];
        }
    }
    return &s_sd_error_searches[0]; // extremely unlikely: evict the first slot
}

static SidetnfsDirSearchResult sd_error_search_advance(SidetnfsSdErrorSearchSlot *search, SidetnfsAtariDirEntry *out_entry)
{
    if (search->next_index > 0 || strncmp(search->path, "/", sizeof(search->path)) != 0)
    {
        search->active = false;
        return SIDETNFS_DIR_SEARCH_NOT_FOUND;
    }
    search->next_index = 1;

    SidetnfsAtariDirEntry entry;
    memset(&entry, 0, sizeof(entry));
    char text[SIDETNFS_SD_ERROR_TEXT_MAX];
    size_t text_len = sidetnfs_build_sd_error_text(search->slot, search->driveletter, text, sizeof(text));
    strncpy(entry.name, SIDETNFS_SD_ERROR_NAME, sizeof(entry.name) - 1);
    entry.attr = 0; // plain file
    entry.size = (uint32_t)text_len;
    entry.valid = true;

    if (sidetnfs_gemdos_pattern_match(entry.name, search->pattern) &&
        sidetnfs_gemdos_attr_match(entry.attr, search->attribs))
    {
        *out_entry = entry;
        return SIDETNFS_DIR_SEARCH_FOUND;
    }
    search->active = false;
    return SIDETNFS_DIR_SEARCH_NOT_FOUND;
}

SidetnfsDirSearchResult sidetnfs_sd_error_search_start(uint32_t ndta, int slot, char driveletter, const char *path,
                                                         const char *pattern, uint8_t attribs,
                                                         SidetnfsAtariDirEntry *out_entry)
{
    SidetnfsSdErrorSearchSlot *search = find_sd_error_search_slot(ndta);
    if (!search)
    {
        search = alloc_sd_error_search_slot();
    }
    memset(search, 0, sizeof(*search));
    search->ndta = ndta;
    strncpy(search->path, path, sizeof(search->path) - 1);
    strncpy(search->pattern, pattern, sizeof(search->pattern) - 1);
    search->attribs = attribs;
    search->active = true;
    search->slot = slot;
    search->driveletter = driveletter;

    return sd_error_search_advance(search, out_entry);
}

SidetnfsDirSearchResult sidetnfs_sd_error_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry)
{
    SidetnfsSdErrorSearchSlot *search = find_sd_error_search_slot(ndta);
    if (!search)
    {
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    return sd_error_search_advance(search, out_entry);
}

bool sidetnfs_sd_error_search_is_active(uint32_t ndta)
{
    return find_sd_error_search_slot(ndta) != NULL;
}

void sidetnfs_sd_error_search_close(uint32_t ndta)
{
    SidetnfsSdErrorSearchSlot *search = find_sd_error_search_slot(ndta);
    if (search)
    {
        search->active = false;
    }
}

void sidetnfs_sd_error_search_close_all(void)
{
    for (unsigned i = 0; i < SIDETNFS_SD_ERROR_SEARCH_SLOTS; i++)
    {
        s_sd_error_searches[i].active = false;
    }
}

uint16_t sidetnfs_sd_error_search_count_active(void)
{
    uint16_t count = 0;
    for (unsigned i = 0; i < SIDETNFS_SD_ERROR_SEARCH_SLOTS; i++)
    {
        if (s_sd_error_searches[i].active)
        {
            count++;
        }
    }
    return count;
}
