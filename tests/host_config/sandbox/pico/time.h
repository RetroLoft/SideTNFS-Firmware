#ifndef PICO_TIME_H
#define PICO_TIME_H
#include <stdint.h>
typedef int64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return 0; }
static inline void sleep_us(uint32_t us) { (void)us; }
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return to - from; }
#endif
