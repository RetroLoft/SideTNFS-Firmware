#ifndef SCFS_H
#define SCFS_H

#include <stdbool.h>
#include <stdint.h>

bool scfs_directory_exists(const char *path);

typedef struct
{
    uint32_t free_clusters;
    uint32_t total_clusters;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
} ScFsDiskInfo;

bool scfs_get_disk_info(const char *path, ScFsDiskInfo *info);

#endif // SCFS_H
