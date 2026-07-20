/**
 * File: test_dta_lifecycle.c
 * Fase 10B2: host test for the config-drive DTA lifecycle
 * (sidetnfs_config_drive_backend.c) -- Fsetdta/Fsfirst/DTA_EXIST/Fsnext/
 * DTA_RELEASE/close_all, exactly as gemdrvemul.c drives it.
 *
 * Compiles the REAL sidetnfs_config_drive_backend.c (via a symlink into
 * sandbox/, see that directory) against a stub filesys.h (the real one
 * pulls in Pico SDK/FatFs headers a host toolchain can't build) and
 * host-only definitions of:
 *   - the two Fase 10A flash arrays (fake, small content -- only the
 *     names "SIDETNFS.PRG"/"README.TXT" and the DTA lifecycle matter
 *     here, never file content)
 *   - sidetnfs_gemdos_pattern_match()/sidetnfs_gemdos_attr_match(), copied
 *     verbatim from sidetnfs_probe.c (that file itself is not
 *     host-compilable -- cyw43/lwIP/hardware deps throughout), so the
 *     wildcard/attribute semantics under test are the real ones, not a
 *     simplified stand-in.
 *
 * Not wired into build.sh/CMakeLists.txt (a pure host-side check, like the
 * ad hoc host models used in earlier phases -- see report) -- run directly,
 * e.g.:
 *   gcc -std=c11 -Wall -Wextra \
 *       -Isandbox/include -Isandbox \
 *       test_dta_lifecycle.c sandbox/sidetnfs_config_drive_backend.c \
 *       -o /tmp/test_dta_lifecycle && /tmp/test_dta_lifecycle
 */
#define _POSIX_C_SOURCE 200809L /* for strnlen(), used by the copied normalize_gemdos_pattern() */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/filesys.h" /* FS_ST_ARCH, used by sidetnfs_gemdos_attr_match() below */
#include "include/sidetnfs_config_drive_backend.h"

/* ---- Fase 10A flash arrays: fake host-side definitions -------------- */
/* Real firmware embeds the actual SIDETNFS.PRG/README.TXT bytes; only the
 * fixed names ("SIDETNFS.PRG", "README.TXT") and distinguishable sizes
 * matter for DTA lifecycle testing, never the byte content. */
const uint8_t sidetnfs_config_prg[] = "FAKE-PRG-CONTENT-FOR-HOST-TEST";
const uint32_t sidetnfs_config_prg_length = sizeof(sidetnfs_config_prg);

const uint8_t sidetnfs_config_readme[] = "FAKE-README-CONTENT";
const uint32_t sidetnfs_config_readme_length = sizeof(sidetnfs_config_readme);

/* ---- sidetnfs_gemdos_pattern_match()/sidetnfs_gemdos_attr_match() ---
 * Copied verbatim from romemul/sidetnfs_probe.c (normalize_gemdos_pattern,
 * wildcard_match_upper, and the two public functions) -- kept byte-for-byte
 * identical in logic so this test exercises the real GEMDOS wildcard/attr
 * semantics, not an approximation. */
static void normalize_gemdos_pattern(const char *pattern, char *out, size_t out_size)
{
    size_t len = strnlen(pattern, out_size - 1);
    memcpy(out, pattern, len);
    out[len] = '\0';
    if (len >= 2 && out[len - 1] == '*' && out[len - 2] == '.')
    {
        out[len - 2] = '\0';
    }
}

static bool wildcard_match_upper(const char *pat, const char *str)
{
    const char *s = str;
    const char *p = pat;
    const char *star_p = NULL;
    const char *star_s = NULL;

    while (*s != '\0')
    {
        if (*p == '?' || toupper((unsigned char)*p) == toupper((unsigned char)*s))
        {
            p++;
            s++;
        }
        else if (*p == '*')
        {
            star_p = p++;
            star_s = s;
        }
        else if (star_p != NULL)
        {
            p = star_p + 1;
            s = ++star_s;
        }
        else
        {
            return false;
        }
    }
    while (*p == '*')
    {
        p++;
    }
    return *p == '\0';
}

bool sidetnfs_gemdos_pattern_match(const char *name83, const char *pattern)
{
    if (name83 == NULL || pattern == NULL)
    {
        return false;
    }
    char norm_pattern[13];
    normalize_gemdos_pattern(pattern, norm_pattern, sizeof(norm_pattern));
    return wildcard_match_upper(norm_pattern, name83);
}

bool sidetnfs_gemdos_attr_match(uint8_t entry_attr, uint8_t search_attr)
{
    uint8_t effective_attr = entry_attr;
    if (entry_attr == 0)
    {
        effective_attr |= FS_ST_ARCH;
    }
    return (effective_attr & search_attr) != 0;
}

/* ---- tiny assert-based test harness ---------------------------------- */
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                     \
    do                                                                                                   \
    {                                                                                                    \
        g_checks++;                                                                                      \
        if (!(cond))                                                                                     \
        {                                                                                                 \
            g_failures++;                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
        }                                                                                                 \
    } while (0)

#define CHECK_STR_EQ(a, b) CHECK(strcmp((a), (b)) == 0)

/* ---- test cases -------------------------------------------------------- */

/* All searches below use FS_ST_ARCH as the search attribute, not 0: every
 * config-drive entry has attr == FS_ST_READONLY|FS_ST_ARCH (never 0), and
 * sidetnfs_gemdos_attr_match() is an OR-style test ((effective_attr &
 * search_attr) != 0) -- a search_attr of 0 can never match anything here.
 * FS_ST_ARCH is the bit sidetnfs_probe.c's own comment identifies as what a
 * real "normal-file" GEMDOS search actually carries in practice. */

/* FSETDTA(ndta); FSFIRST(ndta,"*.*"); DTA_EXIST->true; FSNEXT->README.TXT;
 * DTA_EXIST->true; FSNEXT->ENMFIL; DTA_EXIST->false; DTA_RELEASE(ndta). */
static void test_full_protocol_sequence(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta = 0x1000;

    /* FSETDTA itself never touches the config-drive registry (see
     * gemdrvemul.c's GEMDRVEMUL_FSETDTA_CALL under SIDETNFS_CONFIG_DRIVE_ONLY)
     * -- nothing to call here, but the registry must still be empty. */
    CHECK(!sidetnfs_config_drive_search_is_active(ndta));

    SidetnfsAtariDirEntry entry;
    SidetnfsDirSearchResult r = sidetnfs_config_drive_search_start(ndta, "*.*", FS_ST_ARCH, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "SIDETNFS.PRG");

    CHECK(sidetnfs_config_drive_search_is_active(ndta));

    r = sidetnfs_config_drive_search_next(ndta, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "README.TXT");

    CHECK(sidetnfs_config_drive_search_is_active(ndta));

    r = sidetnfs_config_drive_search_next(ndta, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_NOT_FOUND); /* -> GEMDOS_ENMFIL at the gemdrvemul.c level */

    CHECK(!sidetnfs_config_drive_search_is_active(ndta));

    sidetnfs_config_drive_search_close(ndta); /* DTA_RELEASE */
    CHECK(!sidetnfs_config_drive_search_is_active(ndta));
}

static void test_pattern_prg_only(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta = 0x2000;
    SidetnfsAtariDirEntry entry;

    SidetnfsDirSearchResult r = sidetnfs_config_drive_search_start(ndta, "*.PRG", FS_ST_ARCH, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "SIDETNFS.PRG");

    r = sidetnfs_config_drive_search_next(ndta, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_NOT_FOUND); /* README.TXT must not match *.PRG */
}

static void test_pattern_txt_only(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta = 0x2001;
    SidetnfsAtariDirEntry entry;

    SidetnfsDirSearchResult r = sidetnfs_config_drive_search_start(ndta, "*.TXT", FS_ST_ARCH, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "README.TXT");

    r = sidetnfs_config_drive_search_next(ndta, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_NOT_FOUND); /* SIDETNFS.PRG must not match *.TXT */
}

static void test_exact_name_case_insensitive(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta = 0x2002;
    SidetnfsAtariDirEntry entry;

    /* Exact lowercase name -- GEMDOS wildcard matching is case-insensitive. */
    SidetnfsDirSearchResult r = sidetnfs_config_drive_search_start(ndta, "sidetnfs.prg", FS_ST_ARCH, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "SIDETNFS.PRG");
    r = sidetnfs_config_drive_search_next(ndta, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_NOT_FOUND);

    sidetnfs_config_drive_search_close_all();
    r = sidetnfs_config_drive_search_start(ndta, "ReadMe.Txt", FS_ST_ARCH, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "README.TXT");

    /* Also lookup() (used by Fopen), same case-insensitivity. */
    const uint8_t *data = NULL;
    uint32_t size = 0;
    CHECK(sidetnfs_config_drive_lookup("readme.txt", &data, &size));
    CHECK(size == sidetnfs_config_readme_length);
    CHECK(sidetnfs_config_drive_lookup("SIDETNFS.PRG", &data, &size));
    CHECK(size == sidetnfs_config_prg_length);
    CHECK(!sidetnfs_config_drive_lookup("NOPE.XXX", &data, &size));
}

static void test_two_concurrent_dta_addresses(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta_a = 0x3000;
    const uint32_t ndta_b = 0x3004;
    SidetnfsAtariDirEntry entry_a, entry_b;

    CHECK(sidetnfs_config_drive_search_start(ndta_a, "*.*", FS_ST_ARCH, &entry_a) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK(sidetnfs_config_drive_search_start(ndta_b, "*.*", FS_ST_ARCH, &entry_b) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry_a.name, "SIDETNFS.PRG");
    CHECK_STR_EQ(entry_b.name, "SIDETNFS.PRG");

    /* Advance only ndta_a -- ndta_b's independent cursor must be unaffected. */
    CHECK(sidetnfs_config_drive_search_next(ndta_a, &entry_a) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry_a.name, "README.TXT");

    CHECK(sidetnfs_config_drive_search_is_active(ndta_a));
    CHECK(sidetnfs_config_drive_search_is_active(ndta_b));

    CHECK(sidetnfs_config_drive_search_next(ndta_b, &entry_b) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry_b.name, "README.TXT");

    CHECK(sidetnfs_config_drive_search_count_active() == 2);
}

static void test_dta_release_during_active_search(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta = 0x4000;
    SidetnfsAtariDirEntry entry;

    CHECK(sidetnfs_config_drive_search_start(ndta, "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK(sidetnfs_config_drive_search_is_active(ndta));
    uint32_t count_before = sidetnfs_config_drive_search_count_active();

    sidetnfs_config_drive_search_close(ndta); /* DTA_RELEASE mid-search */

    CHECK(!sidetnfs_config_drive_search_is_active(ndta));
    CHECK(sidetnfs_config_drive_search_count_active() == count_before - 1);
}

static void test_close_all_with_active_searches(void)
{
    sidetnfs_config_drive_search_close_all();
    SidetnfsAtariDirEntry entry;
    const uint32_t ndtas[3] = {0x5000, 0x5004, 0x5008};

    for (int i = 0; i < 3; i++)
    {
        CHECK(sidetnfs_config_drive_search_start(ndtas[i], "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND);
    }
    CHECK(sidetnfs_config_drive_search_count_active() == 3);

    sidetnfs_config_drive_search_close_all();

    CHECK(sidetnfs_config_drive_search_count_active() == 0);
    for (int i = 0; i < 3; i++)
    {
        CHECK(!sidetnfs_config_drive_search_is_active(ndtas[i]));
    }
}

static void test_repeated_fsfirst_same_dta(void)
{
    sidetnfs_config_drive_search_close_all();
    const uint32_t ndta = 0x6000;
    SidetnfsAtariDirEntry entry;

    CHECK(sidetnfs_config_drive_search_start(ndta, "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK(sidetnfs_config_drive_search_next(ndta, &entry) == SIDETNFS_DIR_SEARCH_FOUND); /* now mid-sequence */
    CHECK(sidetnfs_config_drive_search_count_active() == 1);

    /* A repeated Fsfirst for the same ndta must start completely fresh
     * (not accumulate a second slot, not continue the old cursor). */
    CHECK(sidetnfs_config_drive_search_start(ndta, "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "SIDETNFS.PRG");
    CHECK(sidetnfs_config_drive_search_count_active() == 1); /* still exactly one slot for this ndta */
}

static void test_slot_exhaustion_does_not_corrupt_other_dta(void)
{
    sidetnfs_config_drive_search_close_all();
    SidetnfsAtariDirEntry entry;
    /* SIDETNFS_CONFIG_DRIVE_SEARCH_SLOTS == 4 (see sidetnfs_config_drive_backend.c). */
    const uint32_t ndtas[4] = {0x7000, 0x7004, 0x7008, 0x700C};
    for (int i = 0; i < 4; i++)
    {
        CHECK(sidetnfs_config_drive_search_start(ndtas[i], "*.*", FS_ST_ARCH, &entry) == SIDETNFS_DIR_SEARCH_FOUND);
    }
    CHECK(sidetnfs_config_drive_search_count_active() == 4);

    /* A 5th, different ndta must not silently evict/corrupt any of the
     * four already-active searches -- controlled failure instead. */
    SidetnfsDirSearchResult r = sidetnfs_config_drive_search_start(0x7010, "*.*", FS_ST_ARCH, &entry);
    CHECK(r == SIDETNFS_DIR_SEARCH_ERROR);

    /* All four original searches must still be intact and independently
     * advanceable. */
    for (int i = 0; i < 4; i++)
    {
        CHECK(sidetnfs_config_drive_search_is_active(ndtas[i]));
    }
    CHECK(sidetnfs_config_drive_search_next(ndtas[0], &entry) == SIDETNFS_DIR_SEARCH_FOUND);
    CHECK_STR_EQ(entry.name, "README.TXT");

    sidetnfs_config_drive_search_close_all();
}

int main(void)
{
    test_full_protocol_sequence();
    test_pattern_prg_only();
    test_pattern_txt_only();
    test_exact_name_case_insensitive();
    test_two_concurrent_dta_addresses();
    test_dta_release_during_active_search();
    test_close_all_with_active_searches();
    test_repeated_fsfirst_same_dta();
    test_slot_exhaustion_does_not_corrupt_other_dta();

    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    /* Note: "no FatFS allocations in configdrive-only" is verified
     * separately, via source inspection of GEMDRVEMUL_FSETDTA_CALL and the
     * ELF/disassembly check after the real Pico build -- not host-testable
     * here, since it concerns gemdrvemul.c's dispatch code and FatFS
     * symbols, not this backend module. */
    return g_failures == 0 ? 0 : 1;
}
