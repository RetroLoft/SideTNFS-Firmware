/* Fase 12B host-test stub: the real hardware/sync.h is Pico-SDK-only. No
 * real interrupt state to save on a host binary -- these are no-ops. */
#ifndef HARDWARE_SYNC_H
#define HARDWARE_SYNC_H

#include <stdint.h>

static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t status) { (void)status; }

#endif // HARDWARE_SYNC_H
