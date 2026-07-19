/**
 * File: sidetnfs_config.c
 * Description: Fase 9C -- load/validate/mutate/persist the SideTNFS
 * drive-list flash sector (SIDETNFS_CONFIG_FLASH_OFFSET, see
 * sidetnfs_config.h). Only sidetnfs_config_save() ever touches flash
 * (erase+program); every other function here only ever mutates the RAM
 * copy.
 */
#include "include/sidetnfs_config.h"

#include <stddef.h>
#include <string.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

// Fase 9C: built-in default whenever flash is blank/corrupt/unknown-version.
// Mirrors the hardcoded TNFS server sidetnfs_probe.c still uses to actually
// connect this phase (SIDETNFS_SERVER_IP/PORT/MOUNT_NAME there) -- kept in
// sync by hand for now. This module never changes the active runtime.
static const sidetnfs_drive_config_t SIDETNFS_DEFAULT_DRIVE = {
    .used = 1,
    .drive_letter = 'N',
    .type = SIDETNFS_DRIVE_TNFS,
    .transport = SIDETNFS_TRANSPORT_UDP,
    .port = 16384,
    .reserved0 = {0},
    .nickname = "RetroLoft",
    .host = "192.168.178.10",
    .mount_path = "Atari.ST",
    .sd_path = {0},
};

#define SIDETNFS_DEFAULT_CONFIG_DRIVE_LETTER 'S'

static sidetnfs_drive_flash_t g_config;
static bool g_config_ready = false;

// Standard bit-by-bit CRC32 (IEEE 802.3 / zlib polynomial 0xEDB88320, init
// and final XOR 0xFFFFFFFF). Same method as Fase 9B2 -- no table, this only
// ever runs at boot and at SAVE_CONFIG time over a ~1.5KB block.
static uint32_t sidetnfs_crc32(const uint8_t *data, size_t len)
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

static bool sidetnfs_is_valid_drive_letter(uint8_t letter)
{
    if (letter < 'A' || letter > 'Z')
    {
        return false;
    }
    if (letter == 'A' || letter == 'B')
    {
        return false;
    }
    return true;
}

// Validates drive_letter and the fields relevant to `type` (TNFS:
// transport/port/host/mount_path; SD: sd_path). Does not check letter
// uniqueness -- that requires comparing against the rest of the config, so
// it is done by the caller (sidetnfs_config_set_drive()/
// sidetnfs_config_validate_full()).
static sidetnfs_config_status_t sidetnfs_validate_drive_record(const sidetnfs_drive_config_t *drive)
{
    if (!sidetnfs_is_valid_drive_letter(drive->drive_letter))
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_LETTER;
    }

    if (drive->type == SIDETNFS_DRIVE_TNFS)
    {
        if (drive->transport != SIDETNFS_TRANSPORT_UDP && drive->transport != SIDETNFS_TRANSPORT_TCP)
        {
            return SIDETNFS_CONFIG_STATUS_INVALID_TRANSPORT;
        }
        if (drive->port == 0)
        {
            return SIDETNFS_CONFIG_STATUS_INVALID_PORT;
        }
        if (drive->host[0] == '\0')
        {
            return SIDETNFS_CONFIG_STATUS_INVALID_HOST;
        }
        if (drive->mount_path[0] == '\0')
        {
            return SIDETNFS_CONFIG_STATUS_INVALID_MOUNT_PATH;
        }
    }
    else if (drive->type == SIDETNFS_DRIVE_SD)
    {
        if (drive->sd_path[0] == '\0')
        {
            return SIDETNFS_CONFIG_STATUS_INVALID_SD_PATH;
        }
    }
    else
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_TYPE;
    }

    return SIDETNFS_CONFIG_STATUS_OK;
}

static void sidetnfs_config_force_nul_termination(sidetnfs_drive_flash_t *config)
{
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        config->drives[i].nickname[SIDETNFS_NICKNAME_LEN - 1] = '\0';
        config->drives[i].host[SIDETNFS_HOST_LEN - 1] = '\0';
        config->drives[i].mount_path[SIDETNFS_MOUNTPATH_LEN - 1] = '\0';
        config->drives[i].sd_path[SIDETNFS_SDPATH_LEN - 1] = '\0';
    }
}

static void sidetnfs_config_recompute_drive_count(void)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (g_config.drives[i].used)
        {
            count++;
        }
    }
    g_config.drive_count = count;
}

static void sidetnfs_config_load_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.magic = SIDETNFS_CONFIG_MAGIC;
    g_config.version = SIDETNFS_CONFIG_FLASH_VERSION;
    g_config.config_drive_letter = SIDETNFS_DEFAULT_CONFIG_DRIVE_LETTER;
    g_config.drive_count = 1;
    g_config.drives[0] = SIDETNFS_DEFAULT_DRIVE;
    sidetnfs_config_force_nul_termination(&g_config);
    // g_config.crc32 is deliberately left at 0 -- this in-RAM struct is
    // only written to flash by sidetnfs_config_save(), which always
    // recomputes crc32 itself before programming.
}

// Full structural + semantic validation of a candidate flash/RAM block:
// magic/version/CRC are NOT checked here (the caller decides whether those
// apply -- sidetnfs_config_init() checks them separately before ever
// calling this). Checks config_drive_letter, drive_count range, every used
// record's own fields, and full letter-uniqueness (every used record
// against every other used record and against config_drive_letter).
static sidetnfs_config_status_t sidetnfs_config_validate_structure(const sidetnfs_drive_flash_t *config)
{
    if (!sidetnfs_is_valid_drive_letter(config->config_drive_letter))
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_LETTER;
    }

    if (config->drive_count > SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_TOO_MANY_DRIVES;
    }

    uint8_t used_count = 0;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (!config->drives[i].used)
        {
            continue;
        }
        used_count++;

        sidetnfs_config_status_t rc = sidetnfs_validate_drive_record(&config->drives[i]);
        if (rc != SIDETNFS_CONFIG_STATUS_OK)
        {
            return rc;
        }

        if (config->drives[i].drive_letter == config->config_drive_letter)
        {
            return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
        }

        for (uint8_t j = (uint8_t)(i + 1); j < SIDETNFS_MAX_DRIVES; j++)
        {
            if (config->drives[j].used && config->drives[j].drive_letter == config->drives[i].drive_letter)
            {
                return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
            }
        }
    }

    if (used_count != config->drive_count)
    {
        return SIDETNFS_CONFIG_STATUS_TOO_MANY_DRIVES;
    }

    return SIDETNFS_CONFIG_STATUS_OK;
}

void sidetnfs_config_init(void)
{
    const sidetnfs_drive_flash_t *flash_ptr =
        (const sidetnfs_drive_flash_t *)(XIP_BASE + SIDETNFS_CONFIG_FLASH_OFFSET);

    // static: this struct is ~1.5KB, too large to safely put on this
    // target's small core0 stack (SCRATCH_Y, the region backing the
    // stack in romemul/memmap_romemul.ld, is only 4KB total). Safe as
    // static here -- sidetnfs_config_init() runs once, synchronously, from
    // main() before any command dispatch loop starts.
    static sidetnfs_drive_flash_t candidate;
    memcpy(&candidate, flash_ptr, sizeof(candidate));

    bool valid = (candidate.magic == SIDETNFS_CONFIG_MAGIC) &&
                 (candidate.version == SIDETNFS_CONFIG_FLASH_VERSION);

    if (valid)
    {
        uint32_t computed_crc = sidetnfs_crc32((const uint8_t *)&candidate, offsetof(sidetnfs_drive_flash_t, crc32));
        valid = (computed_crc == candidate.crc32);
    }

    if (valid)
    {
        valid = (sidetnfs_config_validate_structure(&candidate) == SIDETNFS_CONFIG_STATUS_OK);
    }

    if (!valid)
    {
        // Never trust a block that failed any check, even partially --
        // fall back to the full built-in default rather than salvaging
        // individual records out of it. This also transparently covers
        // pre-Fase-9C flash (old "STNF" magic/version 1): it simply fails
        // the magic check above and falls through to defaults here.
        sidetnfs_config_load_defaults();
        g_config_ready = true;
        return;
    }

    g_config = candidate;
    // Defensive even though the CRC already proves this is bit-for-bit
    // what was written: guarantees every reader downstream can treat these
    // as ordinary NUL-terminated C strings no matter what wrote the flash.
    sidetnfs_config_force_nul_termination(&g_config);

    g_config_ready = true;
}

uint32_t sidetnfs_config_get_max_drives(void)
{
    return SIDETNFS_MAX_DRIVES;
}

uint8_t sidetnfs_config_get_drive_count(void)
{
    return g_config_ready ? g_config.drive_count : 0;
}

uint8_t sidetnfs_config_get_config_drive_letter(void)
{
    return g_config_ready ? g_config.config_drive_letter : 0;
}

sidetnfs_config_status_t sidetnfs_config_get_drive(uint8_t index, sidetnfs_drive_config_t *out)
{
    memset(out, 0, sizeof(*out));

    if (index >= SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_INDEX;
    }

    if (!g_config_ready || !g_config.drives[index].used)
    {
        return SIDETNFS_CONFIG_STATUS_EMPTY_SLOT;
    }

    *out = g_config.drives[index];
    return SIDETNFS_CONFIG_STATUS_OK;
}

sidetnfs_config_status_t sidetnfs_config_set_drive(uint8_t index, const sidetnfs_drive_config_t *in)
{
    if (index >= SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_INDEX;
    }

    if (!in->used)
    {
        // Clearing via SET_DRIVE(used=0) is equivalent to DELETE_DRIVE --
        // no type-specific field validation applies to a slot being
        // marked unused.
        memset(&g_config.drives[index], 0, sizeof(g_config.drives[index]));
        sidetnfs_config_recompute_drive_count();
        return SIDETNFS_CONFIG_STATUS_OK;
    }

    sidetnfs_drive_config_t candidate = *in;
    candidate.used = 1;
    candidate.reserved0[0] = 0;
    candidate.reserved0[1] = 0;
    candidate.nickname[SIDETNFS_NICKNAME_LEN - 1] = '\0';
    candidate.host[SIDETNFS_HOST_LEN - 1] = '\0';
    candidate.mount_path[SIDETNFS_MOUNTPATH_LEN - 1] = '\0';
    candidate.sd_path[SIDETNFS_SDPATH_LEN - 1] = '\0';

    sidetnfs_config_status_t rc = sidetnfs_validate_drive_record(&candidate);
    if (rc != SIDETNFS_CONFIG_STATUS_OK)
    {
        return rc;
    }

    if (candidate.drive_letter == g_config.config_drive_letter)
    {
        return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
    }

    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (i == index)
        {
            continue;
        }
        if (g_config.drives[i].used && g_config.drives[i].drive_letter == candidate.drive_letter)
        {
            return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
        }
    }

    // Zero the fields that don't apply to this record's type.
    if (candidate.type == SIDETNFS_DRIVE_SD)
    {
        candidate.transport = 0;
        candidate.port = 0;
        memset(candidate.host, 0, sizeof(candidate.host));
        memset(candidate.mount_path, 0, sizeof(candidate.mount_path));
    }
    else // SIDETNFS_DRIVE_TNFS -- the only other type sidetnfs_validate_drive_record() accepts
    {
        memset(candidate.sd_path, 0, sizeof(candidate.sd_path));
    }

    g_config.drives[index] = candidate;
    sidetnfs_config_recompute_drive_count();
    return SIDETNFS_CONFIG_STATUS_OK;
}

sidetnfs_config_status_t sidetnfs_config_delete_drive(uint8_t index)
{
    if (index >= SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_INDEX;
    }

    if (!g_config.drives[index].used)
    {
        return SIDETNFS_CONFIG_STATUS_EMPTY_SLOT;
    }

    memset(&g_config.drives[index], 0, sizeof(g_config.drives[index]));
    sidetnfs_config_recompute_drive_count();
    return SIDETNFS_CONFIG_STATUS_OK;
}

sidetnfs_config_status_t sidetnfs_config_set_config_drive_letter(uint8_t new_letter)
{
    if (!sidetnfs_is_valid_drive_letter(new_letter))
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_LETTER;
    }

    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (g_config.drives[i].used && g_config.drives[i].drive_letter == new_letter)
        {
            return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
        }
    }

    g_config.config_drive_letter = new_letter;
    return SIDETNFS_CONFIG_STATUS_OK;
}

// Rounded up to a whole number of flash program pages (FLASH_PAGE_SIZE,
// 256 bytes on RP2040) -- flash_range_program() requires its count to be a
// page multiple. 1552 bytes -> 1792 (7 pages), still well within the 4096-
// byte sector, so only the needed pages are programmed, not the whole
// sector.
#define SIDETNFS_CONFIG_PROGRAM_SIZE (((sizeof(sidetnfs_drive_flash_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)
_Static_assert(SIDETNFS_CONFIG_PROGRAM_SIZE <= SIDETNFS_CONFIG_FLASH_SIZE, "SIDETNFS_CONFIG_PROGRAM_SIZE no longer fits in one SIDETNFS_CONFIG_FLASH_SIZE sector");

sidetnfs_config_status_t sidetnfs_config_save(void)
{
    // Recompute drive_count from the used bitmap right before validating,
    // so "drive_count equals the number of used records" is always true
    // for whatever gets persisted, regardless of how it got here.
    sidetnfs_config_recompute_drive_count();

    sidetnfs_config_status_t validate_result = sidetnfs_config_validate_structure(&g_config);
    if (validate_result != SIDETNFS_CONFIG_STATUS_OK)
    {
        return validate_result;
    }

    // Build a clean image: reserved bytes zeroed, unused records fully
    // zeroed, type-irrelevant fields zeroed, strings NUL-terminated.
    // static: same stack-size reasoning as sidetnfs_config_init() above --
    // this function is only ever called synchronously from the single
    // GEMDRIVE command dispatch loop, never re-entrantly.
    static sidetnfs_drive_flash_t clean;
    memset(&clean, 0, sizeof(clean));
    clean.magic = SIDETNFS_CONFIG_MAGIC;
    clean.version = SIDETNFS_CONFIG_FLASH_VERSION;
    clean.config_drive_letter = g_config.config_drive_letter;
    clean.drive_count = g_config.drive_count;

    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (!g_config.drives[i].used)
        {
            continue; // already zeroed by the memset above
        }
        clean.drives[i] = g_config.drives[i];
        clean.drives[i].used = 1;
        clean.drives[i].reserved0[0] = 0;
        clean.drives[i].reserved0[1] = 0;
        if (clean.drives[i].type == SIDETNFS_DRIVE_SD)
        {
            clean.drives[i].transport = 0;
            clean.drives[i].port = 0;
            memset(clean.drives[i].host, 0, sizeof(clean.drives[i].host));
            memset(clean.drives[i].mount_path, 0, sizeof(clean.drives[i].mount_path));
        }
        else
        {
            memset(clean.drives[i].sd_path, 0, sizeof(clean.drives[i].sd_path));
        }
    }
    sidetnfs_config_force_nul_termination(&clean);

    clean.crc32 = sidetnfs_crc32((const uint8_t *)&clean, offsetof(sidetnfs_drive_flash_t, crc32));

    static uint8_t program_buf[SIDETNFS_CONFIG_PROGRAM_SIZE];
    memset(program_buf, 0, sizeof(program_buf));
    memcpy(program_buf, &clean, sizeof(clean));

    // Same pattern romemul/config.c's write_all_entries() already uses:
    // interrupts disabled only around the erase+program pair, exactly one
    // sector erased, then interrupts restored immediately.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SIDETNFS_CONFIG_FLASH_OFFSET, SIDETNFS_CONFIG_FLASH_SIZE);
    flash_range_program(SIDETNFS_CONFIG_FLASH_OFFSET, program_buf, sizeof(program_buf));
    restore_interrupts(ints);

    const sidetnfs_drive_flash_t *flash_ptr =
        (const sidetnfs_drive_flash_t *)(XIP_BASE + SIDETNFS_CONFIG_FLASH_OFFSET);
    static sidetnfs_drive_flash_t readback;
    memcpy(&readback, flash_ptr, sizeof(readback));

    if (readback.magic != SIDETNFS_CONFIG_MAGIC || readback.version != SIDETNFS_CONFIG_FLASH_VERSION)
    {
        return SIDETNFS_CONFIG_STATUS_FLASH_WRITE_FAILED;
    }

    uint32_t computed_crc = sidetnfs_crc32((const uint8_t *)&readback, offsetof(sidetnfs_drive_flash_t, crc32));
    if (computed_crc != readback.crc32)
    {
        return SIDETNFS_CONFIG_STATUS_CRC_MISMATCH;
    }

    // Success -- mirror the exact persisted (clean) image in RAM too.
    g_config = clean;
    g_config_ready = true;
    return SIDETNFS_CONFIG_STATUS_OK;
}
