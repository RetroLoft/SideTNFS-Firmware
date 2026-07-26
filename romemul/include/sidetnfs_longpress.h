/**
 * File: sidetnfs_longpress.h
 * Fase 12B2 (SELECT 10s factory reset): pure, host-testable long-press
 * timing decision -- no GPIO/sleep/hardware calls, so it can be unit
 * tested without the Pico SDK (see tests/host_config/test_longpress.c).
 * The actual GPIO polling loop lives in main.c and calls this once per
 * poll tick.
 */
#ifndef SIDETNFS_LONGPRESS_H
#define SIDETNFS_LONGPRESS_H

#include <stdint.h>
#include <stdbool.h>

#define SIDETNFS_FACTORY_RESET_HOLD_MS 10000u
#define SIDETNFS_FACTORY_RESET_POLL_MS 15u

typedef enum
{
    SIDETNFS_LONGPRESS_WAITING,   // still counting, button still held, threshold not reached yet
    SIDETNFS_LONGPRESS_CANCELLED, // button released before the threshold -- caller must change nothing
    SIDETNFS_LONGPRESS_TRIGGERED  // button held continuously for >= threshold_ms
} sidetnfs_longpress_result_t;

// One poll-tick decision. `button_pressed` is the current, debounced
// button reading; `elapsed_ms` is the total time the button has been
// continuously held (including this tick). Never mutates any shared
// state -- pure function of its two inputs, so the caller's own polling
// loop is the only place that accumulates elapsed_ms, and only for as
// long as button_pressed keeps coming back true.
static inline sidetnfs_longpress_result_t sidetnfs_longpress_poll_step(
    bool button_pressed, uint32_t elapsed_ms, uint32_t threshold_ms)
{
    if (!button_pressed)
    {
        return SIDETNFS_LONGPRESS_CANCELLED;
    }
    if (elapsed_ms >= threshold_ms)
    {
        return SIDETNFS_LONGPRESS_TRIGGERED;
    }
    return SIDETNFS_LONGPRESS_WAITING;
}

#endif // SIDETNFS_LONGPRESS_H
