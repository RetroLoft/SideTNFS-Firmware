/**
 * File: test_longpress.c
 * Fase 12B2: host test for the SELECT 10s factory-reset long-press
 * timing decision (sidetnfs_longpress_poll_step(), header-only, no SDK
 * dependency -- see romemul/include/sidetnfs_longpress.h).
 *
 * Run:
 *   gcc -std=c11 -Wall -Wextra -I ../../romemul -o /tmp/test_longpress \
 *       test_longpress.c && /tmp/test_longpress
 */
#include <stdio.h>

#include "include/sidetnfs_longpress.h"

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

// Test 9: long-press shorter than 10 seconds -- never TRIGGERED, and
// CANCELLED as soon as the button is released, regardless of how close
// to the threshold it got.
static void test9_short_press_never_triggers(void)
{
    printf("Test 9: long-press shorter than 10s never triggers\n");

    // Held for 9999ms then released -- one tick before the threshold.
    CHECK(sidetnfs_longpress_poll_step(true, 9999u, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_WAITING,
          "9999ms held: still WAITING, not yet TRIGGERED");
    CHECK(sidetnfs_longpress_poll_step(false, 9999u, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_CANCELLED,
          "released at 9999ms: CANCELLED");

    // Held briefly (one poll tick) then released.
    CHECK(sidetnfs_longpress_poll_step(true, SIDETNFS_FACTORY_RESET_POLL_MS, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_WAITING,
          "one tick held: WAITING");
    CHECK(sidetnfs_longpress_poll_step(false, SIDETNFS_FACTORY_RESET_POLL_MS, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_CANCELLED,
          "released after one tick: CANCELLED");

    // Never pressed at all.
    CHECK(sidetnfs_longpress_poll_step(false, 0u, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_CANCELLED,
          "never pressed: CANCELLED immediately");
}

// Test 10: exactly 10 seconds of continuous hold -- TRIGGERED exactly
// once at/after the threshold, never before it.
static void test10_full_press_triggers_exactly_once(void)
{
    printf("Test 10: long-press of exactly 10s triggers exactly once\n");

    uint32_t elapsed = 0;
    int trigger_count = 0;
    int transitions_to_triggered = 0;

    // Simulate the real polling loop: accumulate elapsed_ms in
    // SIDETNFS_FACTORY_RESET_POLL_MS steps, button held throughout,
    // stop polling the instant TRIGGERED first appears (matching
    // main.c's for(;;) loop, which breaks out right there).
    for (;;)
    {
        elapsed += SIDETNFS_FACTORY_RESET_POLL_MS;
        sidetnfs_longpress_result_t r = sidetnfs_longpress_poll_step(true, elapsed, SIDETNFS_FACTORY_RESET_HOLD_MS);
        CHECK(r != SIDETNFS_LONGPRESS_CANCELLED, "button held throughout: never CANCELLED");
        if (r == SIDETNFS_LONGPRESS_TRIGGERED)
        {
            trigger_count++;
            transitions_to_triggered++;
            break; // the real caller's loop breaks here too -- exactly one factory-save follows
        }
        if (elapsed > SIDETNFS_FACTORY_RESET_HOLD_MS + 1000u)
        {
            break; // safety bound for this test loop only, should never be hit
        }
    }

    CHECK(trigger_count == 1, "TRIGGERED reached exactly once in the simulated poll loop");
    CHECK(transitions_to_triggered == 1, "exactly one factory-save/reset request would be issued");
    CHECK(elapsed >= SIDETNFS_FACTORY_RESET_HOLD_MS, "trigger only happens at/after the full 10000ms threshold");

    // Confirm the threshold boundary itself: exactly at 10000ms is
    // TRIGGERED, one tick before is not.
    CHECK(sidetnfs_longpress_poll_step(true, SIDETNFS_FACTORY_RESET_HOLD_MS, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_TRIGGERED,
          "exactly at threshold: TRIGGERED");
    CHECK(sidetnfs_longpress_poll_step(true, SIDETNFS_FACTORY_RESET_HOLD_MS - 1u, SIDETNFS_FACTORY_RESET_HOLD_MS) == SIDETNFS_LONGPRESS_WAITING,
          "one ms before threshold: still WAITING");
}

int main(void)
{
    test9_short_press_never_triggers();
    test10_full_press_triggers_exactly_once();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
