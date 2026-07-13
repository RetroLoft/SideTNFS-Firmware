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
