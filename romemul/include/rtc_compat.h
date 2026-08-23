/**
 * @file rtc_compat.h
 * @brief RP2350 shim providing the RP2040 hardware_rtc call signatures
 * (rtc_init / rtc_get_datetime / rtc_set_datetime) used by rtcemul.c and
 * gemdrvemul.c, implemented on top of pico-sdk's cross-chip pico_aon_timer
 * abstraction (which drives hardware_powman on RP2350 instead of the
 * RP2040-only RTC peripheral).
 *
 * Only included for non-RP2040 boards -- see rtcemul.h. RP2040 keeps using
 * hardware/rtc.h directly, unchanged.
 *
 * Requires PICO_INCLUDE_RTC_DATETIME=1 (set unconditionally in
 * romemul/CMakeLists.txt) so that datetime_t and the datetime_t<->struct tm
 * converters (datetime_to_tm/tm_to_datetime, pico/util/datetime.h) exist on
 * RP2350 too -- upstream, that header only enables them by default on
 * RP2040.
 */

#ifndef RTC_COMPAT_H
#define RTC_COMPAT_H

#include "pico/aon_timer.h"
#include "pico/util/datetime.h"

static inline void rtc_init(void)
{
    // Mirrors what rtc_init() + the first rtc_set_datetime() gave callers
    // on RP2040: a running clock with *some* well-formed value, overwritten
    // for real as soon as NTP (or another source) calls rtc_set_datetime().
    // aon_timer_start_calendar() both starts the Powman timer and seeds it,
    // matching aon_timer.c's own RP2040 definition of aon_timer_start_calendar
    // (rtc_init() + aon_timer_set_time_calendar()).
    struct tm tm0 = {0};
    tm0.tm_year = 2024 - 1900;
    tm0.tm_mon = 0;
    tm0.tm_mday = 1;
    aon_timer_start_calendar(&tm0);
}

static inline bool rtc_set_datetime(datetime_t *dt)
{
    struct tm tm;
    datetime_to_tm(dt, &tm);
    return aon_timer_set_time_calendar(&tm);
}

static inline bool rtc_get_datetime(datetime_t *dt)
{
    struct tm tm;
    if (!aon_timer_get_time_calendar(&tm))
    {
        return false;
    }
    tm_to_datetime(&tm, dt);
    return true;
}

#endif // RTC_COMPAT_H
