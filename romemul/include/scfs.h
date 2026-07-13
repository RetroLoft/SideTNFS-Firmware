#ifndef SCFS_H
#define SCFS_H

#include <stdbool.h>
#include <stdint.h>
#include "f_util.h"

bool scfs_directory_exists(const char *path);

typedef struct
{
    uint32_t free_clusters;
    uint32_t total_clusters;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
} ScFsDiskInfo;

bool scfs_get_disk_info(const char *path, ScFsDiskInfo *info);

// FatFS FRESULT -> GEMDOS error code mapping, matching the mapping already
// used by GEMDRVEMUL_DDELETE_CALL. Implemented in gemdrvemul.c (not scfs.c)
// because the GEMDOS_* target constants are defined in gemdrvemul.h, which
// already includes this header -- including gemdrvemul.h from scfs.c would
// be circular.
int16_t scfs_fresult_to_gemdos_error(FRESULT fr);

#endif // SCFS_H
