/* Fase 10B2 host-test stub: the real romemul/include/filesys.h pulls in
 * Pico SDK / FatFs_SPI hardware headers (sd_card.h, hw_config.h, f_util.h,
 * ...) that don't compile with a host toolchain. sidetnfs_config_drive_backend.c
 * only needs the FS_ST_* attribute bit constants from it, so this stub
 * provides exactly those, with the exact same values as the real header,
 * and nothing else. Picked up instead of the real filesys.h only because
 * sidetnfs_config_drive_backend.c is symlinked into this sandbox directory
 * (see ../sidetnfs_config_drive_backend.c) -- the compiler resolves its
 * quote-include "include/filesys.h" relative to the symlink's own
 * location, not the real file's directory.
 */
#ifndef FILESYS_H
#define FILESYS_H

#define FS_ST_READONLY 0x1
#define FS_ST_HIDDEN 0x2
#define FS_ST_SYSTEM 0x4
#define FS_ST_LABEL 0x8
#define FS_ST_FOLDER 0x10
#define FS_ST_ARCH 0x20

#endif // FILESYS_H
