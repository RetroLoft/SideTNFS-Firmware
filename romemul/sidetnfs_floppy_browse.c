/**
 * File: sidetnfs_floppy_browse.c
 * Description: see include/sidetnfs_floppy_browse.h for the full design
 * rationale. Single active browse session (s_browse below) -- FLOPPY.PRG
 * is one Atari-side client, same assumption the rest of this protocol
 * already makes.
 */
#include "include/sidetnfs_floppy_browse.h"
#include "include/sidetnfs_floppy_config.h"
#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_sd_service.h"
#include "include/memfunc.h"
#include "f_util.h" // ff.h (FRESULT/FATFS/FILINFO/DIR/AM_DIR), same include this codebase's own SD code uses

#include <string.h>
#include <stdio.h>

typedef struct
{
    bool open;
    uint8_t backend; // SIDETNFS_FLOPPY_BACKEND_TNFS / _SD
    uint8_t profile_index;
    int tnfs_slot; // SIDETNFS_PROBE_FLOPPY_SLOT_BASE (single shared slot), TNFS only; -1 for SD
    // No stored mount_path here: TNFS paths sent to OPENDIRX are the CWD
    // alone (MOUNT already scoped the slot's session root to mount_path
    // server-side) -- see sidetnfs_floppy_browse_open()'s own comment.
    char sd_root[SIDETNFS_FLOPPY_SDPATH_LEN];
    char cwd[FLOPPY_BROWSE_CWD_LEN];
    uint32_t generation;

    // In-progress GET_PAGE walk (TNFS only -- see
    // sidetnfs_floppy_browse_get_page()'s own top-of-function comment for
    // why this exists: a deep page must never be walked to completion
    // inside one blocking dispatch call, since that call runs on the same
    // core that must keep servicing the time-critical Atari bus. SD reads
    // are fast enough to always finish within a single call and never use
    // this).
    //
    // Step 3: one combined page walk has two internal phases (dirs, then
    // files) -- walk_files_phase says which is currently active.
    // walk_matched is PHASE-LOCAL (reset to 0 both when a fresh walk
    // starts and again at the dirs->files phase transition);
    // walk_collected is the ONE combined slot counter shared across both
    // phases (never reset at the transition). walk_dirs_total is set
    // exactly once, at the moment phase dirs reaches the backend's real
    // end-of-directory, and is what phase files' own skip count is
    // computed from (see sidetnfs_floppy_browse_get_page()'s own
    // comment).
    bool walk_active;
    bool walk_files_phase;
    uint32_t walk_dirs_total;
    uint32_t walk_page_index;
    uint32_t walk_generation;
    uint32_t walk_matched;      // matching entries seen so far THIS PHASE
    uint16_t walk_collected;    // entries already written into the ROM3 page region, both phases combined
    uint8_t walk_tnfs_handle;   // open TNFS dir handle, only meaningful while walk_tnfs_handle_open
    bool walk_tnfs_handle_open;
} floppy_browse_state_t;

static floppy_browse_state_t s_browse = {0};
// 0 is never a valid generation -- lets a caller that has never opened a
// browse session use 0 to always miss the FLOPPY_BROWSE_ERR_STALE_GENERATION
// check safely (it can never equal a real session's generation).
static uint32_t s_next_generation = 1;

// ------------------------------------------------------------------
// Path safety
// ------------------------------------------------------------------

// Mirrors gemdrvemul.c's own normalize_gemdos_path() algorithm exactly
// (collapse "."/".." components via a token list rebuild, ".." at root is
// a no-op -- never above root, consecutive separators collapse for free
// via strtok_r) but sized for FLOPPY_BROWSE_CWD_LEN (256, the Step 2
// spec's own CWD size requirement) instead of gemdrvemul.c's
// MAX_FOLDER_LENGTH (128). Kept as an independent copy rather than an
// exported/reused function -- gemdrvemul.c's own version is deliberately
// GEMDOS-path-shaped/sized for GEMDOS's Dsetpath/Fsfirst callers, and this
// project's browser must not create a dependency in that direction (see
// this project's RESEARCH-STEP0.md). `in` may contain backslashes (never
// actually produced by this module itself -- TNFS/FatFS names never
// contain them -- normalized defensively anyway, same as the original).
static bool floppy_normalize_path(const char *in, char *out, size_t out_size)
{
    if (in == NULL || out == NULL || out_size < 2)
    {
        return false;
    }

    char work[FLOPPY_BROWSE_CWD_LEN];
    size_t in_len = strlen(in);
    if (in_len >= sizeof(work))
    {
        return false; // input itself already too long to even process
    }
    memcpy(work, in, in_len + 1);

    for (size_t i = 0; work[i] != '\0'; i++)
    {
        if (work[i] == '\\')
        {
            work[i] = '/';
        }
    }

    const char *components[FLOPPY_BROWSE_CWD_LEN / 2];
    size_t component_count = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(work, "/", &saveptr);
    while (tok != NULL)
    {
        if (strcmp(tok, ".") == 0)
        {
            // dropped
        }
        else if (strcmp(tok, "..") == 0)
        {
            if (component_count > 0)
            {
                component_count--;
            }
            // else: already at root -- no-op, never above root
        }
        else if (component_count < (sizeof(components) / sizeof(components[0])))
        {
            components[component_count++] = tok;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    char result[FLOPPY_BROWSE_CWD_LEN];
    size_t pos = 0;
    result[pos++] = '/';
    for (size_t i = 0; i < component_count; i++)
    {
        size_t clen = strlen(components[i]);
        size_t needed = clen + (i > 0 ? 1 : 0);
        if (pos + needed >= sizeof(result))
        {
            return false; // would not fit even in the local working buffer
        }
        if (i > 0)
        {
            result[pos++] = '/';
        }
        memcpy(result + pos, components[i], clen);
        pos += clen;
    }
    result[pos] = '\0';

    if (pos + 1 > out_size)
    {
        return false; // does not fit in the caller's buffer -- out left untouched
    }
    memcpy(out, result, pos + 1);
    return true;
}

// Builds "<root><normalized_cwd>" into out, collapsing a doubled
// separator at the join point (root and cwd are never required to agree
// on trailing/leading slash conventions). Returns false only if the
// result would not fit out_size -- callers map that to
// FLOPPY_BROWSE_ERR_PATH_TOO_LONG. Mirrors gemdrvemul.c's own
// sidetnfs_sd_build_fatfs_path()/get_tnfs_relative_pathname() contract:
// normalize first, then always concatenate strictly AFTER the profile's
// own root, never splice in the middle -- makes "above root" structurally
// unrepresentable without a separate escape/allow-list check.
static bool join_root_and_cwd(const char *root, const char *cwd, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s%s", root, cwd);
    if (n < 0 || (size_t)n >= out_size)
    {
        return false;
    }
    char *p;
    while ((p = strstr(out, "//")) != NULL)
    {
        memmove(p, p + 1, strlen(p));
    }
    return true;
}

// Closes out any in-progress GET_*_PAGE walk (TNFS handle included) --
// called whenever the active CWD/session is about to change (a fresh
// BROWSE_OPEN, or a successful CHANGE_DIR) so a walk that belonged to the
// OLD cwd/generation never lingers holding a TNFS directory handle open
// on the server. Harmless no-op if no walk is active.
static void abandon_walk(void)
{
    if (s_browse.walk_active && s_browse.backend == SIDETNFS_FLOPPY_BACKEND_TNFS && s_browse.walk_tnfs_handle_open)
    {
        sidetnfs_tnfs_raw_closedir(s_browse.tnfs_slot, s_browse.walk_tnfs_handle);
    }
    s_browse.walk_active = false;
    s_browse.walk_tnfs_handle_open = false;
}

// ------------------------------------------------------------------
// BROWSE_OPEN
// ------------------------------------------------------------------

sidetnfs_floppy_browse_status_t sidetnfs_floppy_browse_open(uint8_t profile_index, bool network_ok,
                                                              uint32_t *out_generation, char *out_cwd,
                                                              size_t out_cwd_size)
{
    if (out_generation == NULL || out_cwd == NULL || out_cwd_size < FLOPPY_BROWSE_CWD_LEN)
    {
        return FLOPPY_BROWSE_ERR_BACKEND_ERROR; // caller contract violation, not a real runtime path
    }
    *out_generation = 0;
    snprintf(out_cwd, out_cwd_size, "/");
    abandon_walk();

    if (profile_index >= SIDETNFS_FLOPPY_MAX_PROFILES)
    {
        return FLOPPY_BROWSE_ERR_INVALID_PROFILE;
    }
    sidetnfs_floppy_profile_config_t profile;
    sidetnfs_floppy_config_status_t cfg_status = sidetnfs_floppy_config_get_profile(profile_index, &profile);
    if (cfg_status != SIDETNFS_FLOPPY_STATUS_OK || !sidetnfs_floppy_profile_is_configured(&profile))
    {
        return FLOPPY_BROWSE_ERR_INVALID_PROFILE;
    }

    char requested_cwd[FLOPPY_BROWSE_CWD_LEN];
    if (profile.last_directory[0] == '\0' ||
        !floppy_normalize_path(profile.last_directory, requested_cwd, sizeof(requested_cwd)))
    {
        snprintf(requested_cwd, sizeof(requested_cwd), "/");
    }

    if (profile.backend == SIDETNFS_FLOPPY_BACKEND_TNFS)
    {
        if (profile.fields.tnfs.host[0] == '\0')
        {
            return FLOPPY_BROWSE_ERR_SOURCE_NOT_CONFIGURED;
        }

        int slot = SIDETNFS_PROBE_FLOPPY_SLOT_BASE; // single shared slot -- see sidetnfs_probe.h's own comment

        // Only re-populate (which resets any live session -- see
        // sidetnfs_probe_set_slot_context()'s own contract) when the
        // profile's own connection fields actually differ from whatever
        // this shared slot currently holds -- repeated BROWSE_OPEN calls
        // on the SAME already-connected profile must not pay a fresh
        // ~200ms MOUNT round trip every time. Switching to a DIFFERENT
        // TNFS profile always re-populates (host/port/mount_path won't
        // match) and pays that cost once, which is the trade this shared
        // slot makes for a much smaller static RAM footprint than one
        // slot per profile (see sidetnfs_probe.h).
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
            cfg.drive_letter = 0; // never published as a GEMDOS drive -- outside g_runtime_drives[]/g_drive_number_table
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
                return FLOPPY_BROWSE_ERR_TNFS_NOT_CONNECTED;
            }
            return FLOPPY_BROWSE_ERR_TNFS_HOST_UNREACHABLE;
        }

        // Validate the requested CWD actually opens; fall back to root
        // once if it doesn't (a stale last_directory -- a subdirectory
        // deleted since the last browse -- must never leave BROWSE_OPEN
        // permanently stuck). NOTE: the path sent to OPENDIRX is the CWD
        // ALONE, never mount_path+CWD -- TNFS's own MOUNT command already
        // scopes this slot's session root to mount_path server-side (same
        // contract sidetnfs_probe.c's own send_opendirx_probe() documents:
        // "MOUNT already scoped the session to '/Atari.ST', so '/' is its
        // root"); re-prepending mount_path here would ask the server for
        // <mount_path>/<mount_path>/... one level too deep, which is
        // exactly the bug that made a real profile's BROWSE_OPEN return
        // DIR_NOT_FOUND against its own configured root.
        uint8_t handle;
        SidetnfsTnfsDirOpenResult open_result = sidetnfs_tnfs_raw_opendir(slot, requested_cwd, &handle);
        if (open_result != SIDETNFS_TNFS_DIR_OK && strcmp(requested_cwd, "/") != 0)
        {
            snprintf(requested_cwd, sizeof(requested_cwd), "/");
            open_result = sidetnfs_tnfs_raw_opendir(slot, requested_cwd, &handle);
        }
        if (open_result != SIDETNFS_TNFS_DIR_OK)
        {
            if (open_result == SIDETNFS_TNFS_DIR_NOT_FOUND) return FLOPPY_BROWSE_ERR_DIR_NOT_FOUND;
            if (open_result == SIDETNFS_TNFS_DIR_ACCESS_DENIED) return FLOPPY_BROWSE_ERR_ACCESS_DENIED;
            if (open_result == SIDETNFS_TNFS_DIR_PATH_TOO_LONG) return FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
            return FLOPPY_BROWSE_ERR_BACKEND_ERROR;
        }
        sidetnfs_tnfs_raw_closedir(slot, handle);

        s_browse.open = true;
        s_browse.backend = SIDETNFS_FLOPPY_BACKEND_TNFS;
        s_browse.profile_index = profile_index;
        s_browse.tnfs_slot = slot;
    }
    else if (profile.backend == SIDETNFS_FLOPPY_BACKEND_SD)
    {
        if (profile.fields.sd.sd_path[0] == '\0')
        {
            return FLOPPY_BROWSE_ERR_SOURCE_NOT_CONFIGURED;
        }
        if (!sidetnfs_sd_service_has_run() || sidetnfs_sd_global_status() != SIDETNFS_SD_STATUS_READY)
        {
            return FLOPPY_BROWSE_ERR_SD_NOT_PRESENT;
        }

        char sd_root[SIDETNFS_FLOPPY_SDPATH_LEN + 4];
        snprintf(sd_root, sizeof(sd_root), "0:%s", profile.fields.sd.sd_path);
        char fatfs_path[FLOPPY_BROWSE_CWD_LEN + SIDETNFS_FLOPPY_SDPATH_LEN + 4];
        if (!join_root_and_cwd(sd_root, requested_cwd, fatfs_path, sizeof(fatfs_path)))
        {
            return FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
        }
        FILINFO fno;
        FRESULT fr = f_stat(fatfs_path, &fno);
        if (!(fr == FR_OK && (fno.fattrib & AM_DIR)) && strcmp(requested_cwd, "/") != 0)
        {
            snprintf(requested_cwd, sizeof(requested_cwd), "/");
            if (!join_root_and_cwd(sd_root, requested_cwd, fatfs_path, sizeof(fatfs_path)))
            {
                return FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
            }
            fr = f_stat(fatfs_path, &fno);
        }
        if (!(fr == FR_OK && (fno.fattrib & AM_DIR)))
        {
            return FLOPPY_BROWSE_ERR_DIR_NOT_FOUND;
        }

        s_browse.open = true;
        s_browse.backend = SIDETNFS_FLOPPY_BACKEND_SD;
        s_browse.profile_index = profile_index;
        s_browse.tnfs_slot = -1;
        strncpy(s_browse.sd_root, profile.fields.sd.sd_path, sizeof(s_browse.sd_root) - 1);
        s_browse.sd_root[sizeof(s_browse.sd_root) - 1] = '\0';
    }
    else
    {
        // Invalid backend value on an otherwise-configured slot --
        // sidetnfs_floppy_config_set_profile() already rejects this at
        // write time, so this should be unreachable; defensive only.
        return FLOPPY_BROWSE_ERR_SOURCE_NOT_CONFIGURED;
    }

    strncpy(s_browse.cwd, requested_cwd, sizeof(s_browse.cwd) - 1);
    s_browse.cwd[sizeof(s_browse.cwd) - 1] = '\0';
    s_browse.generation = s_next_generation++;

    *out_generation = s_browse.generation;
    strncpy(out_cwd, s_browse.cwd, out_cwd_size - 1);
    out_cwd[out_cwd_size - 1] = '\0';
    return FLOPPY_BROWSE_OK;
}

// ------------------------------------------------------------------
// BROWSE_CHANGE_DIR
// ------------------------------------------------------------------

// Fills *out_generation/*out_cwd from the CURRENT (unchanged) browse
// session -- used on every early-return path below so a failed
// CHANGE_DIR always echoes back a valid, still-correct CWD/generation
// rather than leaving the caller's own copy stale.
static void echo_current_state(uint32_t *out_generation, char *out_cwd, size_t out_cwd_size)
{
    *out_generation = s_browse.generation;
    strncpy(out_cwd, s_browse.cwd, out_cwd_size - 1);
    out_cwd[out_cwd_size - 1] = '\0';
}

sidetnfs_floppy_browse_status_t sidetnfs_floppy_browse_change_dir(uint32_t generation, bool go_up, const char *name,
                                                                    uint32_t *out_generation, char *out_cwd,
                                                                    size_t out_cwd_size)
{
    if (out_generation == NULL || out_cwd == NULL || out_cwd_size < FLOPPY_BROWSE_CWD_LEN)
    {
        return FLOPPY_BROWSE_ERR_BACKEND_ERROR;
    }
    if (!s_browse.open)
    {
        *out_generation = 0;
        snprintf(out_cwd, out_cwd_size, "/");
        return FLOPPY_BROWSE_ERR_NOT_OPEN;
    }
    if (generation != s_browse.generation)
    {
        echo_current_state(out_generation, out_cwd, out_cwd_size);
        return FLOPPY_BROWSE_ERR_STALE_GENERATION;
    }

    char candidate_input[FLOPPY_BROWSE_CWD_LEN + FLOPPY_BROWSE_NAME_LEN + 2];
    if (go_up)
    {
        int n = snprintf(candidate_input, sizeof(candidate_input), "%s/..", s_browse.cwd);
        if (n < 0 || (size_t)n >= sizeof(candidate_input))
        {
            echo_current_state(out_generation, out_cwd, out_cwd_size);
            return FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
        }
    }
    else
    {
        if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
            strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        {
            // A page entry name is never expected to be empty, contain a
            // separator, or be "."/".." -- reject outright rather than
            // let it walk somewhere the caller didn't actually ask for.
            echo_current_state(out_generation, out_cwd, out_cwd_size);
            return FLOPPY_BROWSE_ERR_DIR_NOT_FOUND;
        }
        int n = snprintf(candidate_input, sizeof(candidate_input), "%s/%s", s_browse.cwd, name);
        if (n < 0 || (size_t)n >= sizeof(candidate_input))
        {
            echo_current_state(out_generation, out_cwd, out_cwd_size);
            return FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
        }
    }

    char candidate_cwd[FLOPPY_BROWSE_CWD_LEN];
    if (!floppy_normalize_path(candidate_input, candidate_cwd, sizeof(candidate_cwd)))
    {
        echo_current_state(out_generation, out_cwd, out_cwd_size);
        return FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
    }

    // Verify the candidate actually opens before ever committing to it --
    // a failed CHANGE_DIR must never change the active CWD.
    sidetnfs_floppy_browse_status_t verify_status;
    if (s_browse.backend == SIDETNFS_FLOPPY_BACKEND_TNFS)
    {
        // candidate_cwd alone, never mount_path+candidate_cwd -- see
        // sidetnfs_floppy_browse_open()'s own comment on why.
        uint8_t handle;
        SidetnfsTnfsDirOpenResult r = sidetnfs_tnfs_raw_opendir(s_browse.tnfs_slot, candidate_cwd, &handle);
        if (r == SIDETNFS_TNFS_DIR_OK)
        {
            sidetnfs_tnfs_raw_closedir(s_browse.tnfs_slot, handle);
            verify_status = FLOPPY_BROWSE_OK;
        }
        else if (r == SIDETNFS_TNFS_DIR_NOT_FOUND)
            verify_status = FLOPPY_BROWSE_ERR_DIR_NOT_FOUND;
        else if (r == SIDETNFS_TNFS_DIR_ACCESS_DENIED)
            verify_status = FLOPPY_BROWSE_ERR_ACCESS_DENIED;
        else if (r == SIDETNFS_TNFS_DIR_PATH_TOO_LONG)
            verify_status = FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
        else
            verify_status = FLOPPY_BROWSE_ERR_BACKEND_ERROR;
    }
    else
    {
        char sd_root[SIDETNFS_FLOPPY_SDPATH_LEN + 4];
        snprintf(sd_root, sizeof(sd_root), "0:%s", s_browse.sd_root);
        char fatfs_path[FLOPPY_BROWSE_CWD_LEN + SIDETNFS_FLOPPY_SDPATH_LEN + 4];
        if (!join_root_and_cwd(sd_root, candidate_cwd, fatfs_path, sizeof(fatfs_path)))
        {
            verify_status = FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
        }
        else
        {
            FILINFO fno;
            FRESULT fr = f_stat(fatfs_path, &fno);
            if (fr == FR_OK && (fno.fattrib & AM_DIR))
                verify_status = FLOPPY_BROWSE_OK;
            else if (fr == FR_NO_PATH || fr == FR_NO_FILE)
                verify_status = FLOPPY_BROWSE_ERR_DIR_NOT_FOUND;
            else if (fr == FR_DENIED)
                verify_status = FLOPPY_BROWSE_ERR_ACCESS_DENIED;
            else
                verify_status = FLOPPY_BROWSE_ERR_BACKEND_ERROR;
        }
    }

    if (verify_status != FLOPPY_BROWSE_OK)
    {
        echo_current_state(out_generation, out_cwd, out_cwd_size);
        return verify_status;
    }

    abandon_walk(); // any walk in progress belonged to the CWD we're leaving

    strncpy(s_browse.cwd, candidate_cwd, sizeof(s_browse.cwd) - 1);
    s_browse.cwd[sizeof(s_browse.cwd) - 1] = '\0';
    s_browse.generation = s_next_generation++;

    echo_current_state(out_generation, out_cwd, out_cwd_size);
    return FLOPPY_BROWSE_OK;
}

// ------------------------------------------------------------------
// GET_PAGE (Step 3: one combined dirs-then-files page)
// ------------------------------------------------------------------

// TNFS: small per-CALL round budget, NOT a total-walk budget -- see this
// function's own top-of-function comment. A single dispatch call (and
// therefore this function) must always return quickly regardless of how
// deep the requested page is or how large the directory is, because it
// runs on the same core that must keep answering the time-critical Atari
// bus. FLOPPY_BROWSE_TNFS_MAX_TOTAL_ROUNDS is the overall safety bound
// across every resumed call for ONE page fetch (generous -- a floppy-image
// collection is never expected to come close), guarding only against a
// pathological server that never reaches TNFS_EOF.
//
// SD: local FatFS reads have no unbounded network wait, so a page always
// finishes within a single call -- FLOPPY_BROWSE_SD_MAX_WALK_ROUNDS is
// cheap headroom against a runaway loop, not a deliberately-reached limit.
#define FLOPPY_BROWSE_TNFS_ROUNDS_PER_CALL 8
#define FLOPPY_BROWSE_TNFS_MAX_TOTAL_ROUNDS 100000
#define FLOPPY_BROWSE_SD_MAX_WALK_ROUNDS 20000

// Writes one entry name directly into the ROM3 shared-memory window --
// byte-copy + CHANGE_ENDIANESS_BLOCK16, the same Pico->Atari string-field
// convention every other command in this protocol already uses (see
// GEMDRVEMUL_FLOPPY_GET_PROFILE's own dispatch handler). No RAM page
// buffer is ever allocated for this -- see this project's own
// RAM-discipline history (sidetnfs_floppy_config.h's top-of-file note).
static void write_page_entry(uint32_t memory_shared_address, uint32_t entries_offset, uint16_t slot_index,
                              const char *name)
{
    uint32_t addr = entries_offset + (uint32_t)slot_index * FLOPPY_BROWSE_NAME_LEN;
    size_t len = strnlen(name, FLOPPY_BROWSE_NAME_LEN - 1);
    memset((void *)(memory_shared_address + addr), 0, FLOPPY_BROWSE_NAME_LEN);
    memcpy((void *)(memory_shared_address + addr), name, len);
    CHANGE_ENDIANESS_BLOCK16(memory_shared_address + addr, FLOPPY_BROWSE_NAME_LEN);
}

// Writes one entry's dir/file flag into the parallel is_dir[] word array --
// same WRITE_WORD convention every other plain-word field in this protocol
// already uses. Index-matched with write_page_entry()'s own slot_index.
static void write_is_dir_entry(uint32_t memory_shared_address, uint32_t is_dir_offset, uint16_t slot_index,
                                bool is_dir)
{
    uint32_t addr = is_dir_offset + (uint32_t)slot_index * 2;
    WRITE_WORD(memory_shared_address, addr, is_dir ? 1 : 0);
}

// Fetches ONE combined page of the active CWD's subdirectories followed
// by its files (Step 3 -- see this file's own header comment). Walked in
// two internal phases against a single combined page_index; see the
// walk_files_phase/walk_dirs_total fields' own comment on
// floppy_browse_state_t for the skip/collect math across the phase
// boundary.
//
// TNFS walks are INCREMENTAL and RESUMABLE: this function never performs
// more than FLOPPY_BROWSE_TNFS_ROUNDS_PER_CALL real TNFS round trips
// before returning, however deep the page or however large the directory
// -- a single call therefore always returns quickly (bounded by a handful
// of TNFS_FS_WAIT_MAX_ITER-bounded round trips, same order of magnitude as
// any other single command in this protocol), instead of the earlier
// design's up-to-thousands-of-rounds walk inside one blocking call, which
// would have tied up the same core that must keep servicing the
// time-critical Atari bus for however long that took.
//
// When more work remains, this returns FLOPPY_BROWSE_STATUS_IN_PROGRESS
// (not an error) and parks its progress in s_browse.walk_* (including
// which phase it's in and the still-open TNFS dir handle) -- the caller
// (the Atari-side client, see floppy_probe.c's polling wrapper) re-issues
// the IDENTICAL request (same generation/page_index) to resume exactly
// where this call left off, until a terminal status (OK/END_OF_DIRECTORY/
// an error) comes back. Matched/collected entries accumulate in s_browse
// across calls; entries already written into the ROM3 page region are
// never re-cleared on a resume (only on a genuinely NEW request -- see
// `resuming` below), so a partially-built page is never visible to the
// Atari as such: nothing publishes it (writes the response header +
// random token) until this function returns a terminal status.
//
// SD walks always finish within this one call (no unbounded network wait
// to chunk around), so `resuming` is always false for the SD backend in
// practice.
floppy_browse_page_result_t sidetnfs_floppy_browse_get_page(uint32_t generation, uint32_t page_index,
                                                              uint32_t memory_shared_address, uint32_t entries_offset,
                                                              uint32_t is_dir_offset)
{
    floppy_browse_page_result_t result;
    memset(&result, 0, sizeof(result));
    result.page_index = page_index;

    if (!s_browse.open)
    {
        result.status = FLOPPY_BROWSE_ERR_NOT_OPEN;
        return result;
    }
    if (generation != s_browse.generation)
    {
        result.status = FLOPPY_BROWSE_ERR_STALE_GENERATION;
        result.generation = s_browse.generation;
        return result;
    }
    result.generation = s_browse.generation;

    bool resuming =
        s_browse.walk_active && s_browse.walk_generation == generation && s_browse.walk_page_index == page_index;
    if (s_browse.walk_active && !resuming)
    {
        // A different request arrived while a walk was still parked
        // (e.g. the Atari gave up on an earlier poll sequence and asked
        // for something else) -- drop the stale one cleanly (closes any
        // open TNFS handle) before starting the new one.
        abandon_walk();
    }
    if (!resuming)
    {
        // Fresh walk: clear the ROM3 page region exactly once, up front
        // -- every subsequent resumed call for THIS walk only ever adds
        // to it, never re-clears it. Starts in phase dirs.
        memset((void *)(memory_shared_address + entries_offset), 0,
               (size_t)FLOPPY_BROWSE_PAGE_ENTRIES * FLOPPY_BROWSE_NAME_LEN);
        memset((void *)(memory_shared_address + is_dir_offset), 0, (size_t)FLOPPY_BROWSE_PAGE_ENTRIES * 2);
        s_browse.walk_active = true;
        s_browse.walk_page_index = page_index;
        s_browse.walk_generation = generation;
        s_browse.walk_files_phase = false;
        s_browse.walk_dirs_total = 0;
        s_browse.walk_matched = 0;
        s_browse.walk_collected = 0;
        s_browse.walk_tnfs_handle_open = false;
    }

    uint32_t combined_skip = page_index * (uint32_t)FLOPPY_BROWSE_PAGE_ENTRIES;
    bool has_next = false;
    bool backend_error = false;
    bool finished = false;

    if (s_browse.backend == SIDETNFS_FLOPPY_BACKEND_TNFS)
    {
        // One round budget spans BOTH phases within this call -- if phase
        // dirs reaches real EOF partway through the budget, phase files
        // starts immediately and consumes whatever budget remains, rather
        // than waiting for a whole extra resumed call just to begin.
        for (uint32_t round = 0; round < FLOPPY_BROWSE_TNFS_ROUNDS_PER_CALL; round++)
        {
            if (!s_browse.walk_tnfs_handle_open)
            {
                // s_browse.cwd alone, never mount_path+cwd -- see
                // sidetnfs_floppy_browse_open()'s own comment on why. Same
                // CWD for both phases -- phase files always starts a
                // FRESH enumeration from the top, filtered differently.
                uint8_t handle;
                SidetnfsTnfsDirOpenResult open_result =
                    sidetnfs_tnfs_raw_opendir(s_browse.tnfs_slot, s_browse.cwd, &handle);
                if (open_result != SIDETNFS_TNFS_DIR_OK)
                {
                    s_browse.walk_active = false;
                    result.status = (open_result == SIDETNFS_TNFS_DIR_NOT_FOUND) ? FLOPPY_BROWSE_ERR_DIR_NOT_FOUND
                                     : (open_result == SIDETNFS_TNFS_DIR_ACCESS_DENIED)
                                         ? FLOPPY_BROWSE_ERR_ACCESS_DENIED
                                     : (open_result == SIDETNFS_TNFS_DIR_PATH_TOO_LONG)
                                         ? FLOPPY_BROWSE_ERR_PATH_TOO_LONG
                                         : FLOPPY_BROWSE_ERR_BACKEND_ERROR;
                    return result;
                }
                s_browse.walk_tnfs_handle = handle;
                s_browse.walk_tnfs_handle_open = true;
            }

            if (s_browse.walk_matched > FLOPPY_BROWSE_TNFS_MAX_TOTAL_ROUNDS)
            {
                // Pathological case only (server never reaches TNFS_EOF)
                // -- give up rather than resume forever. Re-armed per
                // phase since walk_matched resets at the transition.
                backend_error = true;
                break;
            }

            SidetnfsTnfsRawEntry entry;
            int r = sidetnfs_tnfs_raw_readdir(s_browse.tnfs_slot, s_browse.walk_tnfs_handle, &entry);
            if (r < 0)
            {
                // "error 13" investigation: ndta carries walk_matched
                // (this phase's own skip/collect progress at the moment
                // of failure), index=outer round, count=walk_collected
                // (combined across phases), result=1 dirs phase/2 files
                // phase, attr=page_index (fits a byte for realistic UIs).
                // Paired with the SIDETNFS_DIAG_FLOPPY_RAW_READDIR_FAIL
                // event sidetnfs_tnfs_raw_readdir() itself just logged
                // (the actual reason this call returned -1).
                sidetnfs_diag_log(SIDETNFS_DIAG_FLOPPY_GET_PAGE_BACKEND_ERROR, s_browse.walk_matched, NULL, NULL,
                                   NULL, (uint16_t)round, s_browse.walk_collected,
                                   s_browse.walk_files_phase ? 2 : 1, (uint8_t)page_index);
                backend_error = true;
                break;
            }
            if (r == 0)
            {
                // Real end of directory for the CURRENT phase.
                sidetnfs_tnfs_raw_closedir(s_browse.tnfs_slot, s_browse.walk_tnfs_handle);
                s_browse.walk_tnfs_handle_open = false;
                if (s_browse.walk_files_phase)
                {
                    finished = true; // both phases done -- whole page complete
                    break;
                }
                // Phase dirs -> phase files: dirs_total is exactly how
                // many dir entries this phase ever counted (skipped or
                // collected), since it walked the CWD to its real end.
                s_browse.walk_dirs_total = s_browse.walk_matched;
                s_browse.walk_files_phase = true;
                s_browse.walk_matched = 0; // phase-local counter resets
                continue; // next round opens the fresh files-phase handle
            }
            bool want_dirs_now = !s_browse.walk_files_phase;
            if (entry.is_dir != want_dirs_now)
            {
                continue; // wrong kind for the current phase
            }
            uint32_t phase_skip = s_browse.walk_files_phase
                                       ? (combined_skip > s_browse.walk_dirs_total
                                              ? combined_skip - s_browse.walk_dirs_total
                                              : 0)
                                       : combined_skip;
            if (s_browse.walk_matched < phase_skip)
            {
                s_browse.walk_matched++;
                continue;
            }
            if (s_browse.walk_collected < FLOPPY_BROWSE_PAGE_ENTRIES)
            {
                write_page_entry(memory_shared_address, entries_offset, s_browse.walk_collected, entry.name);
                write_is_dir_entry(memory_shared_address, is_dir_offset, s_browse.walk_collected, want_dirs_now);
                s_browse.walk_collected++;
                s_browse.walk_matched++;
            }
            else
            {
                has_next = true;
                finished = true;
                break;
            }
        }

        if ((finished || backend_error) && s_browse.walk_tnfs_handle_open)
        {
            sidetnfs_tnfs_raw_closedir(s_browse.tnfs_slot, s_browse.walk_tnfs_handle);
            s_browse.walk_tnfs_handle_open = false;
        }
    }
    else
    {
        // SD: both phases always finish within this one call (no
        // unbounded network wait to chunk around).
        char sd_root[SIDETNFS_FLOPPY_SDPATH_LEN + 4];
        snprintf(sd_root, sizeof(sd_root), "0:%s", s_browse.sd_root);
        char fatfs_path[FLOPPY_BROWSE_CWD_LEN + SIDETNFS_FLOPPY_SDPATH_LEN + 4];
        if (!join_root_and_cwd(sd_root, s_browse.cwd, fatfs_path, sizeof(fatfs_path)))
        {
            s_browse.walk_active = false;
            result.status = FLOPPY_BROWSE_ERR_PATH_TOO_LONG;
            return result;
        }

        DIR dj;
        FILINFO fno;
        FRESULT fr = f_findfirst(&dj, &fno, fatfs_path, "*");
        if (fr != FR_OK)
        {
            s_browse.walk_active = false;
            result.status = (fr == FR_NO_PATH || fr == FR_NO_FILE) ? FLOPPY_BROWSE_ERR_DIR_NOT_FOUND
                             : (fr == FR_DENIED)                    ? FLOPPY_BROWSE_ERR_ACCESS_DENIED
                                                                     : FLOPPY_BROWSE_ERR_BACKEND_ERROR;
            return result;
        }

        // Phase dirs.
        uint32_t round;
        for (round = 0; round < FLOPPY_BROWSE_SD_MAX_WALK_ROUNDS && fr == FR_OK && fno.fname[0] != '\0'; round++)
        {
            bool is_dir = (fno.fattrib & AM_DIR) != 0;
            if (is_dir)
            {
                if (s_browse.walk_matched < combined_skip)
                {
                    s_browse.walk_matched++;
                }
                else if (s_browse.walk_collected < FLOPPY_BROWSE_PAGE_ENTRIES)
                {
                    if (strlen(fno.fname) < FLOPPY_BROWSE_NAME_LEN)
                    {
                        write_page_entry(memory_shared_address, entries_offset, s_browse.walk_collected, fno.fname);
                        write_is_dir_entry(memory_shared_address, is_dir_offset, s_browse.walk_collected, true);
                        s_browse.walk_collected++;
                    }
                    s_browse.walk_matched++;
                }
                else
                {
                    has_next = true;
                    break;
                }
            }
            fr = f_findnext(&dj, &fno);
        }
        f_closedir(&dj);
        if (round >= FLOPPY_BROWSE_SD_MAX_WALK_ROUNDS)
        {
            backend_error = true;
        }
        s_browse.walk_dirs_total = s_browse.walk_matched;

        // Phase files -- fresh enumeration of the same CWD, only if phase
        // dirs didn't already fill every slot.
        if (!backend_error && !has_next)
        {
            fr = f_findfirst(&dj, &fno, fatfs_path, "*");
            if (fr != FR_OK)
            {
                backend_error = true;
            }
            else
            {
                uint32_t file_skip =
                    combined_skip > s_browse.walk_dirs_total ? combined_skip - s_browse.walk_dirs_total : 0;
                uint32_t file_matched = 0;
                for (; round < FLOPPY_BROWSE_SD_MAX_WALK_ROUNDS && fr == FR_OK && fno.fname[0] != '\0'; round++)
                {
                    bool is_dir = (fno.fattrib & AM_DIR) != 0;
                    if (!is_dir)
                    {
                        if (file_matched < file_skip)
                        {
                            file_matched++;
                        }
                        else if (s_browse.walk_collected < FLOPPY_BROWSE_PAGE_ENTRIES)
                        {
                            if (strlen(fno.fname) < FLOPPY_BROWSE_NAME_LEN)
                            {
                                write_page_entry(memory_shared_address, entries_offset, s_browse.walk_collected,
                                                  fno.fname);
                                write_is_dir_entry(memory_shared_address, is_dir_offset, s_browse.walk_collected,
                                                    false);
                                s_browse.walk_collected++;
                            }
                            file_matched++;
                        }
                        else
                        {
                            has_next = true;
                            break;
                        }
                    }
                    fr = f_findnext(&dj, &fno);
                }
                f_closedir(&dj);
                if (round >= FLOPPY_BROWSE_SD_MAX_WALK_ROUNDS)
                {
                    backend_error = true;
                }
            }
        }
        finished = true; // SD always completes within one call
    }

    if (backend_error)
    {
        s_browse.walk_active = false;
        result.status = FLOPPY_BROWSE_ERR_BACKEND_ERROR;
        return result;
    }

    if (!finished)
    {
        // Budget for this call is exhausted but the walk isn't done --
        // state stays parked in s_browse (including which phase) for the
        // next resumed call.
        result.status = FLOPPY_BROWSE_STATUS_IN_PROGRESS;
        return result;
    }

    s_browse.walk_active = false;
    result.count = s_browse.walk_collected;
    result.has_next = has_next;
    result.has_prev = page_index > 0;
    result.status = (s_browse.walk_collected == 0 && !has_next) ? FLOPPY_BROWSE_STATUS_END_OF_DIRECTORY : FLOPPY_BROWSE_OK;
    return result;
}
