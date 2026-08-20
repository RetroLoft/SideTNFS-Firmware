#ifndef FILESYS_H
#define FILESYS_H

#include "debug.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "sd_card.h"
#include "f_util.h"
#include "hw_config.h"

#include "config.h"
#include "memfunc.h"

#define GEMDOS_FILE_ATTRIB_VOLUME_LABEL 8

#define NUM_BYTES_PER_SECTOR 512
#define SPF_MAX 9

#define MAX_FOLDER_LENGTH 128 // Max length of the folder names

#define MAX_WIFI_PASSWORD_LENGTH 64

#define STORAGE_POLL_INTERVAL 30000

#define FS_ST_READONLY 0x1 // Read only
#define FS_ST_HIDDEN 0x2   // Hidden
#define FS_ST_SYSTEM 0x4   // System
#define FS_ST_LABEL 0x8    // Volume label
#define FS_ST_FOLDER 0x10  // Directory
#define FS_ST_ARCH 0x20    // Archive

#define bswap_16(x) (((x) >> 8) | (((x) & 0xFF) << 8))

typedef struct
{
    uint16_t ID;              /* Word : ID marker, should be $0E0F */
    uint16_t SectorsPerTrack; /* Word : Sectors per track */
    uint16_t Sides;           /* Word : Sides (0 or 1; add 1 to this to get correct number of sides) */
    uint16_t StartingTrack;   /* Word : Starting track (0-based) */
    uint16_t EndingTrack;     /* Word : Ending track (0-based) */
} MSAHEADERSTRUCT;

// Define the structure to hold floppy image parameters
typedef struct
{
    uint16_t template;
    uint16_t num_tracks;
    uint16_t num_sectors;
    uint16_t num_sides;
    uint16_t overwrite;
    char volume_name[14]; // Round to 14 to avoid problems with odd addresses
    char floppy_name[256];
} FloppyImageHeader;

FRESULT checkDiskSpace(const char *path, uint32_t nDiskSize);
FRESULT MSA_to_ST(const char *folder, char *msaFilename, char *stFilename, bool overwrite_flag);
FRESULT create_blank_ST_image(const char *folder, char *stFilename, int nTracks, int nSectors, int nSides, const char *volLavel, bool overwrite);
FRESULT copy_file(const char *folder, const char *src_filename, const char *dest_filename, bool overwrite_flag);
int directory_exists(const char *dir);
void get_card_info(FATFS *fs_ptr, uint32_t *totalSize_MB, uint32_t *freeSpace_MB);
FRESULT read_and_trim_file(const char *path, char **content, size_t max_length);
void split_fullpath(const char *fullPath, char *drive, char *folders, char *filePattern);
void back_2_forwardslash(char *path);
void forward_2_backslash(char *path);
void shorten_fname(const char *originalName, char shortenedName[12]);
void remove_dup_slashes(char *str);
uint8_t attribs_st2fat(uint8_t st_attribs);
uint8_t attribs_fat2st(uint8_t fat_attribs);
void get_attribs_st_str(char attribs_str[6], uint8_t st_attribs);
void upper_fname(const char *originalName, char upperName[14]);
void filter_fname(const char *originalName, char filteredName[14]);
void extract_filename(const char *url, char filename[256]);
bool is_floppy_rw(const char *filename);
void change_spi_speed();

#endif // FILESYS_H