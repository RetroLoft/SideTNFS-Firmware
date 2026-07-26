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
#include <stdio.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

// Fase 9C: built-in default whenever flash is blank/corrupt/unknown-version.
// Mirrors the hardcoded TNFS server sidetnfs_probe.c still uses to actually
// connect this phase (SIDETNFS_SERVER_IP/PORT/MOUNT_NAME there) -- kept in
// sync by hand for now. This module never changes the active runtime.
static const sidetnfs_drive_config_t SIDETNFS_DEFAULT_DRIVE = {
    .state = SIDETNFS_DRIVE_SLOT_ENABLED,
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
static bool g_config_pending = false; // Fase 9E: see sidetnfs_config_is_pending()

// Fase 12A: set exactly once, by sidetnfs_config_init(), never touched
// anywhere else -- see sidetnfs_config_fallback_reason_t's own doc comment.
static bool g_config_loaded_from_flash = false;
static sidetnfs_config_fallback_reason_t g_config_fallback_reason = SIDETNFS_CONFIG_FALLBACK_NONE;
static sidetnfs_config_status_t g_config_fallback_structure_status = SIDETNFS_CONFIG_STATUS_OK;

// Fase 12B: set exactly once, by sidetnfs_config_init(), never touched
// anywhere else -- see sidetnfs_config_migrated_from_version()'s own doc
// comment. 0 means "no migration happened this boot".
static uint32_t g_config_migrated_from_version = 0;

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

// Fase 12B: counts CONFIGURED slots (DISABLED + ENABLED), not just
// ENABLED ones -- see sidetnfs_drive_flash_t.drive_count's own comment.
static void sidetnfs_config_recompute_drive_count(void)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (sidetnfs_drive_slot_is_configured(&g_config.drives[i]))
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
// calling this, including for a just-migrated block -- see
// sidetnfs_config_migrate_v2_to_v3()). Checks config_drive_letter,
// drive_count range, every slot's own `state` value, every
// DISABLED/ENABLED record's own fields, and full letter-uniqueness (every
// DISABLED/ENABLED record against every other DISABLED/ENABLED record and
// against config_drive_letter). Fase 12B: EMPTY slots are fully skipped --
// they never participate in duplicate-letter checks or drive_count.
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

    uint8_t configured_count = 0;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        const sidetnfs_drive_config_t *drive = &config->drives[i];

        if (drive->state > SIDETNFS_DRIVE_SLOT_ENABLED)
        {
            return SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE;
        }

        if (sidetnfs_drive_slot_is_empty(drive))
        {
            continue;
        }
        configured_count++;

        sidetnfs_config_status_t rc = sidetnfs_validate_drive_record(drive);
        if (rc != SIDETNFS_CONFIG_STATUS_OK)
        {
            return rc;
        }

        if (drive->drive_letter == config->config_drive_letter)
        {
            return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
        }

        for (uint8_t j = (uint8_t)(i + 1); j < SIDETNFS_MAX_DRIVES; j++)
        {
            if (sidetnfs_drive_slot_is_configured(&config->drives[j]) &&
                config->drives[j].drive_letter == drive->drive_letter)
            {
                return SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER;
            }
        }
    }

    if (configured_count != config->drive_count)
    {
        return SIDETNFS_CONFIG_STATUS_TOO_MANY_DRIVES;
    }

    return SIDETNFS_CONFIG_STATUS_OK;
}

// Fase 12B: byte-for-byte identical to sidetnfs_drive_config_t /
// sidetnfs_drive_flash_t -- the record layout never changed size or field
// offsets between flash format version 2 and 3, only the meaning/range of
// the first record field (used: 0/1 -> state: 0/1/2). These historical
// types exist purely so sidetnfs_config_migrate_v2_to_v3() below can read
// "this blob's first record byte means legacy `used`, not the new
// tri-state `state`" self-documentingly, without a second, genuinely
// divergent struct to maintain. Local to this file -- nothing else needs
// to know flash format version 2 ever existed.
typedef struct
{
    uint8_t used;
    uint8_t drive_letter;
    uint8_t type;
    uint8_t transport;
    uint16_t port;
    uint8_t reserved0[2];
    char nickname[SIDETNFS_NICKNAME_LEN];
    char host[SIDETNFS_HOST_LEN];
    char mount_path[SIDETNFS_MOUNTPATH_LEN];
    char sd_path[SIDETNFS_SDPATH_LEN];
} sidetnfs_drive_config_v2_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint8_t config_drive_letter;
    uint8_t drive_count;
    uint8_t reserved[2];
    sidetnfs_drive_config_v2_t drives[SIDETNFS_MAX_DRIVES];
    uint32_t crc32;
} sidetnfs_drive_flash_v2_t;

_Static_assert(sizeof(sidetnfs_drive_config_v2_t) == sizeof(sidetnfs_drive_config_t),
               "sidetnfs_drive_config_v2_t must stay byte-identical to sidetnfs_drive_config_t for the migration path to be safe");
_Static_assert(sizeof(sidetnfs_drive_flash_v2_t) == sizeof(sidetnfs_drive_flash_t),
               "sidetnfs_drive_flash_v2_t must stay byte-identical to sidetnfs_drive_flash_t for the migration path to be safe");
_Static_assert(offsetof(sidetnfs_drive_flash_v2_t, crc32) == offsetof(sidetnfs_drive_flash_t, crc32),
               "sidetnfs_drive_flash_v2_t.crc32 must be at the same byte offset as sidetnfs_drive_flash_t.crc32");

#define SIDETNFS_CONFIG_FLASH_VERSION_V2 2u

// Fase 12B: migrates a magic-checked version-2 block to the current
// (version 3) layout, entirely in RAM. Returns false (*new_config left
// untouched) if the v2 block's own CRC -- computed over its own bytes
// using the v2 layout, which is byte-identical to v3's, so this is
// exactly the same computation sidetnfs_config_init() already does for a
// native v3 block -- does not match; the caller treats that exactly like
// any other CRC_MISMATCH, never attempting to migrate a corrupt block.
// On success: every field is preserved except used==0 -> state
// SIDETNFS_DRIVE_SLOT_EMPTY (record otherwise left zeroed), used!=0 ->
// state SIDETNFS_DRIVE_SLOT_ENABLED (matching the only behavior a
// version-2 config could ever have -- every used record was always
// live). magic stays SIDETNFS_CONFIG_MAGIC, version becomes
// SIDETNFS_CONFIG_FLASH_VERSION, drive_count is recomputed (DISABLED +
// ENABLED, but a fresh migration can only ever produce ENABLED records),
// crc32 is recomputed over the new layout. The result still goes through
// sidetnfs_config_validate_structure() same as any other candidate --
// this function only remaps, it does not itself decide validity beyond
// the CRC check above. Never writes to flash, same rule as every other
// function in this file except sidetnfs_config_save().
static bool sidetnfs_config_migrate_v2_to_v3(const sidetnfs_drive_flash_v2_t *old_config, sidetnfs_drive_flash_t *new_config)
{
    uint32_t computed_crc = sidetnfs_crc32((const uint8_t *)old_config, offsetof(sidetnfs_drive_flash_v2_t, crc32));
    if (computed_crc != old_config->crc32)
    {
        return false;
    }

    memset(new_config, 0, sizeof(*new_config));
    new_config->magic = SIDETNFS_CONFIG_MAGIC;
    new_config->version = SIDETNFS_CONFIG_FLASH_VERSION;
    new_config->config_drive_letter = old_config->config_drive_letter;

    uint8_t configured_count = 0;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        const sidetnfs_drive_config_v2_t *old_drive = &old_config->drives[i];
        sidetnfs_drive_config_t *new_drive = &new_config->drives[i];

        if (!old_drive->used)
        {
            continue; // already zeroed above -- state stays SIDETNFS_DRIVE_SLOT_EMPTY
        }

        new_drive->state = SIDETNFS_DRIVE_SLOT_ENABLED;
        new_drive->drive_letter = old_drive->drive_letter;
        new_drive->type = old_drive->type;
        new_drive->transport = old_drive->transport;
        new_drive->port = old_drive->port;
        memcpy(new_drive->nickname, old_drive->nickname, sizeof(new_drive->nickname));
        memcpy(new_drive->host, old_drive->host, sizeof(new_drive->host));
        memcpy(new_drive->mount_path, old_drive->mount_path, sizeof(new_drive->mount_path));
        memcpy(new_drive->sd_path, old_drive->sd_path, sizeof(new_drive->sd_path));
        configured_count++;
    }
    new_config->drive_count = configured_count;

    sidetnfs_config_force_nul_termination(new_config);
    new_config->crc32 = sidetnfs_crc32((const uint8_t *)new_config, offsetof(sidetnfs_drive_flash_t, crc32));
    return true;
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

    // Fase 12A: each check now records WHICH reason a fallback would be
    // for, before any of them are actually acted on -- see
    // sidetnfs_config_fallback_reason_t's own doc comment. The pass/fail
    // outcome and ordering are unchanged from before this phase; only the
    // bookkeeping is new.
    sidetnfs_config_fallback_reason_t reason = SIDETNFS_CONFIG_FALLBACK_NONE;
    sidetnfs_config_status_t structure_status = SIDETNFS_CONFIG_STATUS_OK;
    uint32_t migrated_from = 0;

    bool valid = (candidate.magic == SIDETNFS_CONFIG_MAGIC);
    if (!valid)
    {
        reason = SIDETNFS_CONFIG_FALLBACK_BAD_MAGIC;
    }

    if (valid && candidate.version == SIDETNFS_CONFIG_FLASH_VERSION_V2)
    {
        // Fase 12B: migrate in RAM before any further validation --
        // sidetnfs_config_migrate_v2_to_v3() checks the v2 block's own CRC
        // internally and refuses to migrate a corrupt block. The raw
        // flash bytes read into `candidate` above are byte-identical to
        // sidetnfs_drive_flash_v2_t (see that type's own comment), so
        // reinterpreting the same bytes through the v2 type here is safe.
        static sidetnfs_drive_flash_t migrated;
        const sidetnfs_drive_flash_v2_t *candidate_as_v2 = (const sidetnfs_drive_flash_v2_t *)&candidate;
        if (sidetnfs_config_migrate_v2_to_v3(candidate_as_v2, &migrated))
        {
            candidate = migrated;
            migrated_from = SIDETNFS_CONFIG_FLASH_VERSION_V2;
        }
        else
        {
            valid = false;
            reason = SIDETNFS_CONFIG_FALLBACK_CRC_MISMATCH;
        }
    }
    else if (valid && candidate.version != SIDETNFS_CONFIG_FLASH_VERSION)
    {
        // Fase 12A/12B: a genuinely unsupported format version -- neither
        // the current layout nor the one older version this firmware
        // knows how to migrate from. Never a firmware/UF2 build hash
        // check (see report). A future format revision should extend the
        // migration chain above rather than widen this check.
        valid = false;
        reason = SIDETNFS_CONFIG_FALLBACK_UNSUPPORTED_VERSION;
    }

    if (valid)
    {
        // Fase 12B: for a migrated block this re-verifies the CRC
        // sidetnfs_config_migrate_v2_to_v3() itself just computed over the
        // new (v3) layout -- always true for a correctly implemented
        // migration, kept as the same defensive, uniform check every
        // candidate (native v3 or migrated) goes through.
        uint32_t computed_crc = sidetnfs_crc32((const uint8_t *)&candidate, offsetof(sidetnfs_drive_flash_t, crc32));
        valid = (computed_crc == candidate.crc32);
        if (!valid)
        {
            reason = SIDETNFS_CONFIG_FALLBACK_CRC_MISMATCH;
        }
    }

    if (valid)
    {
        structure_status = sidetnfs_config_validate_structure(&candidate);
        valid = (structure_status == SIDETNFS_CONFIG_STATUS_OK);
        if (!valid)
        {
            reason = SIDETNFS_CONFIG_FALLBACK_STRUCTURE_INVALID;
        }
    }

    if (!valid)
    {
        // Never trust a block that failed any check, even partially --
        // fall back to the full built-in default rather than salvaging
        // individual records out of it. This also transparently covers
        // pre-Fase-9C flash (old "STNF" magic/version 1): it simply fails
        // the magic check above and falls through to defaults here. A v2
        // block that failed migration (bad CRC) or whose migrated result
        // failed structure validation falls back the exact same way --
        // never a partial/salvaged migration.
        sidetnfs_config_load_defaults();
        g_config_ready = true;
        g_config_loaded_from_flash = false;
        g_config_fallback_reason = reason;
        g_config_fallback_structure_status = structure_status;
        g_config_migrated_from_version = 0;
        return;
    }

    g_config = candidate;
    g_config_loaded_from_flash = true;
    g_config_fallback_reason = SIDETNFS_CONFIG_FALLBACK_NONE;
    g_config_fallback_structure_status = SIDETNFS_CONFIG_STATUS_OK;
    g_config_migrated_from_version = migrated_from;
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

uint8_t sidetnfs_config_get_enabled_drive_count(void)
{
    if (!g_config_ready)
    {
        return 0;
    }
    uint8_t count = 0;
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (sidetnfs_drive_slot_is_enabled(&g_config.drives[i]))
        {
            count++;
        }
    }
    return count;
}

uint8_t sidetnfs_config_get_config_drive_letter(void)
{
    return g_config_ready ? g_config.config_drive_letter : 0;
}

uint32_t sidetnfs_config_migrated_from_version(void)
{
    return g_config_migrated_from_version;
}

sidetnfs_config_status_t sidetnfs_config_get_drive(uint8_t index, sidetnfs_drive_config_t *out)
{
    memset(out, 0, sizeof(*out));

    if (index >= SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_INDEX;
    }

    // Fase 12B: EMPTY is a normal, valid record state, not an error --
    // *out is already fully zeroed (state == SIDETNFS_DRIVE_SLOT_EMPTY ==
    // 0) by the memset above, matching an EMPTY slot's actual content
    // exactly, whether or not g_config_ready yet.
    if (!g_config_ready)
    {
        return SIDETNFS_CONFIG_STATUS_OK;
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

    if (in->state == SIDETNFS_DRIVE_SLOT_EMPTY)
    {
        // Clearing via SET_DRIVE(state=EMPTY) is equivalent to
        // DELETE_DRIVE -- no type-specific field validation applies to a
        // slot being emptied, and every other byte is wiped, not just the
        // state (Fase 12B: a slot going EMPTY must never leave stale
        // nickname/host/mount_path/sd_path behind).
        memset(&g_config.drives[index], 0, sizeof(g_config.drives[index]));
        sidetnfs_config_recompute_drive_count();
        return SIDETNFS_CONFIG_STATUS_OK;
    }

    if (in->state != SIDETNFS_DRIVE_SLOT_DISABLED && in->state != SIDETNFS_DRIVE_SLOT_ENABLED)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE;
    }

    sidetnfs_drive_config_t candidate = *in;
    candidate.reserved0[0] = 0;
    candidate.reserved0[1] = 0;
    candidate.nickname[SIDETNFS_NICKNAME_LEN - 1] = '\0';
    candidate.host[SIDETNFS_HOST_LEN - 1] = '\0';
    candidate.mount_path[SIDETNFS_MOUNTPATH_LEN - 1] = '\0';
    candidate.sd_path[SIDETNFS_SDPATH_LEN - 1] = '\0';

    // Fase 12B: DISABLED requires exactly the same fully-valid record as
    // ENABLED (see report, section 8) -- a disabled drive must be able to
    // become enabled again later without re-entering any data, and two
    // stored configs sharing a letter are unwanted even if one of them is
    // currently disabled.
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
        if (sidetnfs_drive_slot_is_configured(&g_config.drives[i]) && g_config.drives[i].drive_letter == candidate.drive_letter)
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

    g_config.drives[index] = candidate; // candidate.state is DISABLED or ENABLED, exactly as the caller asked
    sidetnfs_config_recompute_drive_count();
    return SIDETNFS_CONFIG_STATUS_OK;
}

// Fase 12B: state-only mutation, delegating entirely to
// sidetnfs_config_set_drive() above so validation never diverges between
// the two entry points. See sidetnfs_config.h's own doc comment for the
// exact per-target-state contract.
sidetnfs_config_status_t sidetnfs_config_set_drive_state(uint8_t index, sidetnfs_drive_slot_state_t new_state)
{
    if (index >= SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_INDEX;
    }

    if (new_state == SIDETNFS_DRIVE_SLOT_EMPTY)
    {
        sidetnfs_drive_config_t empty;
        memset(&empty, 0, sizeof(empty));
        return sidetnfs_config_set_drive(index, &empty);
    }

    if (new_state != SIDETNFS_DRIVE_SLOT_DISABLED && new_state != SIDETNFS_DRIVE_SLOT_ENABLED)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_STATE;
    }

    if (!sidetnfs_drive_slot_is_configured(&g_config.drives[index]))
    {
        // EMPTY -> ENABLED/DISABLED without a full record is rejected --
        // there is nothing stored to preserve, and inventing default
        // field values here would silently duplicate
        // sidetnfs_config_set_drive()'s own validation contract. Callers
        // that want to configure a currently-EMPTY slot must use
        // sidetnfs_config_set_drive() with a complete record instead.
        return SIDETNFS_CONFIG_STATUS_EMPTY_SLOT;
    }

    sidetnfs_drive_config_t candidate = g_config.drives[index];
    candidate.state = (uint8_t)new_state;
    return sidetnfs_config_set_drive(index, &candidate);
}

sidetnfs_config_status_t sidetnfs_config_delete_drive(uint8_t index)
{
    if (index >= SIDETNFS_MAX_DRIVES)
    {
        return SIDETNFS_CONFIG_STATUS_INVALID_INDEX;
    }

    if (sidetnfs_drive_slot_is_empty(&g_config.drives[index]))
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

    // Fase 12B: a DISABLED slot still reserves its letter against the
    // settings drive -- see report ("conflict met SETTINGS-driveletter").
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        if (sidetnfs_drive_slot_is_configured(&g_config.drives[i]) && g_config.drives[i].drive_letter == new_letter)
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
    // Recompute drive_count (DISABLED + ENABLED) right before validating,
    // so "drive_count equals the number of configured records" is always
    // true for whatever gets persisted, regardless of how it got here.
    sidetnfs_config_recompute_drive_count();

    sidetnfs_config_status_t validate_result = sidetnfs_config_validate_structure(&g_config);
    if (validate_result != SIDETNFS_CONFIG_STATUS_OK)
    {
        return validate_result;
    }

    // Build a clean image: reserved bytes zeroed, EMPTY records fully
    // zeroed, DISABLED/ENABLED records preserved with their real state,
    // type-irrelevant fields zeroed, strings NUL-terminated.
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
        if (sidetnfs_drive_slot_is_empty(&g_config.drives[i]))
        {
            continue; // already zeroed by the memset above
        }
        // Fase 12B: preserve whichever of DISABLED/ENABLED the slot
        // actually is -- clean.drives[i] = g_config.drives[i] already
        // copied the real .state, never force it to a fixed value.
        clean.drives[i] = g_config.drives[i];
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
    // Fase 9E: flash now holds the new config, but the active TNFS
    // session/drive letter do not change here -- only the proven
    // Atari-reset boundary (sidetnfs_probe_reinit_active_server(), driven
    // from GEMDRVEMUL_PING in gemdrvemul.c) may clear this flag, once it
    // has actually adopted the new config.
    g_config_pending = true;
    return SIDETNFS_CONFIG_STATUS_OK;
}

bool sidetnfs_config_is_pending(void)
{
    return g_config_pending;
}

void sidetnfs_config_clear_pending(void)
{
    g_config_pending = false;
}

bool sidetnfs_config_loaded_from_flash(void)
{
    return g_config_loaded_from_flash;
}

sidetnfs_config_fallback_reason_t sidetnfs_config_get_fallback_reason(void)
{
    return g_config_fallback_reason;
}

sidetnfs_config_status_t sidetnfs_config_get_fallback_structure_status(void)
{
    return g_config_fallback_structure_status;
}

// Fase 12B2: reuses sidetnfs_config_load_defaults() (same factory image
// sidetnfs_config_init() falls back to) and the existing, already-safe
// sidetnfs_config_save() -- no separate flash erase/program logic here.
// See sidetnfs_config.h's own doc comment for the full contract.
sidetnfs_config_status_t sidetnfs_config_factory_reset(void)
{
    sidetnfs_config_load_defaults();
    return sidetnfs_config_save();
}

void sidetnfs_config_force_factory_ram(void)
{
    sidetnfs_config_load_defaults();
    g_config_ready = true;
    // Deliberately NOT touching g_config_loaded_from_flash/
    // g_config_fallback_reason/g_config_migrated_from_version here --
    // sidetnfs_config_dump_uart() (called before this, in main.c) already
    // reported the REAL flash-derived values for this boot; overwriting
    // them here would erase that diagnostic evidence for no benefit.
}

bool sidetnfs_config_flash_matches_factory_defaults(void)
{
    const sidetnfs_drive_flash_t *flash_ptr =
        (const sidetnfs_drive_flash_t *)(XIP_BASE + SIDETNFS_CONFIG_FLASH_OFFSET);
    static sidetnfs_drive_flash_t raw;
    memcpy(&raw, flash_ptr, sizeof(raw));

    if (raw.magic != SIDETNFS_CONFIG_MAGIC || raw.version != SIDETNFS_CONFIG_FLASH_VERSION)
    {
        return false;
    }
    uint32_t computed_crc = sidetnfs_crc32((const uint8_t *)&raw, offsetof(sidetnfs_drive_flash_t, crc32));
    if (computed_crc != raw.crc32)
    {
        return false;
    }

    // Build the exact factory image sidetnfs_config_load_defaults()
    // itself would produce, without touching the live g_config -- same
    // constants (SIDETNFS_DEFAULT_DRIVE/SIDETNFS_DEFAULT_CONFIG_DRIVE_LETTER),
    // no migration path involved at all.
    static sidetnfs_drive_flash_t expected;
    memset(&expected, 0, sizeof(expected));
    expected.magic = SIDETNFS_CONFIG_MAGIC;
    expected.version = SIDETNFS_CONFIG_FLASH_VERSION;
    expected.config_drive_letter = SIDETNFS_DEFAULT_CONFIG_DRIVE_LETTER;
    expected.drive_count = 1;
    expected.drives[0] = SIDETNFS_DEFAULT_DRIVE;
    sidetnfs_config_force_nul_termination(&expected);

    // Compare everything up to (excluding) crc32 -- already proven
    // internally self-consistent above, no need to compare it separately.
    return memcmp(&raw, &expected, offsetof(sidetnfs_drive_flash_t, crc32)) == 0;
}

#if SIDETNFS_ENABLE_DIAG_UART
static const char *sidetnfs_diag_state_name(uint8_t state)
{
    switch (state)
    {
    case SIDETNFS_DRIVE_SLOT_EMPTY:
        return "EMPTY";
    case SIDETNFS_DRIVE_SLOT_DISABLED:
        return "DISABLED";
    case SIDETNFS_DRIVE_SLOT_ENABLED:
        return "ENABLED";
    default:
        return "INVALID";
    }
}

// Bounded, NUL-safe print of a fixed-size char field that may not
// actually be NUL-terminated in a corrupt/raw record -- never reads past
// `len` bytes regardless of content.
static void sidetnfs_diag_print_field(const char *label, const char *field, size_t len)
{
    char safe[65]; // SIDETNFS_HOST_LEN (64) is the longest field this is used for
    size_t n = (len < sizeof(safe) - 1) ? len : sizeof(safe) - 1;
    size_t i = 0;
    for (; i < n && field[i] != '\0'; i++)
    {
        safe[i] = (field[i] >= 0x20 && field[i] < 0x7f) ? field[i] : '.';
    }
    safe[i] = '\0';
    printf("%s=\"%s\"", label, safe);
}

void sidetnfs_config_dump_uart(void)
{
    const sidetnfs_drive_flash_t *flash_ptr =
        (const sidetnfs_drive_flash_t *)(XIP_BASE + SIDETNFS_CONFIG_FLASH_OFFSET);
    // Local copy -- same reasoning as sidetnfs_config_init()'s own
    // `candidate`: read-only snapshot, never mutates g_config or flash.
    static sidetnfs_drive_flash_t raw;
    memcpy(&raw, flash_ptr, sizeof(raw));

    uint32_t computed_crc = sidetnfs_crc32((const uint8_t *)&raw, offsetof(sidetnfs_drive_flash_t, crc32));

    printf("\r\n===== SIDETNFS CONFIG DUMP (raw flash header) =====\r\n");
    printf("magic: stored=0x%08lx expected=0x%08lx %s\r\n",
           (unsigned long)raw.magic, (unsigned long)SIDETNFS_CONFIG_MAGIC,
           (raw.magic == SIDETNFS_CONFIG_MAGIC) ? "MATCH" : "MISMATCH");
    printf("version: stored=%lu current=%lu\r\n", (unsigned long)raw.version, (unsigned long)SIDETNFS_CONFIG_FLASH_VERSION);
    printf("crc32: stored=0x%08lx calculated=0x%08lx %s\r\n",
           (unsigned long)raw.crc32, (unsigned long)computed_crc,
           (raw.crc32 == computed_crc) ? "MATCH" : "MISMATCH");
    printf("structure size: sidetnfs_drive_flash_t=%u bytes\r\n", (unsigned)sizeof(raw));
    printf("drive_count (stored): %u\r\n", (unsigned)raw.drive_count);
    printf("settings drive letter (stored): %c (0x%02x)\r\n", (char)raw.config_drive_letter, (unsigned)raw.config_drive_letter);

    printf("\r\n----- Load result (this boot) -----\r\n");
    if (g_config_migrated_from_version != 0)
    {
        printf("loaded: MIGRATED from version %lu\r\n", (unsigned long)g_config_migrated_from_version);
    }
    else if (g_config_loaded_from_flash)
    {
        printf("loaded: NATIVE v%lu (no migration, no fallback)\r\n", (unsigned long)SIDETNFS_CONFIG_FLASH_VERSION);
    }
    else
    {
        printf("loaded: FACTORY FALLBACK, reason=%d structure_status=%d\r\n",
               (int)g_config_fallback_reason, (int)g_config_fallback_structure_status);
    }
    printf("active (interpreted) settings letter: %c\r\n", (char)sidetnfs_config_get_config_drive_letter());
    printf("active (interpreted) drive_count (configured=DISABLED+ENABLED): %u, enabled_count: %u\r\n",
           (unsigned)sidetnfs_config_get_drive_count(), (unsigned)sidetnfs_config_get_enabled_drive_count());

    printf("\r\n----- Raw drive records (as stored on flash) -----\r\n");
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        const sidetnfs_drive_config_t *r = &raw.drives[i];
        printf("slot %u: raw_state=%u letter=0x%02x type=%u transport=%u port=%u ",
               (unsigned)i, (unsigned)r->state, (unsigned)r->drive_letter, (unsigned)r->type,
               (unsigned)r->transport, (unsigned)r->port);
        sidetnfs_diag_print_field("nickname", r->nickname, SIDETNFS_NICKNAME_LEN);
        printf(" ");
        sidetnfs_diag_print_field("host", r->host, SIDETNFS_HOST_LEN);
        printf(" ");
        sidetnfs_diag_print_field("mount_path", r->mount_path, SIDETNFS_MOUNTPATH_LEN);
        printf(" ");
        sidetnfs_diag_print_field("sd_path", r->sd_path, SIDETNFS_SDPATH_LEN);
        printf("\r\n");
    }

    printf("\r\n----- Interpreted drive records (active config) -----\r\n");
    for (uint8_t i = 0; i < SIDETNFS_MAX_DRIVES; i++)
    {
        sidetnfs_drive_config_t d;
        sidetnfs_config_status_t status = sidetnfs_config_get_drive(i, &d);
        printf("slot %u: state=%s configured=%s enabled=%s letter=%c type=%u status=%d\r\n",
               (unsigned)i, sidetnfs_diag_state_name(d.state),
               sidetnfs_drive_slot_is_configured(&d) ? "yes" : "no",
               sidetnfs_drive_slot_is_enabled(&d) ? "yes" : "no",
               (char)(d.drive_letter ? d.drive_letter : '?'), (unsigned)d.type, (int)status);
    }
    printf("===== END CONFIG DUMP =====\r\n\r\n");
}
#endif // SIDETNFS_ENABLE_DIAG_UART
