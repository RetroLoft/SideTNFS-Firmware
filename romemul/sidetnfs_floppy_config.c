/**
 * File: sidetnfs_floppy_config.c
 * Description: -- load/validate/mutate/persist the FLOPPY.PRG
 * server-profile flash sector (SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET, see
 * sidetnfs_floppy_config.h). Only sidetnfs_floppy_config_save() ever
 * touches flash (erase+program); every other function here only ever
 * mutates the RAM copy -- same discipline sidetnfs_config.c uses for the
 * GEMDOS drive list, so flash wear stays bounded to one erase/program
 * cycle per explicit user "Save", never per field edit or per active-
 * profile change.
 *
 * Persistence policy for active_profile_index / last_directory (see the
 * project's RESEARCH-STEP0.md section 6): selecting a different active
 * profile, or a future browser updating a profile's last_directory, only
 * ever changes the RAM copy here -- neither is written to flash until the
 * user explicitly saves (SET_PROFILE/SET_ACTIVE_PROFILE + SAVE_PROFILES).
 * This means the "auto-restore last server/directory on next boot" goal
 * from the project brief is satisfied only up to the last explicit Save,
 * not up to the very last change made in a session -- this is a
 * deliberate, documented trade-off, not an oversight: an alternative
 * "save automatically on quit" policy was considered and rejected for this
 * phase because it would silently reintroduce frequent flash writes,
 * which the project brief explicitly asked to avoid. Revisit only if the
 * user explicitly asks for a different policy.
 *
 * RAM DISCIPLINE: only TWO static whole-image-sized buffers exist in this
 * whole module -- g_config (persistent, must stay resident for GET/SET-
 * PROFILE RAM access) and save()'s own page-rounded program_buf. This is
 * deliberately leaner than the "N full static copies" pattern
 * sidetnfs_config.c uses for its own (much smaller) drive-list struct:
 * init() validates candidate data directly against the XIP-mapped flash
 * pointer instead of memcpy-ing it into a scratch buffer first, and
 * save()'s own readback verification reads directly from the XIP pointer
 * too, instead of a second RAM copy. See sidetnfs_floppy_config.h's own
 * top-of-file note for why this matters on this target.
 */
#include "include/sidetnfs_floppy_config.h"

#include <stddef.h>
#include <string.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

static sidetnfs_floppy_flash_t g_config;
static bool g_config_ready = false;
static bool g_config_loaded_from_flash = false;
static bool g_config_migrated_from_v1 = false;

// Same bit-by-bit CRC32 (IEEE 802.3/zlib polynomial 0xEDB88320, init/final
// XOR 0xFFFFFFFF) as sidetnfs_config.c's sidetnfs_crc32() -- duplicated
// rather than shared across modules, same reasoning main.c's
// atari_do_reset() duplication comment gives for small, self-contained
// helpers in this codebase. Operates on any readable byte range, RAM or
// XIP-mapped flash alike -- callers below pass both.
static uint32_t sidetnfs_floppy_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void sidetnfs_floppy_config_force_nul_termination(sidetnfs_floppy_flash_t *config)
{
    for (uint8_t i = 0; i < SIDETNFS_FLOPPY_MAX_PROFILES; i++)
    {
        sidetnfs_floppy_profile_config_t *p = &config->profiles[i];
        p->nickname[SIDETNFS_FLOPPY_NICKNAME_LEN - 1] = '\0';
        p->last_directory[SIDETNFS_FLOPPY_LASTDIR_LEN - 1] = '\0';
        // Force-terminate BOTH union interpretations unconditionally --
        // cheap, and avoids ever trusting an unterminated string no matter
        // which one `backend` claims is active (defensive, same spirit as
        // sidetnfs_config.c's own force_nul_termination()).
        p->fields.tnfs.host[SIDETNFS_FLOPPY_HOST_LEN - 1] = '\0';
        p->fields.tnfs.mount_path[SIDETNFS_FLOPPY_MOUNTPATH_LEN - 1] = '\0';
        p->fields.sd.sd_path[SIDETNFS_FLOPPY_SDPATH_LEN - 1] = '\0';
    }
}

static void sidetnfs_floppy_config_recompute_profile_count(void)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < SIDETNFS_FLOPPY_MAX_PROFILES; i++)
    {
        if (sidetnfs_floppy_profile_is_configured(&g_config.profiles[i]))
        {
            count++;
        }
    }
    g_config.profile_count = count;
}

static void sidetnfs_floppy_config_load_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.magic = SIDETNFS_FLOPPY_CONFIG_MAGIC;
    g_config.version = SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION;
    g_config.active_profile_index = 0;
    g_config.profile_count = 0;
    // Every slot already EMPTY from the memset -- unlike the GEMDOS drive
    // list, there is no built-in "first profile" default: FLOPPY.PRG has
    // no server address or SD path it can assume, so a fresh/blank flash
    // means "no profiles configured yet", surfaced to the user by
    // profile_count==0.
}

// Validates nickname/backend and the backend-specific fields for a
// DISABLED/ENABLED record. last_directory may always be empty ("not yet
// browsed"). Does not check index range or state -- callers do that.
static sidetnfs_floppy_config_status_t sidetnfs_validate_profile_record(const sidetnfs_floppy_profile_config_t *p)
{
    if (p->nickname[0] == '\0')
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_NICKNAME;
    }

    switch (p->backend)
    {
    case SIDETNFS_FLOPPY_BACKEND_TNFS:
        if (p->fields.tnfs.host[0] == '\0')
        {
            return SIDETNFS_FLOPPY_STATUS_INVALID_HOST;
        }
        if (p->port == 0)
        {
            return SIDETNFS_FLOPPY_STATUS_INVALID_PORT;
        }
        // mount_path may be empty ("server root") -- not validated further.
        break;
    case SIDETNFS_FLOPPY_BACKEND_SD:
        if (p->fields.sd.sd_path[0] == '\0')
        {
            return SIDETNFS_FLOPPY_STATUS_INVALID_SD_PATH;
        }
        // host/port are never stored or validated for an SD profile.
        break;
    default:
        return SIDETNFS_FLOPPY_STATUS_INVALID_BACKEND;
    }

    return SIDETNFS_FLOPPY_STATUS_OK;
}

static sidetnfs_floppy_config_status_t sidetnfs_floppy_config_validate_structure(const sidetnfs_floppy_flash_t *config)
{
    if (config->active_profile_index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_INDEX;
    }
    for (uint8_t i = 0; i < SIDETNFS_FLOPPY_MAX_PROFILES; i++)
    {
        const sidetnfs_floppy_profile_config_t *p = &config->profiles[i];
        if (sidetnfs_floppy_profile_is_empty(p))
        {
            continue;
        }
        if (!sidetnfs_floppy_profile_is_configured(p))
        {
            return SIDETNFS_FLOPPY_STATUS_INVALID_PROFILE_STATE;
        }
        sidetnfs_floppy_config_status_t record_status = sidetnfs_validate_profile_record(p);
        if (record_status != SIDETNFS_FLOPPY_STATUS_OK)
        {
            return record_status;
        }
    }
    return SIDETNFS_FLOPPY_STATUS_OK;
}

// Migrates one v1 (TNFS-only, no `backend` field) record into its v2
// equivalent, explicitly tagging it SIDETNFS_FLOPPY_BACKEND_TNFS -- the
// project brief's explicit requirement for how an old profile without a
// backend field must be interpreted. EMPTY records stay EMPTY (the caller
// already zeroed the destination).
static void sidetnfs_floppy_migrate_profile_v1_to_v2(const sidetnfs_floppy_profile_config_v1_t *old, sidetnfs_floppy_profile_config_t *out)
{
    if (old->state != SIDETNFS_FLOPPY_PROFILE_DISABLED && old->state != SIDETNFS_FLOPPY_PROFILE_ENABLED)
    {
        return; // leave *out fully zeroed (EMPTY)
    }
    out->state = old->state;
    out->backend = SIDETNFS_FLOPPY_BACKEND_TNFS;
    out->port = old->port;
    memcpy(out->nickname, old->nickname, SIDETNFS_FLOPPY_NICKNAME_LEN);
    memcpy(out->last_directory, old->last_directory, SIDETNFS_FLOPPY_LASTDIR_LEN);
    memcpy(out->fields.tnfs.host, old->host, SIDETNFS_FLOPPY_HOST_LEN);
    memcpy(out->fields.tnfs.mount_path, old->mount_path, SIDETNFS_FLOPPY_MOUNTPATH_LEN);
}

// Validates a v1 block's OWN CRC (over the v1 layout/size) directly
// against XIP-mapped flash -- no RAM copy of the v1 image is ever made.
static bool sidetnfs_floppy_v1_crc_valid(const sidetnfs_floppy_flash_v1_t *old)
{
    uint32_t computed_crc = sidetnfs_floppy_crc32((const uint8_t *)old, offsetof(sidetnfs_floppy_flash_v1_t, crc32));
    return computed_crc == old->crc32;
}

void sidetnfs_floppy_config_init(void)
{
    const sidetnfs_floppy_flash_t *flash_ptr =
        (const sidetnfs_floppy_flash_t *)(XIP_BASE + SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET);

    g_config_migrated_from_v1 = false;

    if (flash_ptr->magic != SIDETNFS_FLOPPY_CONFIG_MAGIC)
    {
        // Blank/erased flash or a foreign block -- fall back to defaults.
        sidetnfs_floppy_config_load_defaults();
        g_config_ready = true;
        g_config_loaded_from_flash = false;
        return;
    }

    if (flash_ptr->version == SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION_V1)
    {
        // Reinterpret the SAME flash bytes through the v1 layout -- no RAM
        // copy of the v1 image, see this function's own header comment.
        const sidetnfs_floppy_flash_v1_t *old =
            (const sidetnfs_floppy_flash_v1_t *)(XIP_BASE + SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET);

        if (!sidetnfs_floppy_v1_crc_valid(old))
        {
            sidetnfs_floppy_config_load_defaults();
            g_config_ready = true;
            g_config_loaded_from_flash = false;
            return;
        }

        memset(&g_config, 0, sizeof(g_config));
        g_config.magic = SIDETNFS_FLOPPY_CONFIG_MAGIC;
        g_config.version = SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION; // migrated in RAM to the CURRENT format
        g_config.active_profile_index = (old->active_profile_index < SIDETNFS_FLOPPY_MAX_PROFILES) ? old->active_profile_index : 0;
        for (uint8_t i = 0; i < SIDETNFS_FLOPPY_MAX_PROFILES; i++)
        {
            sidetnfs_floppy_migrate_profile_v1_to_v2(&old->profiles[i], &g_config.profiles[i]);
        }
        sidetnfs_floppy_config_force_nul_termination(&g_config);
        sidetnfs_floppy_config_recompute_profile_count();

        // A migrated block still has to pass the same structural
        // validation a native v2 block would -- never trust a v1 block
        // blindly just because its own CRC checked out.
        if (sidetnfs_floppy_config_validate_structure(&g_config) != SIDETNFS_FLOPPY_STATUS_OK)
        {
            sidetnfs_floppy_config_load_defaults();
            g_config_ready = true;
            g_config_loaded_from_flash = false;
            return;
        }

        g_config_ready = true;
        g_config_loaded_from_flash = true;
        g_config_migrated_from_v1 = true;
        return;
    }

    if (flash_ptr->version != SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION)
    {
        // Neither the current format nor the one older migratable
        // version -- genuinely unsupported. Fall back to defaults.
        sidetnfs_floppy_config_load_defaults();
        g_config_ready = true;
        g_config_loaded_from_flash = false;
        return;
    }

    // Current version: validate directly against the XIP-mapped pointer
    // (no RAM copy) before ever committing anything to g_config.
    uint32_t computed_crc = sidetnfs_floppy_crc32((const uint8_t *)flash_ptr, offsetof(sidetnfs_floppy_flash_t, crc32));
    if (computed_crc != flash_ptr->crc32)
    {
        sidetnfs_floppy_config_load_defaults();
        g_config_ready = true;
        g_config_loaded_from_flash = false;
        return;
    }
    if (sidetnfs_floppy_config_validate_structure(flash_ptr) != SIDETNFS_FLOPPY_STATUS_OK)
    {
        sidetnfs_floppy_config_load_defaults();
        g_config_ready = true;
        g_config_loaded_from_flash = false;
        return;
    }

    // All checks passed against the flash-mapped data directly -- only
    // now is it copied into the persistent RAM copy.
    g_config = *flash_ptr;
    g_config_loaded_from_flash = true;
    // Defensive even though the CRC already proves this is bit-for-bit
    // what was written -- guarantees every reader downstream can treat
    // these as ordinary NUL-terminated C strings no matter what wrote the
    // flash.
    sidetnfs_floppy_config_force_nul_termination(&g_config);
    g_config_ready = true;
}

bool sidetnfs_floppy_config_loaded_from_flash(void)
{
    return g_config_loaded_from_flash;
}

bool sidetnfs_floppy_config_migrated_from_v1(void)
{
    return g_config_migrated_from_v1;
}

uint32_t sidetnfs_floppy_config_get_max_profiles(void)
{
    return SIDETNFS_FLOPPY_MAX_PROFILES;
}

uint8_t sidetnfs_floppy_config_get_profile_count(void)
{
    return g_config_ready ? g_config.profile_count : 0;
}

uint8_t sidetnfs_floppy_config_get_active_profile_index(void)
{
    return g_config_ready ? g_config.active_profile_index : 0;
}

sidetnfs_floppy_config_status_t sidetnfs_floppy_config_get_profile(uint8_t index, sidetnfs_floppy_profile_config_t *out)
{
    memset(out, 0, sizeof(*out));
    if (index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_INDEX;
    }
    *out = g_config.profiles[index];
    return SIDETNFS_FLOPPY_STATUS_OK;
}

// Shared clearing logic for DELETE_PROFILE and SET_PROFILE's own
// state==EMPTY case -- idempotent, always succeeds regardless of whether
// the slot was already empty. Callers with a stricter "there must be
// something here to delete" contract (DELETE_PROFILE) check
// sidetnfs_floppy_profile_is_empty() themselves BEFORE calling this; this
// function itself never reports EMPTY_SLOT.
static void clear_profile_slot(uint8_t index)
{
    memset(&g_config.profiles[index], 0, sizeof(g_config.profiles[index]));
    if (g_config.active_profile_index == index)
    {
        g_config.active_profile_index = 0;
    }
    sidetnfs_floppy_config_recompute_profile_count();
}

sidetnfs_floppy_config_status_t sidetnfs_floppy_config_set_profile(uint8_t index, const sidetnfs_floppy_profile_config_t *in)
{
    if (index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_INDEX;
    }

    if (in->state == SIDETNFS_FLOPPY_PROFILE_EMPTY)
    {
        // Idempotent "ensure this slot is empty" -- NOT the same contract
        // as DELETE_PROFILE below (which correctly errors on an
        // already-empty slot, since an explicit user Delete on nothing is
        // a caller mistake). SET_PROFILE's own contract is "the record
        // now looks like `in`", so setting an EMPTY record onto an
        // already-EMPTY slot must succeed trivially. This matters because
        // FLOPPY.PRG's perform_save() (dialog.c) calls SET_PROFILE once
        // per slot for the FULL 8-slot array on every save, including
        // every currently-unconfigured slot -- treating "already empty"
        // as an error here made every ordinary save (fewer than 8
        // profiles configured) fail on the first unused slot.
        clear_profile_slot(index);
        return SIDETNFS_FLOPPY_STATUS_OK;
    }
    if (in->state != SIDETNFS_FLOPPY_PROFILE_DISABLED && in->state != SIDETNFS_FLOPPY_PROFILE_ENABLED)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_PROFILE_STATE;
    }

    sidetnfs_floppy_config_status_t record_status = sidetnfs_validate_profile_record(in);
    if (record_status != SIDETNFS_FLOPPY_STATUS_OK)
    {
        return record_status;
    }

    g_config.profiles[index] = *in;
    sidetnfs_floppy_config_force_nul_termination(&g_config);
    sidetnfs_floppy_config_recompute_profile_count();
    return SIDETNFS_FLOPPY_STATUS_OK;
}

sidetnfs_floppy_config_status_t sidetnfs_floppy_config_delete_profile(uint8_t index)
{
    if (index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_INDEX;
    }
    if (sidetnfs_floppy_profile_is_empty(&g_config.profiles[index]))
    {
        return SIDETNFS_FLOPPY_STATUS_EMPTY_SLOT;
    }

    clear_profile_slot(index);
    return SIDETNFS_FLOPPY_STATUS_OK;
}

sidetnfs_floppy_config_status_t sidetnfs_floppy_config_set_active_profile(uint8_t index)
{
    if (index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_INDEX;
    }
    if (!sidetnfs_floppy_profile_is_configured(&g_config.profiles[index]))
    {
        return SIDETNFS_FLOPPY_STATUS_EMPTY_SLOT;
    }
    g_config.active_profile_index = index;
    return SIDETNFS_FLOPPY_STATUS_OK;
}

#define SIDETNFS_FLOPPY_CONFIG_PROGRAM_SIZE (((sizeof(sidetnfs_floppy_flash_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)
_Static_assert(SIDETNFS_FLOPPY_CONFIG_PROGRAM_SIZE <= SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE, "SIDETNFS_FLOPPY_CONFIG_PROGRAM_SIZE no longer fits in one SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE sector");

sidetnfs_floppy_config_status_t sidetnfs_floppy_config_save(void)
{
    sidetnfs_floppy_config_recompute_profile_count();

    sidetnfs_floppy_config_status_t validate_result = sidetnfs_floppy_config_validate_structure(&g_config);
    if (validate_result != SIDETNFS_FLOPPY_STATUS_OK)
    {
        return validate_result;
    }

    // Build the clean image directly in the page-rounded program buffer
    // (the ONE extra static buffer this module uses -- see the file
    // header) rather than in a separate "clean" struct plus a third
    // "readback" struct: this IS the buffer that gets programmed to
    // flash, and readback below verifies against the flash-mapped pointer
    // directly, so no third/fourth buffer is needed at all.
    static uint8_t program_buf[SIDETNFS_FLOPPY_CONFIG_PROGRAM_SIZE];
    memset(program_buf, 0, sizeof(program_buf));
    sidetnfs_floppy_flash_t *clean = (sidetnfs_floppy_flash_t *)program_buf;

    clean->magic = SIDETNFS_FLOPPY_CONFIG_MAGIC;
    clean->version = SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION;
    clean->active_profile_index = g_config.active_profile_index;
    clean->profile_count = g_config.profile_count;

    for (uint8_t i = 0; i < SIDETNFS_FLOPPY_MAX_PROFILES; i++)
    {
        if (sidetnfs_floppy_profile_is_empty(&g_config.profiles[i]))
        {
            continue; // already zeroed by the memset above
        }
        // Preserve the record as-is, including whichever union member is
        // actually meaningful for its backend -- SET_PROFILE already
        // guarantees the record (and therefore the union's raw bytes) is
        // fully, deterministically populated for its own backend (see
        // sidetnfs_floppy_config_set_profile()), so there is nothing to
        // selectively re-clear here. Deliberately NOT attempting to zero
        // "the other union member": since the two members physically
        // overlap, doing so would corrupt whichever one is actually in
        // use (e.g. zeroing the 96-byte TNFS view when backend==SD would
        // destroy the first 96 bytes of sd_path).
        clean->profiles[i] = g_config.profiles[i];
    }
    sidetnfs_floppy_config_force_nul_termination(clean);

    clean->crc32 = sidetnfs_floppy_crc32((const uint8_t *)clean, offsetof(sidetnfs_floppy_flash_t, crc32));

    // Same pattern romemul/config.c's write_all_entries() and
    // sidetnfs_config_save() already use: interrupts disabled only around
    // the erase+program pair, exactly one sector erased.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET, SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE);
    flash_range_program(SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET, program_buf, sizeof(program_buf));
    restore_interrupts(ints);

    // Readback verification straight from the XIP-mapped flash pointer --
    // no second RAM copy (see this module's own RAM discipline note).
    const sidetnfs_floppy_flash_t *readback_ptr =
        (const sidetnfs_floppy_flash_t *)(XIP_BASE + SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET);

    if (readback_ptr->magic != SIDETNFS_FLOPPY_CONFIG_MAGIC || readback_ptr->version != SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION)
    {
        return SIDETNFS_FLOPPY_STATUS_FLASH_WRITE_FAILED;
    }

    uint32_t computed_crc = sidetnfs_floppy_crc32((const uint8_t *)readback_ptr, offsetof(sidetnfs_floppy_flash_t, crc32));
    if (computed_crc != readback_ptr->crc32)
    {
        return SIDETNFS_FLOPPY_STATUS_CRC_MISMATCH;
    }

    // Success -- mirror the exact persisted (clean) image in RAM too.
    g_config = *clean;
    g_config_ready = true;
    g_config_loaded_from_flash = true;
    g_config_migrated_from_v1 = false; // now natively v2 on flash
    return SIDETNFS_FLOPPY_STATUS_OK;
}
