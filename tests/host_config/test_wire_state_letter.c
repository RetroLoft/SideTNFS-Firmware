/**
 * File: test_wire_state_letter.c
 * Verification (wire-layout audit): proves that GEMDRVEMUL_SIDETNFS_DRIVE_STATE
 * (0x43B0) and GEMDRVEMUL_SIDETNFS_DRIVE_LETTER (0x43B2) are two fully
 * independent 16-bit shared-memory fields -- writing one can never touch
 * the other's bytes.
 *
 * Does NOT include the real romemul/include/gemdrvemul.h or memfunc.h
 * (both pull in Pico-SDK-only headers a host toolchain can't build).
 * Instead copies, byte-for-byte, the exact macro definitions and the
 * exact numeric offsets already used by both the firmware
 * (romemul/include/gemdrvemul.h) and AtariConfig
 * (AtariConfig/src/sidetnfs_probe.c) -- same "faithful copy" convention
 * already established by tests/host_netconfig/test_netconfig.c.
 *
 * Run:
 *   gcc -std=c11 -Wall -Wextra -o /tmp/test_wire_state_letter \
 *       test_wire_state_letter.c && /tmp/test_wire_state_letter
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- verbatim from romemul/include/memfunc.h ----
#define WRITE_WORD(address, offset, data) *((volatile uint16_t *)((address) + (offset))) = data
#define READ_WORD(address, offset) (*((volatile uint16_t *)((address) + (offset))))

// ---- verbatim numeric offsets, cross-checked against both
// romemul/include/gemdrvemul.h (GEMDRVEMUL_SIDETNFS_DRIVE_STATE/_LETTER)
// and AtariConfig/src/sidetnfs_probe.c (DRIVE_STATE_OFFSET/DRIVE_LETTER_OFFSET) ----
#define DRIVE_STATE_OFFSET  0x43B0UL
#define DRIVE_LETTER_OFFSET 0x43B2UL

#define SIDETNFS_DRIVE_STATE_EMPTY    0
#define SIDETNFS_DRIVE_STATE_DISABLED 1
#define SIDETNFS_DRIVE_STATE_ENABLED  2

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                             \
    do                                                               \
    {                                                                \
        g_checks++;                                                 \
        if (!(cond))                                                \
        {                                                            \
            g_failures++;                                           \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

int main(void)
{
    // Buffer large enough to hold offset 0x43B3 (the last byte of the
    // drive_letter word) plus margin.
    static uint8_t mem[0x4400];
    uintptr_t base = (uintptr_t)mem;

    printf("Wire-layout regression: DRIVE_STATE (0x43B0) vs DRIVE_LETTER (0x43B2)\n");

    // Step 1: recognizable pattern in all four bytes.
    memset(mem + DRIVE_STATE_OFFSET, 0xAA, 4);
    CHECK(mem[DRIVE_STATE_OFFSET + 0] == 0xAA, "pattern byte 0x43B0 set");
    CHECK(mem[DRIVE_STATE_OFFSET + 1] == 0xAA, "pattern byte 0x43B1 set");
    CHECK(mem[DRIVE_LETTER_OFFSET + 0] == 0xAA, "pattern byte 0x43B2 set");
    CHECK(mem[DRIVE_LETTER_OFFSET + 1] == 0xAA, "pattern byte 0x43B3 set");

    // Step 2: write state = ENABLED (2) via the exact same WRITE_WORD
    // macro the firmware uses.
    WRITE_WORD(base, DRIVE_STATE_OFFSET, (uint16_t)SIDETNFS_DRIVE_STATE_ENABLED);

    // Step 3: only 0x43B0-0x43B1 may have changed -- 0x43B2-0x43B3 must
    // still hold the untouched pattern.
    CHECK(mem[DRIVE_LETTER_OFFSET + 0] == 0xAA, "drive_letter byte 0x43B2 untouched by state write");
    CHECK(mem[DRIVE_LETTER_OFFSET + 1] == 0xAA, "drive_letter byte 0x43B3 untouched by state write");

    // Step 4: write drive_letter = 'N' (0x4E) via the same mechanism.
    memset(mem + DRIVE_LETTER_OFFSET, 0xAA, 2); // restore pattern before this sub-check
    WRITE_WORD(base, DRIVE_LETTER_OFFSET, (uint16_t)'N');

    // Step 5: only 0x43B2-0x43B3 may have changed -- 0x43B0-0x43B1 must
    // still hold whatever the state write left there (ENABLED == 2).
    uint16_t state_after_letter_write = READ_WORD(base, DRIVE_STATE_OFFSET);
    CHECK(state_after_letter_write == SIDETNFS_DRIVE_STATE_ENABLED, "state word untouched by drive_letter write");

    // Step 6/7/8: read both back and confirm final values.
    uint16_t state_final = READ_WORD(base, DRIVE_STATE_OFFSET);
    uint16_t letter_final = READ_WORD(base, DRIVE_LETTER_OFFSET);
    CHECK(state_final == SIDETNFS_DRIVE_STATE_ENABLED, "state == ENABLED (2) after both writes");
    CHECK(letter_final == (uint16_t)'N', "drive_letter == 'N' (0x4E) after both writes");

    // Concrete byte dump for the report.
    printf("  bytes: 0x43B0=%02X 0x43B1=%02X 0x43B2=%02X 0x43B3=%02X\n",
           mem[DRIVE_STATE_OFFSET + 0], mem[DRIVE_STATE_OFFSET + 1],
           mem[DRIVE_LETTER_OFFSET + 0], mem[DRIVE_LETTER_OFFSET + 1]);

    // Boundary check: DRIVE_STATE_OFFSET and DRIVE_LETTER_OFFSET must be
    // exactly 2 bytes apart (adjacent uint16_t words, no gap/overlap).
    CHECK(DRIVE_LETTER_OFFSET - DRIVE_STATE_OFFSET == 2, "DRIVE_STATE and DRIVE_LETTER are adjacent, non-overlapping words");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
