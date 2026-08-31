/**
 * File: sidetnfs_floppy_emul.h
 * Description: read-only .ST floppy-image backend (Phase 3). One small
 * interface -- open/close/read_sector/geometry -- deliberately hiding
 * whether the active image lives on TNFS or SD, so a later .MSA format,
 * drive B:, a sector cache, or a new source never has to touch the
 * command dispatcher (GEMDRVEMUL_FLOPPY_SESSION_START/_READ_SECTOR in
 * gemdrvemul.c) at all -- only this file's own implementation changes.
 *
 * Also owns the two Phase 2B boot-policy flags (install_gemdrive/
 * install_floppy) as an actual runtime object, per the project's own
 * "these must eventually be real runtime state, not just ROM3 field
 * documentation" requirement: sidetnfs_floppy_emul_init() sets the
 * mandatory fail-safe default (YES/NO) once per Pico power-cycle and
 * publishes it into ROM3 immediately, and
 * sidetnfs_floppy_emul_set_boot_policy() is the only other place either
 * flag ever changes (FLOPPY_SESSION_START, and later the long-SELECT
 * exit-restore path). Neither flag is ever read from or written to
 * flash -- pure RAM, session/launch state only.
 */
#ifndef SIDETNFS_FLOPPY_EMUL_H
#define SIDETNFS_FLOPPY_EMUL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    SIDETNFS_FLOPPY_EMUL_OK = 0,
    SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE = 1,      // profile index out of range, EMPTY, or an invalid/unrecognized backend value
    SIDETNFS_FLOPPY_EMUL_ERR_SOURCE_NOT_CONFIGURED = 2, // backend's required field (host/sd_path) is blank
    SIDETNFS_FLOPPY_EMUL_ERR_TNFS_NOT_CONNECTED = 3,    // no WiFi, or host never resolved
    SIDETNFS_FLOPPY_EMUL_ERR_TNFS_HOST_UNREACHABLE = 4, // MOUNT sent, no response within the bounded wait, or mount rc != OK
    SIDETNFS_FLOPPY_EMUL_ERR_SD_NOT_PRESENT = 5,        // SD service hasn't run yet, or global status isn't READY
    SIDETNFS_FLOPPY_EMUL_ERR_FILE_NOT_FOUND = 6,        // image_path does not exist on the resolved backend
    SIDETNFS_FLOPPY_EMUL_ERR_ACCESS_DENIED = 7,         // backend reported a permission error
    SIDETNFS_FLOPPY_EMUL_ERR_PATH_TOO_LONG = 8,         // resolved SD path would exceed the backend's own path buffer
    SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID = 9,      // file size is 0, or not an exact multiple of 512
    SIDETNFS_FLOPPY_EMUL_ERR_BPB_INVALID = 10,          // sector 0's BPB bytes-per-sector field isn't 512
    SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED = 11, // sides/sectors-per-track/track-count outside the MVP's supported ranges
    SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_MISMATCH = 12,    // file size doesn't divide exactly by sides*sectors_per_track, or BPB total-sector field != filesize/512
    SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED = 13,          // backend I/O error reading a sector (seek or read)
    SIDETNFS_FLOPPY_EMUL_ERR_OUT_OF_RANGE = 14,         // requested LBA >= total_sectors
    SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR = 15,        // generic TNFS/SD error not covered above
    SIDETNFS_FLOPPY_EMUL_ERR_NOT_OPEN = 16,             // read_sector called with no image open/ready
} sidetnfs_floppy_emul_status_t;

typedef struct
{
    uint8_t sides;             // 1 or 2
    uint8_t sectors_per_track; // 9, 10 or 11
    uint8_t tracks;            // 80..85, derived from file size, never trusted from the BPB alone
    uint16_t bytes_per_sector; // always 512 (NUM_BYTES_PER_SECTOR), validated not assumed
    uint32_t total_sectors;    // == filesize / 512 (the authoritative source, see req #2)
} sidetnfs_floppy_geometry_t;

// Boot-time init: closes any backend (defensive; none should exist this
// early), sets install_gemdrive=true/install_floppy=false in RAM, and
// publishes both into ROM3 immediately. Call exactly once, from
// init_gemdrvemul(), before the command-dispatch loop starts. Never
// touches flash.
void sidetnfs_floppy_emul_init(uint32_t memory_shared_address);

// Current requested Atari-boot-configuration flags (RAM only).
bool sidetnfs_floppy_emul_install_gemdrive(void);
bool sidetnfs_floppy_emul_install_floppy(void);

// Sets both boot-policy flags at once and publishes them into ROM3
// immediately. Used by GEMDRVEMUL_FLOPPY_SESSION_START and by the
// (not-yet-implemented) long-SELECT exit-restore path, which calls this
// with (true, false) to reproduce the exact power-cycle default.
void sidetnfs_floppy_emul_set_boot_policy(uint32_t memory_shared_address, bool install_gemdrive, bool install_floppy);

// Pure geometry/BPB validation (req #2) -- no I/O, no static state, no
// Pico-SDK dependency, so it is host-testable exactly like
// sidetnfs_longpress.h's sidetnfs_longpress_poll_step() (see
// tests/host_floppy_emul/test_floppy_bpb_validation.c). Takes the actual
// file size plus the four BPB fields already parsed from sector 0
// (bytes-per-sector, total-sectors, sectors-per-track, sides -- same
// byte offsets create_blank_ST_image() in filesys.c writes: 11-12, 19-20,
// 24-25, 26-27) and applies the exact algorithm documented at the top of
// sidetnfs_floppy_emul.c: filesize must be a nonzero multiple of 512;
// BPS must be 512; sides must be 1 or 2; sectors-per-track must be
// 9-11; filesize/512 must divide evenly by sides*sectors-per-track,
// giving a track count in [80,85]; and the BPB's own total-sector field
// must equal filesize/512. The literal 512 here (not NUM_BYTES_PER_SECTOR)
// is deliberate -- this header has no filesys.h/Pico-SDK include, by
// design; sidetnfs_floppy_emul.c's own _Static_assert cross-checks the
// two never drift apart.
static inline sidetnfs_floppy_emul_status_t sidetnfs_floppy_emul_validate_geometry(
    uint32_t filesize, uint16_t bpb_bytes_per_sector, uint16_t bpb_total_sectors, uint16_t bpb_sectors_per_track,
    uint16_t bpb_sides, sidetnfs_floppy_geometry_t *out_geom)
{
    if (out_geom)
    {
        out_geom->sides = 0;
        out_geom->sectors_per_track = 0;
        out_geom->tracks = 0;
        out_geom->bytes_per_sector = 0;
        out_geom->total_sectors = 0;
    }
    if (filesize == 0 || (filesize % 512u) != 0)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID;
    }
    if (bpb_bytes_per_sector != 512u)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_BPB_INVALID;
    }
    if ((bpb_sides != 1 && bpb_sides != 2) || bpb_sectors_per_track < 9 || bpb_sectors_per_track > 11)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED;
    }
    uint32_t total_sectors_from_filesize = filesize / 512u;
    uint32_t sides_x_spt = (uint32_t)bpb_sides * (uint32_t)bpb_sectors_per_track;
    if (total_sectors_from_filesize % sides_x_spt != 0)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_MISMATCH;
    }
    uint32_t tracks = total_sectors_from_filesize / sides_x_spt;
    if (tracks < 80 || tracks > 85)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED;
    }
    if (bpb_total_sectors != total_sectors_from_filesize)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_MISMATCH;
    }
    if (out_geom)
    {
        out_geom->sides = (uint8_t)bpb_sides;
        out_geom->sectors_per_track = (uint8_t)bpb_sectors_per_track;
        out_geom->tracks = (uint8_t)tracks;
        out_geom->bytes_per_sector = 512u;
        out_geom->total_sectors = total_sectors_from_filesize;
    }
    return SIDETNFS_FLOPPY_EMUL_OK;
}

// Closes any open backend (TNFS handle or SD file), then resolves
// profile_index's configured source, opens image_path read-only, and
// validates it (file size, BPB, geometry -- see the .c file's own
// top-of-function comment for the exact algorithm). On ANY failure
// (including image_path == NULL/empty), no backend is left open and
// *out_geom is left zeroed. On success, *out_geom is filled and the
// image is ready for sidetnfs_floppy_emul_read_sector().
sidetnfs_floppy_emul_status_t sidetnfs_floppy_emul_open(uint8_t profile_index, const char *image_path,
                                                          bool network_ok, sidetnfs_floppy_geometry_t *out_geom);

// Closes whatever backend is open, if any (harmless no-op otherwise) and
// clears the runtime geometry. Used both when opening a new image (see
// sidetnfs_floppy_emul_open()'s own comment) and when INSTALL_FLOPPY=NO
// (FLOPPY_SESSION_START must still guarantee no stale backend survives).
void sidetnfs_floppy_emul_close(void);

bool sidetnfs_floppy_emul_is_open(void);

// Reads exactly one 512-byte logical sector into out_buf. On ANY
// failure -- not open, lba out of range, or a backend I/O error --
// out_buf is zeroed before the error is returned, so a caller can never
// mistake a failed read's leftover buffer contents for real sector data.
sidetnfs_floppy_emul_status_t sidetnfs_floppy_emul_read_sector(uint32_t lba, uint8_t *out_buf);

#endif // SIDETNFS_FLOPPY_EMUL_H
