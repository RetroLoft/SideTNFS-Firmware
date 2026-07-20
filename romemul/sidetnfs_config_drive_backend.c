/**
 * File: sidetnfs_config_drive_backend.c
 * Description: Fase 10B -- read-only, root-only virtual drive serving the
 * Fase 10A flash-embedded SIDETNFS.PRG/README.TXT directly from their
 * const arrays. No RAM copy of either file, no SD/WiFi/TNFS access, no
 * flash writes anywhere in this file.
 */
#include "include/sidetnfs_config_drive_backend.h"

#include <string.h>

#include "include/filesys.h" // FS_ST_* Atari attribute bits
#include "include/sidetnfs_config_drive.h" // sidetnfs_config_prg/readme + _length, Fase 10A

// SIDETNFS_CONFIG_DRIVE_DATE/TIME/ATTR are declared in the header (shared
// with gemdrvemul.c's GEMDRVEMUL_FDATETIME_CALL/FATTRIB_CALL handling).

// Fase 10B: exactly two fixed files, by design (see report) -- looked up
// by index rather than a static table, since the flash arrays' _length
// symbols are extern const (not compile-time constant expressions, so
// they cannot populate a static/global initializer in this translation
// unit).
static bool file_by_index(uint8_t index, const char **out_name, const uint8_t **out_data, uint32_t *out_size)
{
    switch (index)
    {
    case 0:
        *out_name = "SIDETNFS.PRG";
        *out_data = sidetnfs_config_prg;
        *out_size = sidetnfs_config_prg_length;
        return true;
    case 1:
        *out_name = "README.TXT";
        *out_data = sidetnfs_config_readme;
        *out_size = sidetnfs_config_readme_length;
        return true;
    default:
        return false;
    }
}

static bool name_equals_ci(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z')
        {
            ca = (char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z')
        {
            cb = (char)(cb - 'a' + 'A');
        }
        if (ca != cb)
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool sidetnfs_config_drive_lookup(const char *name83, const uint8_t **out_data, uint32_t *out_size)
{
    if (name83 == NULL)
    {
        return false;
    }
    for (uint8_t i = 0; i < SIDETNFS_CONFIG_DRIVE_FILE_COUNT; i++)
    {
        const char *name;
        const uint8_t *data;
        uint32_t size;
        file_by_index(i, &name, &data, &size);
        if (name_equals_ci(name, name83))
        {
            *out_data = data;
            *out_size = size;
            return true;
        }
    }
    return false;
}

static void build_entry(uint8_t index, SidetnfsAtariDirEntry *out)
{
    const char *name;
    const uint8_t *data;
    uint32_t size;
    file_by_index(index, &name, &data, &size);
    (void)data;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    out->attr = SIDETNFS_CONFIG_DRIVE_ATTR;
    out->size = size;
    out->date = (uint16_t)SIDETNFS_CONFIG_DRIVE_DATE;
    out->time = (uint16_t)SIDETNFS_CONFIG_DRIVE_TIME;
    out->valid = true;
    out->skipped = false;
}

// Fase 10B: small fixed-size ndta-keyed search registry, same shape as
// sidetnfs_probe.c's fake no-network search table (SIDETNFS_SEARCH_SLOTS)
// -- only an index (0, 1, or SIDETNFS_CONFIG_DRIVE_FILE_COUNT == done) is
// tracked per slot, no I/O, nothing that can fail.
#define SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS 4u

typedef struct
{
    bool active;
    uint32_t ndta;
    uint8_t next_index;
    char pattern[16];
    uint8_t attribs;
} SidetnfsConfigDriveSearch;

static SidetnfsConfigDriveSearch s_searches[SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS] = {0};

static SidetnfsConfigDriveSearch *find_slot(uint32_t ndta)
{
    for (unsigned i = 0; i < SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS; i++)
    {
        if (s_searches[i].active && s_searches[i].ndta == ndta)
        {
            return &s_searches[i];
        }
    }
    return NULL;
}

// Fase 10B2: unlike sidetnfs_probe.c's alloc_tnfs_dta_slot() (which evicts
// slot 0 unconditionally when all slots are active), this never sacrifices
// someone else's live search -- there is no meaningful "expired" state for
// a config-drive slot (no handle, no timeout, nothing that can go stale on
// its own), so silently evicting slot 0 would risk corrupting a second,
// unrelated DTA's in-progress Fsfirst/Fsnext sequence. Returns NULL when
// every slot is genuinely active; the caller turns that into a controlled
// SIDETNFS_DIR_SEARCH_ERROR instead of touching any slot.
static SidetnfsConfigDriveSearch *alloc_slot(void)
{
    for (unsigned i = 0; i < SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS; i++)
    {
        if (!s_searches[i].active)
        {
            return &s_searches[i];
        }
    }
    return NULL;
}

// Scans file_by_index() starting at *inout_index for the first entry
// matching pattern/attribs, filling *out and advancing *inout_index past
// it on a match. Returns true on a match, false if none remain.
static bool scan_for_match(uint8_t *inout_index, const char *pattern, uint8_t attribs, SidetnfsAtariDirEntry *out)
{
    for (uint8_t i = *inout_index; i < SIDETNFS_CONFIG_DRIVE_FILE_COUNT; i++)
    {
        SidetnfsAtariDirEntry candidate;
        build_entry(i, &candidate);
        if (sidetnfs_gemdos_pattern_match(candidate.name, pattern) &&
            sidetnfs_gemdos_attr_match(candidate.attr, attribs))
        {
            *out = candidate;
            *inout_index = (uint8_t)(i + 1);
            return true;
        }
    }
    *inout_index = SIDETNFS_CONFIG_DRIVE_FILE_COUNT;
    return false;
}

SidetnfsDirSearchResult sidetnfs_config_drive_search_start(uint32_t ndta, const char *pattern, uint8_t attribs,
                                                             SidetnfsAtariDirEntry *out_entry)
{
    // A repeated Fsfirst for the same ndta always starts fresh, matching
    // both the SD/FatFS backend's insertDTA() and the TNFS DTA
    // registry's insertTnfsDTA() -- see their own report notes.
    SidetnfsConfigDriveSearch *slot = find_slot(ndta);
    if (slot == NULL)
    {
        slot = alloc_slot();
    }
    if (slot == NULL)
    {
        // All SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS are active for other
        // ndta's -- a controlled failure, not a silently corrupted search.
        return SIDETNFS_DIR_SEARCH_ERROR;
    }
    memset(slot, 0, sizeof(*slot));
    slot->ndta = ndta;
    strncpy(slot->pattern, pattern ? pattern : "*.*", sizeof(slot->pattern) - 1);
    slot->attribs = attribs;
    slot->active = true;

    uint8_t index = 0;
    bool found = scan_for_match(&index, slot->pattern, slot->attribs, out_entry);
    slot->next_index = index;

    if (!found)
    {
        slot->active = false; // exhausted immediately -- nothing to keep open
        return SIDETNFS_DIR_SEARCH_NOT_FOUND;
    }
    return SIDETNFS_DIR_SEARCH_FOUND;
}

SidetnfsDirSearchResult sidetnfs_config_drive_search_next(uint32_t ndta, SidetnfsAtariDirEntry *out_entry)
{
    SidetnfsConfigDriveSearch *slot = find_slot(ndta);
    if (slot == NULL)
    {
        return SIDETNFS_DIR_SEARCH_NOT_FOUND;
    }

    uint8_t index = slot->next_index;
    bool found = scan_for_match(&index, slot->pattern, slot->attribs, out_entry);
    slot->next_index = index;

    if (!found)
    {
        slot->active = false;
        return SIDETNFS_DIR_SEARCH_NOT_FOUND;
    }
    return SIDETNFS_DIR_SEARCH_FOUND;
}

void sidetnfs_config_drive_search_close(uint32_t ndta)
{
    SidetnfsConfigDriveSearch *slot = find_slot(ndta);
    if (slot != NULL)
    {
        slot->active = false;
    }
}

bool sidetnfs_config_drive_search_is_active(uint32_t ndta)
{
    return find_slot(ndta) != NULL;
}

uint32_t sidetnfs_config_drive_search_count_active(void)
{
    uint32_t count = 0;
    for (unsigned i = 0; i < SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS; i++)
    {
        if (s_searches[i].active)
        {
            count++;
        }
    }
    return count;
}

void sidetnfs_config_drive_search_close_all(void)
{
    for (unsigned i = 0; i < SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS; i++)
    {
        s_searches[i].active = false;
    }
}

void sidetnfs_config_drive_get_disk_info(uint32_t *out_total_clusters, uint32_t *out_free_clusters,
                                          uint32_t *out_bytes_per_sector, uint32_t *out_sectors_per_cluster)
{
    // 256 KiB = 512 bytes/sector * 1 sector/cluster * 512 clusters.
    // 0 free clusters: this drive is read-only, so "no free space to
    // write into" is the honest answer, not an arbitrary choice.
    *out_bytes_per_sector = 512;
    *out_sectors_per_cluster = 1;
    *out_total_clusters = 512;
    *out_free_clusters = 0;
}
