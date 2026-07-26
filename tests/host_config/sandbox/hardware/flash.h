/* Fase 12B host-test stub: the real hardware/flash.h is Pico-SDK-only
 * (not host-compilable). Provides the exact symbols sidetnfs_config.c
 * actually calls (FLASH_PAGE_SIZE, flash_range_erase(),
 * flash_range_program()) plus XIP_BASE, all backed by g_fake_flash
 * (defined in the test driver) instead of real XIP flash -- same pattern
 * tests/host_netconfig uses for config.h's fake-flash stub.
 */
#ifndef HARDWARE_FLASH_H
#define HARDWARE_FLASH_H

#include <stddef.h>
#include <stdint.h>

#define FLASH_PAGE_SIZE 256u
#define FLASH_SECTOR_SIZE 4096u

/* Sized to comfortably hold both SIDETNFS_CONFIG_FLASH_OFFSET (0x100000,
 * drive config, +4096) and SIDETNFS_SYSTEM_CONFIG_FLASH_OFFSET (0x101000,
 * Fase 2 system config, +4096) so "XIP_BASE + <offset>" indexes into this
 * buffer exactly like a real XIP address would, for either test. */
extern uint8_t g_fake_flash[0x102000];
#define XIP_BASE ((uintptr_t)g_fake_flash)

void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);

#endif // HARDWARE_FLASH_H
