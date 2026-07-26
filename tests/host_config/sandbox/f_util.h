#ifndef F_UTIL_H
#define F_UTIL_H
#include <stdint.h>

typedef struct { int dummy; } FIL;
typedef int FRESULT;
// Fase 6: numbered exactly like the real fatfs-sdk/src/ff15/source/ff.h
// enum -- sidetnfs_sd_service.c's own status mapping depends on these
// specific values (FR_NOT_READY/FR_NO_FILESYSTEM/FR_NO_FILE/FR_NO_PATH),
// not just FR_OK.
#define FR_OK 0
#define FR_DISK_ERR 1
#define FR_INT_ERR 2
#define FR_NOT_READY 3
#define FR_NO_FILE 4
#define FR_NO_PATH 5
#define FR_NO_FILESYSTEM 13
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08

typedef unsigned int UINT;

typedef struct { int dummy; } DIR;
typedef struct { char fname[261]; uint8_t fattrib; } FILINFO;
typedef struct { int dummy; } FATFS;
#define AM_DIR 0x10

FRESULT f_open(FIL *fp, const char *path, uint8_t mode);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_close(FIL *fp);
FRESULT f_opendir(DIR *dp, const char *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);
FRESULT f_mount(FATFS *fs, const char *path, uint8_t opt);
FRESULT f_stat(const char *path, FILINFO *fno);

#endif
