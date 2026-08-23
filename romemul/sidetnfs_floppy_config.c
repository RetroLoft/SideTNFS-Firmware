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
 */
#include "include/sidetnfs_floppy_config.h"

#include <stddef.h>
#include <string.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

static sidetnfs_floppy_flash_t g_config;
static bool g_config_ready = false;
static bool g_config_loaded_from_flash = false;

// Same bit-by-bit CRC32 (IEEE 802.3/zlib polynomial 0xEDB88320, init/final
// XOR 0xFFFFFFFF) as sidetnfs_config.c's sidetnfs_crc32() -- duplicated
// rather than shared across modules, same reasoning main.c's
// atari_do_reset() duplication comment gives for small, self-contained
// helpers in this codebase: this only ever runs at boot and at
// SAVE_PROFILES time over a ~3KB block, not a hot path worth sharing
// plumbing for.
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
        config->profiles[i].nickname[SIDETNFS_FLOPPY_NICKNAME_LEN - 1] = '\0';
        config->profiles[i].host[SIDETNFS_FLOPPY_HOST_LEN - 1] = '\0';
        config->profiles[i].mount_path[SIDETNFS_FLOPPY_MOUNTPATH_LEN - 1] = '\0';
        config->profiles[i].last_directory[SIDETNFS_FLOPPY_LASTDIR_LEN - 1] = '\0';
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
    // no server address it can assume, so a fresh/blank flash means "no
    // profiles configured yet", surfaced to the user by profile_count==0.
}

// Validates nickname/host/port for a DISABLED/ENABLED record.
// mount_path/last_directory may be empty ("server root" / "not yet
// browsed"). Does not check index range or state -- callers do that.
static sidetnfs_floppy_config_status_t sidetnfs_validate_profile_record(const sidetnfs_floppy_profile_config_t *p)
{
    if (p->nickname[0] == '\0')
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_NICKNAME;
    }
    if (p->host[0] == '\0')
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_HOST;
    }
    if (p->port == 0)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_PORT;
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

void sidetnfs_floppy_config_init(void)
{
    const sidetnfs_floppy_flash_t *flash_ptr =
        (const sidetnfs_floppy_flash_t *)(XIP_BASE + SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET);

    // static: ~3KB, too large for this target's small core0 stack (see
    // sidetnfs_config_init()'s identical reasoning). Runs once,
    // synchronously, from main() before any command dispatch loop starts.
    static sidetnfs_floppy_flash_t candidate;
    memcpy(&candidate, flash_ptr, sizeof(candidate));

    bool valid = (candidate.magic == SIDETNFS_FLOPPY_CONFIG_MAGIC);
    if (valid && candidate.version != SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION)
    {
        // No older version exists yet -- this store starts at version 1.
        // A future format change should add a migration path here, the
        // same way sidetnfs_config.c's v2->v3 migration works, rather than
        // widening this check.
        valid = false;
    }
    if (valid)
    {
        uint32_t computed_crc = sidetnfs_floppy_crc32((const uint8_t *)&candidate, offsetof(sidetnfs_floppy_flash_t, crc32));
        valid = (computed_crc == candidate.crc32);
    }
    if (valid)
    {
        valid = (sidetnfs_floppy_config_validate_structure(&candidate) == SIDETNFS_FLOPPY_STATUS_OK);
    }

    if (!valid)
    {
        // Never trust a block that failed any check, even partially --
        // fall back to the built-in (all-empty) default rather than
        // salvaging individual records. This also transparently covers
        // blank/erased flash (all 0xFF, fails the magic check).
        sidetnfs_floppy_config_load_defaults();
        g_config_ready = true;
        g_config_loaded_from_flash = false;
        return;
    }

    g_config = candidate;
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

sidetnfs_floppy_config_status_t sidetnfs_floppy_config_set_profile(uint8_t index, const sidetnfs_floppy_profile_config_t *in)
{
    if (index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return SIDETNFS_FLOPPY_STATUS_INVALID_INDEX;
    }

    if (in->state == SIDETNFS_FLOPPY_PROFILE_EMPTY)
    {
        return sidetnfs_floppy_config_delete_profile(index);
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

    memset(&g_config.profiles[index], 0, sizeof(g_config.profiles[index]));
    if (g_config.active_profile_index == index)
    {
        g_config.active_profile_index = 0;
    }
    sidetnfs_floppy_config_recompute_profile_count();
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

    // Build a clean image: reserved bytes zeroed, EMPTY records fully
    // zeroed, DISABLED/ENABLED records preserved with their real state,
    // strings NUL-terminated -- same approach sidetnfs_config_save() uses.
    static sidetnfs_floppy_flash_t clean;
    memset(&clean, 0, sizeof(clean));
    clean.magic = SIDETNFS_FLOPPY_CONFIG_MAGIC;
    clean.version = SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION;
    clean.active_profile_index = g_config.active_profile_index;
    clean.profile_count = g_config.profile_count;

    for (uint8_t i = 0; i < SIDETNFS_FLOPPY_MAX_PROFILES; i++)
    {
        if (sidetnfs_floppy_profile_is_empty(&g_config.profiles[i]))
        {
            continue; // already zeroed by the memset above
        }
        clean.profiles[i] = g_config.profiles[i];
        clean.profiles[i].reserved0[0] = 0;
        clean.profiles[i].reserved0[1] = 0;
        clean.profiles[i].reserved0[2] = 0;
        clean.profiles[i].reserved1[0] = 0;
        clean.profiles[i].reserved1[1] = 0;
    }
    sidetnfs_floppy_config_force_nul_termination(&clean);

    clean.crc32 = sidetnfs_floppy_crc32((const uint8_t *)&clean, offsetof(sidetnfs_floppy_flash_t, crc32));

    static uint8_t program_buf[SIDETNFS_FLOPPY_CONFIG_PROGRAM_SIZE];
    memset(program_buf, 0, sizeof(program_buf));
    memcpy(program_buf, &clean, sizeof(clean));

    // Same pattern romemul/config.c's write_all_entries() and
    // sidetnfs_config_save() already use: interrupts disabled only around
    // the erase+program pair, exactly one sector erased.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET, SIDETNFS_FLOPPY_CONFIG_FLASH_SIZE);
    flash_range_program(SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET, program_buf, sizeof(program_buf));
    restore_interrupts(ints);

    const sidetnfs_floppy_flash_t *flash_ptr =
        (const sidetnfs_floppy_flash_t *)(XIP_BASE + SIDETNFS_FLOPPY_CONFIG_FLASH_OFFSET);
    static sidetnfs_floppy_flash_t readback;
    memcpy(&readback, flash_ptr, sizeof(readback));

    if (readback.magic != SIDETNFS_FLOPPY_CONFIG_MAGIC || readback.version != SIDETNFS_FLOPPY_CONFIG_FLASH_VERSION)
    {
        return SIDETNFS_FLOPPY_STATUS_FLASH_WRITE_FAILED;
    }

    uint32_t computed_crc = sidetnfs_floppy_crc32((const uint8_t *)&readback, offsetof(sidetnfs_floppy_flash_t, crc32));
    if (computed_crc != readback.crc32)
    {
        return SIDETNFS_FLOPPY_STATUS_CRC_MISMATCH;
    }

    // Success -- mirror the exact persisted (clean) image in RAM too.
    g_config = clean;
    g_config_ready = true;
    g_config_loaded_from_flash = true;
    return SIDETNFS_FLOPPY_STATUS_OK;
}
