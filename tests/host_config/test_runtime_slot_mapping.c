/**
 * File: test_runtime_slot_mapping.c
 * Fase 12B3: host test for the runtime-slot assignment DECISION LOGIC
 * that romemul/gemdrvemul.c's sidetnfs_runtime_drives_init() implements.
 *
 * gemdrvemul.c itself is not host-compilable (Pico SDK/hardware
 * dependencies throughout), so this test does NOT link the real
 * function. Instead it re-implements, as a small pure function, exactly
 * the same slot-assignment rules that were traced line-by-line against
 * the real sidetnfs_runtime_drives_init() for this report (see the
 * report's "Geval A-E" section for the full trace with line references).
 * Same "faithful mirror" convention tests/host_config/test_config_slot_states.c
 * already uses for sidetnfs_probe_load_active_server()'s scan logic.
 *
 * If this mirror and the real function ever diverge, that is exactly
 * the kind of drift this test exists to catch on the next review pass --
 * keep the two in sync by hand.
 *
 * Run:
 *   gcc -std=c11 -Wall -Wextra -o /tmp/test_runtime_slot_mapping \
 *       test_runtime_slot_mapping.c && /tmp/test_runtime_slot_mapping
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_RUNTIME 3 /* this test only ever needs N, O, SETTINGS */

typedef enum
{
    BACKEND_NONE = 0,
    BACKEND_TNFS,
    BACKEND_SETTINGS
} backend_t;

typedef struct
{
    bool valid;
    char letter;
    uint32_t drive_number;
    backend_t backend;
} runtime_slot_t;

// Mirrors sidetnfs_runtime_drives_init()'s exact decision sequence:
//   1. slot 0 = whatever `active_letter` names (already resolved
//      upstream by select_gemdrive_drive_letter()/
//      sidetnfs_probe_load_active_server(), which itself skips
//      DISABLED/EMPTY slots -- see report Deel A/F).
//   2. O: gets its own slot ONLY if it is a DIFFERENT drive_number than
//      slot 0's (this is the de-duplication that makes "O disabled" and
//      "N disabled, O becomes slot 0" both work without a double entry).
//   3. SETTINGS is appended after however many of the above ended up
//      valid, UNLESS slot 0 itself already IS the settings letter
//      (legacy CONFIG_DRIVE_ONLY build case -- not exercised by these
//      tests, all of which use a real active_letter).
static void assign_runtime_slots(char active_letter, bool o_enabled, char settings_letter,
                                  runtime_slot_t out[MAX_RUNTIME])
{
    for (int i = 0; i < MAX_RUNTIME; i++)
    {
        out[i].valid = false;
        out[i].letter = 0;
        out[i].drive_number = 0xFFFFFFFFu;
        out[i].backend = BACKEND_NONE;
    }

    if (active_letter >= 'A' && active_letter <= 'Z')
    {
        out[0].valid = true;
        out[0].letter = active_letter;
        out[0].drive_number = (uint32_t)(active_letter - 'A');
        out[0].backend = BACKEND_TNFS;
    }

    uint32_t o_drive_number = (uint32_t)('O' - 'A');
    bool o_valid = out[0].valid && o_enabled && (o_drive_number != out[0].drive_number);
    if (o_valid)
    {
        out[1].valid = true;
        out[1].letter = 'O';
        out[1].drive_number = o_drive_number;
        out[1].backend = BACKEND_TNFS;
    }

    bool slot0_is_settings = out[0].valid && out[0].letter == settings_letter;
    if (slot0_is_settings)
    {
        out[0].backend = BACKEND_SETTINGS;
    }
    else
    {
        int settings_slot = o_valid ? 2 : (out[0].valid ? 1 : 0);
        out[settings_slot].valid = true;
        out[settings_slot].letter = settings_letter;
        out[settings_slot].drive_number = (uint32_t)(settings_letter - 'A');
        out[settings_slot].backend = BACKEND_SETTINGS;
    }
}

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                             \
    do                                                               \
    {                                                                \
        g_checks++;                                                  \
        if (!(cond))                                                 \
        {                                                            \
            g_failures++;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

// Test 1 -- N enabled, O enabled, SETTINGS on S.
static void test1_both_enabled(void)
{
    printf("Test 1: N enabled, O enabled, SETTINGS=S\n");
    runtime_slot_t r[MAX_RUNTIME];
    assign_runtime_slots('N', true, 'S', r);

    CHECK(r[0].valid && r[0].letter == 'N' && r[0].backend == BACKEND_TNFS, "slot0 = N/TNFS");
    CHECK(r[1].valid && r[1].letter == 'O' && r[1].backend == BACKEND_TNFS, "slot1 = O/TNFS");
    CHECK(r[2].valid && r[2].letter == 'S' && r[2].backend == BACKEND_SETTINGS, "slot2 = S/SETTINGS");
    CHECK(r[0].drive_number != r[1].drive_number, "N and O have different drive_numbers");
}

// Test 2 -- only N enabled (O disabled): no gap, SETTINGS directly after.
static void test2_only_n_enabled(void)
{
    printf("Test 2: N enabled, O disabled -- no gap, SETTINGS right after\n");
    runtime_slot_t r[MAX_RUNTIME];
    assign_runtime_slots('N', false, 'S', r);

    CHECK(r[0].valid && r[0].letter == 'N' && r[0].backend == BACKEND_TNFS, "slot0 = N/TNFS");
    CHECK(r[1].valid && r[1].letter == 'S' && r[1].backend == BACKEND_SETTINGS, "slot1 = S/SETTINGS (no gap)");
    CHECK(!r[2].valid, "slot2 unused");
}

// Test 3 -- N disabled, O enabled: O becomes slot 0 (not slot 1!),
// SETTINGS follows at slot 1. This is exactly the case the instruction
// warns must never be assumed to always land O at runtime-slot 1.
static void test3_only_o_enabled(void)
{
    printf("Test 3: N disabled, O enabled -- O becomes runtime slot 0\n");
    // active_letter is 'O' here because sidetnfs_probe_load_active_server()
    // (upstream of sidetnfs_runtime_drives_init(), skipping DISABLED N)
    // would have selected O as the active server -- see report Deel A/F.
    runtime_slot_t r[MAX_RUNTIME];
    assign_runtime_slots('O', true, 'S', r);

    CHECK(r[0].valid && r[0].letter == 'O' && r[0].backend == BACKEND_TNFS, "slot0 = O/TNFS (not N)");
    CHECK(r[0].drive_number == (uint32_t)('O' - 'A'), "slot0 drive_number is O's, not a stale N value");
    CHECK(r[1].valid && r[1].letter == 'S' && r[1].backend == BACKEND_SETTINGS, "slot1 = S/SETTINGS directly after (no duplicate O entry)");
    CHECK(!r[2].valid, "slot2 unused -- O is correctly de-duplicated, not double-published");
}

// Test 4 -- both N and O disabled: no active TNFS drive at all.
// Confirms the (separately reported, not fixed) legacy phantom-letter
// fallback is the ONLY way slot 0 becomes valid in this case -- with a
// real active_letter of 0 (no active server, no legacy fallback either),
// only SETTINGS is ever published, at slot 0, with no out-of-bounds
// access anywhere in the assignment logic.
static void test4_both_disabled(void)
{
    printf("Test 4: N and O both disabled -- only SETTINGS, no crash\n");
    runtime_slot_t r[MAX_RUNTIME];
    assign_runtime_slots(0, false, 'S', r); // 0 = no active letter at all (idealized: no legacy fallback)

    CHECK(r[0].backend != BACKEND_TNFS, "slot0 never becomes a TNFS entry when there is truly no active letter");
    // SETTINGS still gets published (at slot0, since nothing else claimed it).
    int settings_slot = -1;
    for (int i = 0; i < MAX_RUNTIME; i++)
        if (r[i].valid && r[i].backend == BACKEND_SETTINGS) settings_slot = i;
    CHECK(settings_slot == 0, "SETTINGS published at slot0 when nothing else is valid");
    CHECK(!r[1].valid && !r[2].valid, "no other slot spuriously valid");
}

// Test 5 -- SETTINGS letter changed: only letter/drive_number differ,
// backend/slot mapping for N/O is unaffected.
static void test5_settings_letter_changed(void)
{
    printf("Test 5: SETTINGS letter changed -- only letter/drive_number change\n");
    runtime_slot_t r_before[MAX_RUNTIME];
    runtime_slot_t r_after[MAX_RUNTIME];
    assign_runtime_slots('N', true, 'S', r_before);
    assign_runtime_slots('N', true, 'W', r_after);

    CHECK(r_before[0].letter == r_after[0].letter && r_before[0].backend == r_after[0].backend, "slot0 (N) unaffected by settings-letter change");
    CHECK(r_before[1].letter == r_after[1].letter && r_before[1].backend == r_after[1].backend, "slot1 (O) unaffected by settings-letter change");
    CHECK(r_after[2].letter == 'W', "slot2 letter follows the new settings letter");
    CHECK(r_after[2].backend == BACKEND_SETTINGS, "slot2 backend still SETTINGS after the letter change");
    CHECK(r_after[2].drive_number == (uint32_t)('W' - 'A'), "slot2 drive_number recomputed from the new letter");
}

// Test 6 -- SETTINGS must never be mistaken for a TNFS slot at runtime
// slot 2 (or any slot): backend tag is the single source of truth a
// caller must check before ever treating a slot's index as a TNFS
// session/context index (see sidetnfs_runtime_slot_is_settings() in
// gemdrvemul.c, and sidetnfs_probe_get_slot_context()'s own
// backend-agnostic indexing -- it will happily return whatever was
// stored at that index, TNFS or not, so the CALLER's backend check is
// the only thing preventing misuse).
static void test6_settings_never_tnfs_slot(void)
{
    printf("Test 6: SETTINGS at runtime slot 2 is never backend TNFS\n");
    runtime_slot_t r[MAX_RUNTIME];
    assign_runtime_slots('N', true, 'S', r);
    CHECK(r[2].backend == BACKEND_SETTINGS, "slot2 backend is SETTINGS");
    CHECK(r[2].backend != BACKEND_TNFS, "slot2 is never tagged TNFS, regardless of its numeric index");
}

int main(void)
{
    test1_both_enabled();
    test2_only_n_enabled();
    test3_only_o_enabled();
    test4_both_disabled();
    test5_settings_letter_changed();
    test6_settings_never_tnfs_slot();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
