/**
 * File: scfs.c
 * Description: Minimal filesystem-backend wrapper. Currently forwards to the
 * existing FatFS-based helper in filesys.c; intended as the first, smallest
 * possible seam for a future non-FatFS backend.
 */

#include "include/scfs.h"
#include "include/filesys.h"

bool scfs_directory_exists(const char *path)
{
    return directory_exists(path) != 0;
}

bool scfs_get_disk_info(const char *path, ScFsDiskInfo *info)
{
    DWORD fre_clust;
    FATFS *fs;
    FRESULT fr = f_getfree(path, &fre_clust, &fs);
    if (fr != FR_OK)
    {
        return false;
    }
    info->free_clusters = fre_clust;
    info->total_clusters = fs->n_fatent - 2;
    info->sectors_per_cluster = fs->csize;
    info->bytes_per_sector = NUM_BYTES_PER_SECTOR;
    return true;
}
