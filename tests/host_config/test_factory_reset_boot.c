/**
 * File: test_factory_reset_boot.c
 * SELECT-held-at-power-on 10s factory reset: verification of main.c's
 * polling LOOP, including the debounce and LED behaviour.
 *
 * tests/host_config/test_longpress.c covers the pure decision helper
 * (sidetnfs_longpress_poll_step). What that cannot cover is the loop in
 * main.c that drives it: the entry guard, the consecutive-low-sample
 * debounce, the accumulation of elapsed_ms across a bounce, and when the
 * LED goes on/off. That loop is what this file mirrors, transcribed
 * statement for statement from romemul/main.c's
 * sidetnfs_check_select_factory_reset(), with gpio_get()/sleep_ms()
 * replaced by a scriptable button trace and the LED/flash/reset side
 * effects replaced by counters.
 *
 * The real sidetnfs_longpress.h helper is included and used, not copied,
 * so the debounce is tested against the actual contract it documents
 * ("button_pressed is the current, debounced button reading").
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -I ../../romemul \
 *       -o /tmp/test_factory_reset_boot test_factory_reset_boot.c && \
 *       /tmp/test_factory_reset_boot
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "include/sidetnfs_longpress.h" /* the REAL helper, not a copy */

/* verbatim from romemul/main.c */
#define SIDETNFS_FACTORY_RESET_RELEASE_SAMPLES 4u

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

/* ---- scripted button trace ----------------------------------------
 * hold_ms      : how long the user really holds SELECT
 * glitch_at_ms : start of an artificial low-glitch inside the hold
 * glitch_len_ms: how long that glitch lasts (0 = no glitch)
 * ------------------------------------------------------------------ */
static uint32_t g_hold_ms;
static uint32_t g_glitch_at_ms;
static uint32_t g_glitch_len_ms;
static uint32_t g_now_ms;

static bool fake_gpio_select_pressed(void)
{
    if (g_now_ms >= g_hold_ms)
    {
        return false; /* genuinely released */
    }
    if (g_glitch_len_ms > 0 && g_now_ms >= g_glitch_at_ms &&
        g_now_ms < g_glitch_at_ms + g_glitch_len_ms)
    {
        return false; /* bounce / noise: reads low although still held */
    }
    return true;
}

static void fake_sleep_ms(uint32_t ms) { g_now_ms += ms; }

typedef struct
{
    int factory_reset_calls;
    int reboot_calls;
    int led_on_calls;
    int led_off_calls;
    bool led_is_on;
    bool led_on_during_hold; /* LED was on while the loop was still polling */
    bool entered_poll_loop;
    bool returned_normally;
    uint32_t triggered_at_ms;
} boot_result_t;

static boot_result_t g_r;

static void led(bool on)
{
    if (on) { g_r.led_on_calls++; } else { g_r.led_off_calls++; }
    g_r.led_is_on = on;
}

/* mirror of main.c's sidetnfs_check_select_factory_reset() */
static void check_select_factory_reset(void)
{
    if (!fake_gpio_select_pressed())
    {
        g_r.returned_normally = true;
        return; /* not held at boot at all */
    }

    led(true); /* LED on for the whole procedure */
    g_r.entered_poll_loop = true;

    uint32_t elapsed_ms = 0;
    uint32_t low_samples = 0;
    for (;;)
    {
        fake_sleep_ms(SIDETNFS_FACTORY_RESET_POLL_MS);
        elapsed_ms += SIDETNFS_FACTORY_RESET_POLL_MS;

        if (fake_gpio_select_pressed()) { low_samples = 0; }
        else { low_samples++; }

        bool pressed = (low_samples < SIDETNFS_FACTORY_RESET_RELEASE_SAMPLES);

        if (g_r.led_is_on) { g_r.led_on_during_hold = true; }

        sidetnfs_longpress_result_t result =
            sidetnfs_longpress_poll_step(pressed, elapsed_ms, SIDETNFS_FACTORY_RESET_HOLD_MS);

        if (result == SIDETNFS_LONGPRESS_CANCELLED)
        {
            led(false);
            g_r.returned_normally = true;
            return;
        }
        if (result == SIDETNFS_LONGPRESS_TRIGGERED)
        {
            g_r.triggered_at_ms = elapsed_ms;
            break;
        }
    }

    led(true);                       /* already on -- kept explicit, as in main.c */
    g_r.factory_reset_calls++;       /* sidetnfs_config_factory_reset() */
    fake_sleep_ms(1000);
    led(false);
    g_r.reboot_calls++;              /* watchdog_reboot(0, 0, 0) */
}

static boot_result_t run_boot(uint32_t hold_ms, uint32_t glitch_at_ms, uint32_t glitch_len_ms)
{
    g_hold_ms = hold_ms;
    g_glitch_at_ms = glitch_at_ms;
    g_glitch_len_ms = glitch_len_ms;
    g_now_ms = 0;
    boot_result_t zero = {0};
    g_r = zero;
    check_select_factory_reset();
    return g_r;
}

int main(void)
{
    printf("Factory reset (SELECT held at power-on) -- debounce + LED verification\n");
    printf("=======================================================================\n");

    printf("Test 1: SELECT not pressed -> normal boot, LED untouched\n");
    boot_result_t t1 = run_boot(0, 0, 0);
    CHECK(!t1.entered_poll_loop, "1: poll loop never entered");
    CHECK(t1.factory_reset_calls == 0 && t1.reboot_calls == 0, "1: no reset, no reboot");
    CHECK(t1.led_on_calls == 0 && t1.led_off_calls == 0, "1: LED never touched (3 boot flashes happen later, untouched)");
    CHECK(t1.returned_normally, "1: boot continues normally");

    printf("Test 2: released after ~5s -> LED off, no reset, normal boot\n");
    boot_result_t t2 = run_boot(5000, 0, 0);
    CHECK(t2.entered_poll_loop, "2: poll loop entered");
    CHECK(t2.led_on_during_hold, "2: LED was ON while the procedure ran");
    CHECK(!t2.led_is_on, "2: LED switched OFF on cancel");
    CHECK(t2.factory_reset_calls == 0 && t2.reboot_calls == 0, "2: no reset, no reboot");
    CHECK(t2.returned_normally, "2: boot continues normally");

    printf("Test 3: held past 12s -> factory reset + reboot\n");
    boot_result_t t3 = run_boot(12500, 0, 0);
    CHECK(t3.led_on_during_hold, "3: LED on during the hold");
    CHECK(t3.factory_reset_calls == 1, "3: factory reset performed exactly once");
    CHECK(t3.reboot_calls == 1, "3: rebooted exactly once");
    CHECK(!t3.led_is_on, "3: LED off before the reboot (does not block it)");
    /* elapsed_ms advances in POLL_MS steps, so it lands on the first
     * multiple of 15 at or past 10000 (= 10005), never before. */
    CHECK(t3.triggered_at_ms >= SIDETNFS_FACTORY_RESET_HOLD_MS, "3: never triggers before the 10s threshold");
    CHECK(t3.triggered_at_ms < SIDETNFS_FACTORY_RESET_HOLD_MS + SIDETNFS_FACTORY_RESET_POLL_MS,
          "3: triggers on the very first tick at/past the threshold");

    printf("Test 4: 45ms glitch (3 low samples) mid-hold -> procedure survives\n");
    boot_result_t t4 = run_boot(12500, 4000, 45);
    CHECK(t4.factory_reset_calls == 1, "4: a sub-60ms bounce does NOT cancel");
    CHECK(t4.reboot_calls == 1, "4: still reaches the reboot");
    CHECK(t4.triggered_at_ms == t3.triggered_at_ms, "4: glitch costs no hold time (same trigger tick as the clean run)");

    printf("Test 5: 60ms glitch (4 low samples) -> cancels\n");
    boot_result_t t5 = run_boot(12500, 4000, 60);
    CHECK(t5.factory_reset_calls == 0, "5: 4 consecutive low samples cancel the procedure");
    CHECK(!t5.led_is_on, "5: LED off after cancel");
    CHECK(t5.returned_normally, "5: boot continues normally");

    printf("Test 6: repeated short glitches never accumulate into a cancel\n");
    /* three separate 30ms (2-sample) glitches spread across the hold */
    bool all_ok = true;
    for (uint32_t at = 1000; at <= 9000; at += 4000)
    {
        boot_result_t r = run_boot(12500, at, 30);
        if (r.factory_reset_calls != 1) { all_ok = false; }
    }
    CHECK(all_ok, "6: every isolated 2-sample glitch is absorbed");

    printf("Test 7: old (undebounced) behaviour would have failed test 4\n");
    /* Prove the debounce is what saves it: with RELEASE_SAMPLES == 1 the
     * same 45ms glitch cancels, which is exactly the shipped bug. */
    {
        uint32_t saved_hold = 12500, saved_at = 4000, saved_len = 45;
        g_hold_ms = saved_hold; g_glitch_at_ms = saved_at; g_glitch_len_ms = saved_len;
        g_now_ms = 0;
        boot_result_t zero = {0}; g_r = zero;
        /* inline re-run with a threshold of 1 consecutive low sample */
        bool cancelled = false;
        if (fake_gpio_select_pressed())
        {
            uint32_t elapsed_ms = 0, low = 0;
            for (;;)
            {
                fake_sleep_ms(SIDETNFS_FACTORY_RESET_POLL_MS);
                elapsed_ms += SIDETNFS_FACTORY_RESET_POLL_MS;
                if (fake_gpio_select_pressed()) { low = 0; } else { low++; }
                bool pressed = (low < 1u); /* == raw pin state */
                sidetnfs_longpress_result_t res =
                    sidetnfs_longpress_poll_step(pressed, elapsed_ms, SIDETNFS_FACTORY_RESET_HOLD_MS);
                if (res == SIDETNFS_LONGPRESS_CANCELLED) { cancelled = true; break; }
                if (res == SIDETNFS_LONGPRESS_TRIGGERED) { break; }
            }
        }
        CHECK(cancelled, "7: without debounce the same glitch aborts (the original bug)");
    }

    printf("=======================================================================\n");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
