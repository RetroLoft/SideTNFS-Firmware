/**
 * File: sidetnfs_floppy_emul.c
 * Description: see include/sidetnfs_floppy_emul.h for the interface
 * contract and the boot-policy runtime-state rationale.
 *
 * .ST VALIDATION ALGORITHM (sidetnfs_floppy_emul_open(), req #2):
 *   1. Resolve profile_index -> TNFS or SD source (mirrors
 *      sidetnfs_floppy_browse_open()'s own backend resolution, adapted
 *      for opening one file instead of a directory).
 *   2. Open image_path read-only on that backend; determine the file's
 *      real size (TNFS: SEEK_END; SD: f_size() after f_open()).
 *   3. Reject if filesize == 0 or filesize % 512 != 0
 *      (SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID).
 *   4. Read logical sector 0 and parse the Atari GEMDOS BPB fields this
 *      codebase already writes in create_blank_ST_image() (filesys.c) --
 *      same byte offsets, reused here as the authoritative field layout:
 *      offset 11-12 BPS (bytes/sector, LE word), 19-20 SEC (total
 *      sectors, LE word), 24-25 SPT (sectors/track, LE word), 26-27 SIDE
 *      (sides, LE word).
 *   5. Reject if BPS != 512 (ERR_BPB_INVALID).
 *   6. Reject if SIDE not in {1,2} or SPT not in {9,10,11}
 *      (ERR_GEOMETRY_UNSUPPORTED).
 *   7. total_sectors = filesize/512 (the actual file size is
 *      authoritative, per req #2 -- BPB's own SEC field is a
 *      cross-check, never the source of truth). Reject if
 *      total_sectors % (SIDE*SPT) != 0 -- the track division must be
 *      exact (ERR_GEOMETRY_MISMATCH).
 *   8. tracks = total_sectors / (SIDE*SPT). Reject if tracks not in
 *      [80,85] (ERR_GEOMETRY_UNSUPPORTED).
 *   9. Reject if BPB's SEC != total_sectors (ERR_GEOMETRY_MISMATCH) --
 *      "BPB total sectors must agree with actual file size".
 *   On any rejection above, the backend is closed before returning --
 *   never left half-open.
 */
#include "include/sidetnfs_floppy_emul.h"

#include <string.h>
#include <stdio.h>

#include "include/gemdrvemul.h" // GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE/_INSTALL_FLOPPY, ROM3 layout
#include "include/memfunc.h"    // WRITE_WORD
#include "include/filesys.h"    // NUM_BYTES_PER_SECTOR, FatFS types (FIL/FRESULT/f_open/f_lseek/f_read/f_close)
#include "include/sidetnfs_floppy_config.h"
#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_sd_service.h"

// Synthetic guest_fd passed to the TNFS file-read/seek/close primitives
// below -- this backend has no real GEMDOS file descriptor (it is not
// reached through Fopen/Fread), so this is diagnostic-only (see each of
// those functions' own DPRINTF/diag_log call sites); never compared
// against a real fdescriptors[] entry.
#define SIDETNFS_FLOPPY_EMUL_GUEST_FD 0xFFFFFFF0u

typedef enum
{
    FLOPPY_EMUL_BACKEND_NONE = 0,
    FLOPPY_EMUL_BACKEND_TNFS,
    FLOPPY_EMUL_BACKEND_SD
} floppy_emul_backend_kind_t;

typedef struct
{
    floppy_emul_backend_kind_t backend;
    bool handle_open; // true once the underlying file handle/FIL exists, regardless of whether validation later succeeded
    bool ready;        // true only after full validation succeeded -- required by read_sector()
    int tnfs_slot;
    uint8_t tnfs_handle;
    FIL sd_file;
    sidetnfs_floppy_geometry_t geom;
} floppy_emul_state_t;

static floppy_emul_state_t s_state;
static bool s_install_gemdrive = true;
static bool s_install_floppy = false;

static inline uint16_t read_le16(const uint8_t *buf, size_t offset)
{
    return (uint16_t)((uint16_t)buf[offset] | ((uint16_t)buf[offset + 1] << 8));
}

// "0:" + sd_root + "/" + image_path, tolerating image_path with or
// without its own leading slash. Mirrors sidetnfs_floppy_browse.c's own
// join_root_and_cwd() in spirit (not reused directly -- that helper is
// static to browse.c and CWD-shaped, not path-shaped).
static bool join_sd_path(const char *sd_root, const char *image_path, char *out, size_t out_size)
{
    int n = (image_path[0] == '/') ? snprintf(out, out_size, "0:%s%s", sd_root, image_path)
                                    : snprintf(out, out_size, "0:%s/%s", sd_root, image_path);
    return n > 0 && (size_t)n < out_size;
}

static void publish_boot_policy(uint32_t memory_shared_address)
{
    WRITE_WORD(memory_shared_address, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE, s_install_gemdrive ? 1u : 0u);
    WRITE_WORD(memory_shared_address, GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY, s_install_floppy ? 1u : 0u);
}

void sidetnfs_floppy_emul_init(uint32_t memory_shared_address)
{
    sidetnfs_floppy_emul_close();
    s_install_gemdrive = true;  // mandatory fail-safe default, every Pico power-cycle
    s_install_floppy = false;
    publish_boot_policy(memory_shared_address);
    // GEMDRVEMUL_FLOPPY_SESSION_RESET_REQUESTED/_EXIT_ACK_SEEN are reserved,
    // unused fields (see their own comments in gemdrvemul.h) -- nothing
    // reads or writes them any more, so no initialization needed here.
}

bool sidetnfs_floppy_emul_install_gemdrive(void) { return s_install_gemdrive; }
bool sidetnfs_floppy_emul_install_floppy(void) { return s_install_floppy; }

void sidetnfs_floppy_emul_set_boot_policy(uint32_t memory_shared_address, bool install_gemdrive, bool install_floppy)
{
    s_install_gemdrive = install_gemdrive;
    s_install_floppy = install_floppy;
    publish_boot_policy(memory_shared_address);
}

void sidetnfs_floppy_emul_close(void)
{
    if (s_state.handle_open)
    {
        if (s_state.backend == FLOPPY_EMUL_BACKEND_TNFS)
        {
            sidetnfs_tnfs_file_close(SIDETNFS_FLOPPY_EMUL_GUEST_FD, s_state.tnfs_handle, s_state.tnfs_slot);
        }
        else if (s_state.backend == FLOPPY_EMUL_BACKEND_SD)
        {
            f_close(&s_state.sd_file);
        }
    }
    memset(&s_state, 0, sizeof(s_state));
}

bool sidetnfs_floppy_emul_is_open(void)
{
    return s_state.ready;
}

// Assumes a handle is already open (handle_open == true); does NOT
// bounds-check lba against total_sectors -- sidetnfs_floppy_emul_open()
// uses this directly for sector 0 during validation, before
// total_sectors is known. sidetnfs_floppy_emul_read_sector() is the only
// other caller and does the bounds check itself first.
static sidetnfs_floppy_emul_status_t raw_read_sector(uint32_t lba, uint8_t *out_buf)
{
    if (s_state.backend == FLOPPY_EMUL_BACKEND_TNFS)
    {
        uint32_t new_offset = 0;
        uint8_t seek_rc = 0xFFu;
        if (!sidetnfs_tnfs_file_seek(SIDETNFS_FLOPPY_EMUL_GUEST_FD, s_state.tnfs_handle, s_state.tnfs_slot, false,
                                       (int32_t)(lba * (uint32_t)NUM_BYTES_PER_SECTOR), &new_offset, &seek_rc) ||
            seek_rc != 0)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        uint16_t actual = 0;
        if (!sidetnfs_tnfs_file_read(SIDETNFS_FLOPPY_EMUL_GUEST_FD, s_state.tnfs_handle, s_state.tnfs_slot, out_buf,
                                       (uint16_t)NUM_BYTES_PER_SECTOR, &actual) ||
            actual != NUM_BYTES_PER_SECTOR)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        return SIDETNFS_FLOPPY_EMUL_OK;
    }
    if (s_state.backend == FLOPPY_EMUL_BACKEND_SD)
    {
        FRESULT fr = f_lseek(&s_state.sd_file, (FSIZE_t)lba * (FSIZE_t)NUM_BYTES_PER_SECTOR);
        if (fr != FR_OK)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        UINT br = 0;
        fr = f_read(&s_state.sd_file, out_buf, NUM_BYTES_PER_SECTOR, &br);
        if (fr != FR_OK || br != NUM_BYTES_PER_SECTOR)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        return SIDETNFS_FLOPPY_EMUL_OK;
    }
    memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
    return SIDETNFS_FLOPPY_EMUL_ERR_NOT_OPEN;
}

sidetnfs_floppy_emul_status_t sidetnfs_floppy_emul_read_sector(uint32_t lba, uint8_t *out_buf)
{
    if (!s_state.ready)
    {
        memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
        return SIDETNFS_FLOPPY_EMUL_ERR_NOT_OPEN;
    }
    if (lba >= s_state.geom.total_sectors)
    {
        memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
        return SIDETNFS_FLOPPY_EMUL_ERR_OUT_OF_RANGE;
    }
    return raw_read_sector(lba, out_buf);
}

sidetnfs_floppy_emul_status_t sidetnfs_floppy_emul_open(uint8_t profile_index, const char *image_path,
                                                          bool network_ok, sidetnfs_floppy_geometry_t *out_geom)
{
    // Always close first -- "switching/opening another image must cleanly
    // close the previous backend handle first" (req), unconditionally,
    // even if everything below fails.
    sidetnfs_floppy_emul_close();

    if (out_geom)
    {
        memset(out_geom, 0, sizeof(*out_geom));
    }
    if (image_path == NULL || image_path[0] == '\0')
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE;
    }
    if (profile_index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE;
    }

    sidetnfs_floppy_profile_config_t profile;
    if (sidetnfs_floppy_config_get_profile(profile_index, &profile) != SIDETNFS_FLOPPY_STATUS_OK ||
        !sidetnfs_floppy_profile_is_configured(&profile))
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE;
    }

    uint32_t filesize = 0;

    if (profile.backend == SIDETNFS_FLOPPY_BACKEND_TNFS)
    {
        if (profile.fields.tnfs.host[0] == '\0')
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_SOURCE_NOT_CONFIGURED;
        }

        // Same shared single-session slot the LFN browser uses
        // (SIDETNFS_PROBE_FLOPPY_SLOT_BASE) -- FLOPPY.PRG only ever has
        // one active browse/mount at a time, matching that module's own
        // documented assumption, so reusing the slot costs nothing extra
        // and avoids a second static TNFS session.
        int slot = SIDETNFS_PROBE_FLOPPY_SLOT_BASE;
        sidetnfs_slot_tnfs_context_t existing;
        bool need_repopulate = true;
        if (sidetnfs_probe_get_slot_context(slot, &existing) && existing.valid &&
            strcmp(existing.host, profile.fields.tnfs.host) == 0 && existing.port == profile.port &&
            strcmp(existing.mount_path, profile.fields.tnfs.mount_path) == 0)
        {
            need_repopulate = false;
        }
        if (need_repopulate)
        {
            sidetnfs_drive_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.state = SIDETNFS_DRIVE_SLOT_ENABLED;
            cfg.drive_letter = 0; // never a GEMDOS drive
            cfg.type = SIDETNFS_DRIVE_TNFS;
            cfg.transport = SIDETNFS_TRANSPORT_UDP;
            cfg.port = profile.port;
            strncpy(cfg.nickname, profile.nickname, sizeof(cfg.nickname) - 1);
            strncpy(cfg.host, profile.fields.tnfs.host, sizeof(cfg.host) - 1);
            strncpy(cfg.mount_path, profile.fields.tnfs.mount_path, sizeof(cfg.mount_path) - 1);
            sidetnfs_probe_set_slot_context(slot, &cfg);
        }
        if (!sidetnfs_probe_mount_slot(slot))
        {
            SidetnfsDriveErrorCategory err = sidetnfs_probe_classify_slot_error(slot, network_ok);
            if (err == SIDETNFS_DRIVE_ERR_NO_WIFI || err == SIDETNFS_DRIVE_ERR_DNS_FAILED ||
                err == SIDETNFS_DRIVE_ERR_NO_SESSION)
            {
                return SIDETNFS_FLOPPY_EMUL_ERR_TNFS_NOT_CONNECTED;
            }
            return SIDETNFS_FLOPPY_EMUL_ERR_TNFS_HOST_UNREACHABLE;
        }

        uint8_t handle = 0;
        SidetnfsFileOpenResult open_result = sidetnfs_tnfs_file_open(slot, image_path, 0 /* read-only */, &handle);
        if (open_result != SIDETNFS_FILE_OPEN_OK)
        {
            return (open_result == SIDETNFS_FILE_OPEN_NOT_FOUND) ? SIDETNFS_FLOPPY_EMUL_ERR_FILE_NOT_FOUND
                                                                   : SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR;
        }
        s_state.backend = FLOPPY_EMUL_BACKEND_TNFS;
        s_state.tnfs_slot = slot;
        s_state.tnfs_handle = handle;
        s_state.handle_open = true;

        uint32_t end_offset = 0;
        uint8_t seek_rc = 0xFFu;
        if (!sidetnfs_tnfs_file_seek(SIDETNFS_FLOPPY_EMUL_GUEST_FD, handle, slot, true, 0, &end_offset, &seek_rc) ||
            seek_rc != 0)
        {
            sidetnfs_floppy_emul_close();
            return SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR;
        }
        filesize = end_offset;
    }
    else if (profile.backend == SIDETNFS_FLOPPY_BACKEND_SD)
    {
        if (profile.fields.sd.sd_path[0] == '\0')
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_SOURCE_NOT_CONFIGURED;
        }
        if (!sidetnfs_sd_service_has_run() || sidetnfs_sd_global_status() != SIDETNFS_SD_STATUS_READY)
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_SD_NOT_PRESENT;
        }

        char full_path[SIDETNFS_FLOPPY_SDPATH_LEN + SIDETNFS_FLOPPY_FAVORITE_PATH_MAX + 8];
        if (!join_sd_path(profile.fields.sd.sd_path, image_path, full_path, sizeof(full_path)))
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_PATH_TOO_LONG;
        }

        FRESULT fr = f_open(&s_state.sd_file, full_path, FA_READ);
        if (fr != FR_OK)
        {
            if (fr == FR_NO_FILE || fr == FR_NO_PATH)
            {
                return SIDETNFS_FLOPPY_EMUL_ERR_FILE_NOT_FOUND;
            }
            if (fr == FR_DENIED)
            {
                return SIDETNFS_FLOPPY_EMUL_ERR_ACCESS_DENIED;
            }
            return SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR;
        }
        s_state.backend = FLOPPY_EMUL_BACKEND_SD;
        s_state.handle_open = true;
        filesize = (uint32_t)f_size(&s_state.sd_file);
    }
    else
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE; // unreachable in practice -- SET_PROFILE already rejects this
    }

    uint8_t sector0[NUM_BYTES_PER_SECTOR];
    sidetnfs_floppy_emul_status_t validate_result;
    if (filesize == 0 || (filesize % (uint32_t)NUM_BYTES_PER_SECTOR) != 0)
    {
        // sidetnfs_floppy_emul_validate_geometry() would reject this too,
        // but reading sector 0 of a file that isn't even sector-aligned
        // is pointless work -- fail before touching the backend again.
        sidetnfs_floppy_emul_close();
        return SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID;
    }
    if (raw_read_sector(0, sector0) != SIDETNFS_FLOPPY_EMUL_OK)
    {
        sidetnfs_floppy_emul_close();
        return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
    }

    // BPB field offsets: same layout create_blank_ST_image() (filesys.c)
    // already writes -- see this file's own top-of-file comment. The
    // actual validation math (req #2's exact algorithm) lives in the
    // header as a pure, host-testable function -- see its own comment
    // and tests/host_floppy_emul/test_floppy_bpb_validation.c.
    validate_result = sidetnfs_floppy_emul_validate_geometry(filesize, read_le16(sector0, 11), read_le16(sector0, 19),
                                                               read_le16(sector0, 24), read_le16(sector0, 26),
                                                               &s_state.geom);
    if (validate_result != SIDETNFS_FLOPPY_EMUL_OK)
    {
        sidetnfs_floppy_emul_close();
        return validate_result;
    }

    s_state.ready = true;
    if (out_geom)
    {
        *out_geom = s_state.geom;
    }
    return SIDETNFS_FLOPPY_EMUL_OK;
}

// Phase 6A candidate-swap transaction. s_candidate mirrors s_state's own
// shape and holds, at most, ONE opened-but-not-yet-committed replacement
// backend -- see sidetnfs_floppy_emul_open_candidate()'s own comment.
static floppy_emul_state_t s_candidate;

// Closes whatever backend handle *st holds open, if any, then zeroes
// *st. Exact same logic as sidetnfs_floppy_emul_close(), factored out so
// it can also operate on s_candidate and on a saved-off old s_state
// during sidetnfs_floppy_emul_commit_candidate()'s ownership transfer.
// sidetnfs_floppy_emul_close() itself is deliberately left byte-for-byte
// unchanged (still the only thing sidetnfs_floppy_emul_open()/
// SESSION_START ever call) -- this helper is purely additive.
static void close_state(floppy_emul_state_t *st)
{
    if (st->handle_open)
    {
        if (st->backend == FLOPPY_EMUL_BACKEND_TNFS)
        {
            sidetnfs_tnfs_file_close(SIDETNFS_FLOPPY_EMUL_GUEST_FD, st->tnfs_handle, st->tnfs_slot);
        }
        else if (st->backend == FLOPPY_EMUL_BACKEND_SD)
        {
            f_close(&st->sd_file);
        }
    }
    memset(st, 0, sizeof(*st));
}

// Mirrors raw_read_sector()'s exact logic, parameterized by an arbitrary
// state struct instead of always the global s_state. Used only by
// sidetnfs_floppy_emul_open_candidate() to read a CANDIDATE's sector 0
// for validation. Deliberately NOT shared with raw_read_sector() itself,
// which stays completely untouched -- it alone backs the hardware-proven
// per-request read path (sidetnfs_floppy_emul_read_sector(), called from
// every GEMDRVEMUL_FLOPPY_READ_SECTOR). Some duplication accepted here
// rather than risk that path at all.
static sidetnfs_floppy_emul_status_t read_sector_raw_from(floppy_emul_state_t *st, uint32_t lba, uint8_t *out_buf)
{
    if (st->backend == FLOPPY_EMUL_BACKEND_TNFS)
    {
        uint32_t new_offset = 0;
        uint8_t seek_rc = 0xFFu;
        if (!sidetnfs_tnfs_file_seek(SIDETNFS_FLOPPY_EMUL_GUEST_FD, st->tnfs_handle, st->tnfs_slot, false,
                                       (int32_t)(lba * (uint32_t)NUM_BYTES_PER_SECTOR), &new_offset, &seek_rc) ||
            seek_rc != 0)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        uint16_t actual = 0;
        if (!sidetnfs_tnfs_file_read(SIDETNFS_FLOPPY_EMUL_GUEST_FD, st->tnfs_handle, st->tnfs_slot, out_buf,
                                       (uint16_t)NUM_BYTES_PER_SECTOR, &actual) ||
            actual != NUM_BYTES_PER_SECTOR)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        return SIDETNFS_FLOPPY_EMUL_OK;
    }
    if (st->backend == FLOPPY_EMUL_BACKEND_SD)
    {
        FRESULT fr = f_lseek(&st->sd_file, (FSIZE_t)lba * (FSIZE_t)NUM_BYTES_PER_SECTOR);
        if (fr != FR_OK)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        UINT br = 0;
        fr = f_read(&st->sd_file, out_buf, NUM_BYTES_PER_SECTOR, &br);
        if (fr != FR_OK || br != NUM_BYTES_PER_SECTOR)
        {
            memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
            return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
        }
        return SIDETNFS_FLOPPY_EMUL_OK;
    }
    memset(out_buf, 0, NUM_BYTES_PER_SECTOR);
    return SIDETNFS_FLOPPY_EMUL_ERR_NOT_OPEN;
}

// Phase 6A: opens+validates a CANDIDATE image into s_candidate, keeping
// its handle alive on success. s_state (the currently active backend) is
// never touched by this function, on any path -- see the header's own
// comment for why this exists at all (sidetnfs_floppy_emul_open() always
// closes the current backend FIRST, which is wrong for a runtime
// switch). Always discards any stale previous candidate first, mirroring
// sidetnfs_floppy_emul_open()'s own "always close first" discipline,
// applied to the candidate slot instead of the active one. Every failure
// path closes s_candidate (never s_state) before returning.
sidetnfs_floppy_emul_status_t sidetnfs_floppy_emul_open_candidate(uint8_t profile_index, const char *image_path,
                                                                     bool network_ok,
                                                                     sidetnfs_floppy_geometry_t *out_geom)
{
    close_state(&s_candidate);

    if (out_geom)
    {
        memset(out_geom, 0, sizeof(*out_geom));
    }
    if (image_path == NULL || image_path[0] == '\0')
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE;
    }
    if (profile_index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE;
    }

    sidetnfs_floppy_profile_config_t profile;
    if (sidetnfs_floppy_config_get_profile(profile_index, &profile) != SIDETNFS_FLOPPY_STATUS_OK ||
        !sidetnfs_floppy_profile_is_configured(&profile))
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE;
    }

    uint32_t filesize = 0;

    if (profile.backend == SIDETNFS_FLOPPY_BACKEND_TNFS)
    {
        if (profile.fields.tnfs.host[0] == '\0')
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_SOURCE_NOT_CONFIGURED;
        }

        // Same shared slot sidetnfs_floppy_emul_open() uses -- by the time
        // a runtime switch can happen the slot is already mounted for the
        // currently active image, so need_repopulate is always false here
        // in practice; the reuse-check is kept anyway for parity/safety.
        int slot = SIDETNFS_PROBE_FLOPPY_SLOT_BASE;
        sidetnfs_slot_tnfs_context_t existing;
        bool need_repopulate = true;
        if (sidetnfs_probe_get_slot_context(slot, &existing) && existing.valid &&
            strcmp(existing.host, profile.fields.tnfs.host) == 0 && existing.port == profile.port &&
            strcmp(existing.mount_path, profile.fields.tnfs.mount_path) == 0)
        {
            need_repopulate = false;
        }
        if (need_repopulate)
        {
            sidetnfs_drive_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.state = SIDETNFS_DRIVE_SLOT_ENABLED;
            cfg.drive_letter = 0;
            cfg.type = SIDETNFS_DRIVE_TNFS;
            cfg.transport = SIDETNFS_TRANSPORT_UDP;
            cfg.port = profile.port;
            strncpy(cfg.nickname, profile.nickname, sizeof(cfg.nickname) - 1);
            strncpy(cfg.host, profile.fields.tnfs.host, sizeof(cfg.host) - 1);
            strncpy(cfg.mount_path, profile.fields.tnfs.mount_path, sizeof(cfg.mount_path) - 1);
            sidetnfs_probe_set_slot_context(slot, &cfg);
        }
        if (!sidetnfs_probe_mount_slot(slot))
        {
            SidetnfsDriveErrorCategory err = sidetnfs_probe_classify_slot_error(slot, network_ok);
            if (err == SIDETNFS_DRIVE_ERR_NO_WIFI || err == SIDETNFS_DRIVE_ERR_DNS_FAILED ||
                err == SIDETNFS_DRIVE_ERR_NO_SESSION)
            {
                return SIDETNFS_FLOPPY_EMUL_ERR_TNFS_NOT_CONNECTED;
            }
            return SIDETNFS_FLOPPY_EMUL_ERR_TNFS_HOST_UNREACHABLE;
        }

        uint8_t handle = 0;
        SidetnfsFileOpenResult open_result = sidetnfs_tnfs_file_open(slot, image_path, 0 /* read-only */, &handle);
        if (open_result != SIDETNFS_FILE_OPEN_OK)
        {
            return (open_result == SIDETNFS_FILE_OPEN_NOT_FOUND) ? SIDETNFS_FLOPPY_EMUL_ERR_FILE_NOT_FOUND
                                                                    : SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR;
        }
        s_candidate.backend = FLOPPY_EMUL_BACKEND_TNFS;
        s_candidate.tnfs_slot = slot;
        s_candidate.tnfs_handle = handle;
        s_candidate.handle_open = true;

        uint32_t end_offset = 0;
        uint8_t seek_rc = 0xFFu;
        if (!sidetnfs_tnfs_file_seek(SIDETNFS_FLOPPY_EMUL_GUEST_FD, handle, slot, true, 0, &end_offset, &seek_rc) ||
            seek_rc != 0)
        {
            close_state(&s_candidate);
            return SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR;
        }
        filesize = end_offset;
    }
    else if (profile.backend == SIDETNFS_FLOPPY_BACKEND_SD)
    {
        if (profile.fields.sd.sd_path[0] == '\0')
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_SOURCE_NOT_CONFIGURED;
        }
        if (!sidetnfs_sd_service_has_run() || sidetnfs_sd_global_status() != SIDETNFS_SD_STATUS_READY)
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_SD_NOT_PRESENT;
        }

        char full_path[SIDETNFS_FLOPPY_SDPATH_LEN + SIDETNFS_FLOPPY_FAVORITE_PATH_MAX + 8];
        if (!join_sd_path(profile.fields.sd.sd_path, image_path, full_path, sizeof(full_path)))
        {
            return SIDETNFS_FLOPPY_EMUL_ERR_PATH_TOO_LONG;
        }

        FRESULT fr = f_open(&s_candidate.sd_file, full_path, FA_READ);
        if (fr != FR_OK)
        {
            if (fr == FR_NO_FILE || fr == FR_NO_PATH)
            {
                return SIDETNFS_FLOPPY_EMUL_ERR_FILE_NOT_FOUND;
            }
            if (fr == FR_DENIED)
            {
                return SIDETNFS_FLOPPY_EMUL_ERR_ACCESS_DENIED;
            }
            return SIDETNFS_FLOPPY_EMUL_ERR_BACKEND_ERROR;
        }
        s_candidate.backend = FLOPPY_EMUL_BACKEND_SD;
        s_candidate.handle_open = true;
        filesize = (uint32_t)f_size(&s_candidate.sd_file);
    }
    else
    {
        return SIDETNFS_FLOPPY_EMUL_ERR_INVALID_PROFILE; // unreachable in practice -- SET_PROFILE already rejects this
    }

    uint8_t sector0[NUM_BYTES_PER_SECTOR];
    if (filesize == 0 || (filesize % (uint32_t)NUM_BYTES_PER_SECTOR) != 0)
    {
        close_state(&s_candidate);
        return SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID;
    }
    if (read_sector_raw_from(&s_candidate, 0, sector0) != SIDETNFS_FLOPPY_EMUL_OK)
    {
        close_state(&s_candidate);
        return SIDETNFS_FLOPPY_EMUL_ERR_READ_FAILED;
    }

    sidetnfs_floppy_emul_status_t validate_result = sidetnfs_floppy_emul_validate_geometry(
        filesize, read_le16(sector0, 11), read_le16(sector0, 19), read_le16(sector0, 24), read_le16(sector0, 26),
        &s_candidate.geom);
    if (validate_result != SIDETNFS_FLOPPY_EMUL_OK)
    {
        close_state(&s_candidate);
        return validate_result;
    }

    s_candidate.ready = true;
    if (out_geom)
    {
        *out_geom = s_candidate.geom;
    }
    return SIDETNFS_FLOPPY_EMUL_OK;
}

// Atomically (no I/O, cannot fail) makes s_candidate the active backend.
// Must only be called after sidetnfs_floppy_emul_open_candidate() has
// returned SIDETNFS_FLOPPY_EMUL_OK. Transfers ownership of s_candidate's
// handle into s_state FIRST -- s_state already IS the new, validated
// backend before the OLD backend is closed -- so a caller can safely
// start publishing the new image's metadata (and, last of all,
// MEDIA_CHANGED) immediately after this returns, even though closing the
// old backend (which may perform TNFS/SD I/O) happens after the swap.
// The struct copy itself is a safe ownership transfer: floppy_emul_state_t
// holds no pointers back into itself -- FIL's only pointer (FFOBJID.fs)
// targets the single global mounted FATFS volume, not the FIL/state
// struct's own address, so it is unaffected by which copy of the struct
// holds it.
void sidetnfs_floppy_emul_commit_candidate(void)
{
    floppy_emul_state_t old_state = s_state;
    s_state = s_candidate;
    memset(&s_candidate, 0, sizeof(s_candidate)); // ownership transferred -- do not close what s_state now owns
    close_state(&old_state);
}

// Gives up on a candidate opened via sidetnfs_floppy_emul_open_candidate()
// without activating it -- closes its handle if any; s_state is
// untouched. Harmless no-op if there is no open candidate.
void sidetnfs_floppy_emul_discard_candidate(void)
{
    close_state(&s_candidate);
}

_Static_assert(NUM_BYTES_PER_SECTOR == 512, "sidetnfs_floppy_emul_validate_geometry() hardcodes 512 to stay Pico-SDK-free -- keep in sync with filesys.h's NUM_BYTES_PER_SECTOR");
