/**
 * File: gemdrvemul.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: Header file for the GEMDRIVE C program.
 */

#ifndef GEMDRVEMUL_H
#define GEMDRVEMUL_H

#include "debug.h"
#include "constants.h"
#include "firmware_gemdrvemul.h"
#include "sidetnfs_config.h"
#include "sidetnfs_netconfig.h"
#include "sidetnfs_rtcconfig.h"
#include "sidetnfs_update_check.h" // SIDETNFS_UPDATE_VERSION_LEN -- see GEMDRVEMUL_SIDETNFS_UPDATE below
#include "sidetnfs_floppy_browse.h" // FLOPPY_BROWSE_CWD_LEN/_PAGE_ENTRIES -- see GEMDRVEMUL_FLOPPY_BROWSE/_PAGE below
#include "sidetnfs_floppy_emul.h" // sidetnfs_floppy_source_t/sidetnfs_floppy_emul_open/close/read_sector, boot-policy flags -- see GEMDRVEMUL_FLOPPY_SESSION_START/_READ_SECTOR handlers in gemdrvemul.c
#include "tprotocol.h" // MAX_PROTOCOL_PAYLOAD_SIZE
#include "sidetnfs_probe.h" // SIDETNFS_NET_ERR_TEXT_MAX -- see FileDescriptors.net_err_text below
#include "sidetnfs_sd_service.h" // SIDETNFS_SD_ERROR_TEXT_MAX -- see FileDescriptors.sd_error_text below

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "time.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include <hardware/watchdog.h>
#include "hardware/structs/bus_ctrl.h"
#include "pico/cyw43_arch.h"

// See romemul/include/rtcemul.h for why this is board-conditional (RP2350
// has no hardware_rtc peripheral/library) -- gemdrvemul.c calls rtc_init()
// directly, so it needs the same rtc_compat.h shim on non-RP2040 boards.
#if PICO_RP2040
#include "hardware/rtc.h"
#else
#include "rtc_compat.h"
#endif

#include "sd_card.h"
#include "f_util.h"

#include "../../build/romemul.pio.h"

#include "tprotocol.h"
#include "commands.h"
#include "config.h"
#include "memfunc.h"
#include "filesys.h"
#include "scfs.h"
#include "rtcemul.h"

#define DEFAULT_FOPEN_READ_BUFFER_SIZE 16384
#define DEFAULT_FWRITE_BUFFER_SIZE 2048
#define FIRST_FILE_DESCRIPTOR 16384
#define PRG_STRUCT_SIZE 28 // Size of the GEMDOS structure in the executable header file (PRG)
#define SHARED_VARIABLES_MAXSIZE 32
#define SHARED_VARIABLES_SIZE 7
#define DTA_SIZE_ON_ST 44

// Now the index for the shared variables of the program
#define SHARED_VARIABLE_FIRST_FILE_DESCRIPTOR SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 0
#define SHARED_VARIABLE_DRIVE_LETTER SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 1
#define SHARED_VARIABLE_DRIVE_NUMBER SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 2
#define SHARED_VARIABLE_PEXEC_RESTORE SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 3
#define SHARED_VARIABLE_FAKE_FLOPPY SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 4

// Mirrors
// sidecart-gemdrive-atari/src/gemdrive.s exactly --
// SHARED_VARIABLE_PROTOCOL_VERSION/_DRIVE_COUNT/_DRIVE_NUMBER_TABLE (same
// names, same SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE-relative indices 21/22/23).
// The 68k ROM reads and validates this table right after PING succeeds,
// before it installs its GEMDOS trap -- refuses to boot (no silent
// single-drive fallback) if SIDETNFS_GEMDOS_SLOT_PROTOCOL_VERSION doesn't
// match exactly. GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES mirrors the 68k's
// SIDETNFS_MAX_RUNTIME_DRIVES (SIDETNFS_MAX_DRIVES ordinary drives + 1
// CONFIG drive) -- the number of 4-byte slots in the table, indices
// SHARED_VARIABLE_DRIVE_NUMBER_TABLE..+8. In this phase drive count is
// always 1, so only slot 0 is ever read by the 68k side (see
// validate_drive_table/create_virtual_hard_disk in gemdrive.s, both of
// which loop exactly SHARED_VARIABLE_DRIVE_COUNT times) -- the remaining
// eight slots are unread filler, still initialized to a value that fails
// validate_drive_table's own 0..25 range check (-1) rather than left
// undefined, so a future drive-count increase can never silently pick up
// a stale/garbage slot as a real drive.
#define SHARED_VARIABLE_PROTOCOL_VERSION SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 5
#define SHARED_VARIABLE_DRIVE_COUNT SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 6
#define SHARED_VARIABLE_DRIVE_NUMBER_TABLE SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE + 7
#define GEMDRVEMUL_SIDETNFS_MAX_RUNTIME_DRIVES (SIDETNFS_MAX_DRIVES + 1)
#define SIDETNFS_GEMDOS_SLOT_PROTOCOL_VERSION 1

#define GEMDRVEMUL_RANDOM_TOKEN (0x0)                                   // Offset from 0x0000
#define GEMDRVEMUL_RANDOM_TOKEN_SEED (GEMDRVEMUL_RANDOM_TOKEN + 4)      // random_token + 4 bytes
#define GEMDRVEMUL_TIMEOUT_SEC (GEMDRVEMUL_RANDOM_TOKEN_SEED + 4)       // random_token_seed + 4 bytes
#define GEMDRVEMUL_PING_STATUS (GEMDRVEMUL_TIMEOUT_SEC + 4)             // timeout_sec + 4 bytes
#define GEMDRVEMUL_RTC_STATUS (GEMDRVEMUL_PING_STATUS + 4)              // ping status + 4 bytes
#define GEMDRVEMUL_NETWORK_STATUS (GEMDRVEMUL_RTC_STATUS + 8)           // rtc status + 8 bytes
#define GEMDRVEMUL_RTC_ENABLED (GEMDRVEMUL_NETWORK_STATUS + 4)          // network status + 4 bytes
#define GEMDRVEMUL_REENTRY_TRAP (GEMDRVEMUL_RTC_ENABLED + 8)            // rtc enabled + 4 bytes + 4 GAP
#define GEMDRVEMUL_OLD_XBIOS_TRAP (GEMDRVEMUL_REENTRY_TRAP + 4)         // reentry_trap + 4 bytes
#define GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP (GEMDRVEMUL_OLD_XBIOS_TRAP+4) // old_xbios_trap + 4 bytes
#define GEMDRVEMUL_RTC_DATETIME_BCD (GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP + 4) // reentry_trap + 4 bytes
#define GEMDRVEMUL_RTC_DATETIME_MSDOS (GEMDRVEMUL_RTC_DATETIME_BCD + 8)     // rtc_datetime_bcd + 8 bytes
#define GEMDRVEMUL_RTC_Y2K_PATCH (GEMDRVEMUL_RTC_DATETIME_MSDOS + 8)        // rtc_datetime_msdos + 8 bytes

#define GEMDRVEMUL_DEFAULT_PATH (GEMDRVEMUL_RTC_Y2K_PATCH + 4)              // rtc_y2k_patch + 4 bytes
#define GEMDRVEMUL_DTA_F_FOUND (GEMDRVEMUL_DEFAULT_PATH + 128)          // default path + 128 bytes
#define GEMDRVEMUL_DTA_TRANSFER (GEMDRVEMUL_DTA_F_FOUND + 4)            // dta found + 4
#define GEMDRVEMUL_DTA_EXIST (GEMDRVEMUL_DTA_TRANSFER + DTA_SIZE_ON_ST) // dta transfer + DTA_SIZE_ON_ST bytes
#define GEMDRVEMUL_DTA_RELEASE (GEMDRVEMUL_DTA_EXIST + 4)               // dta exist + 4 bytes
#define GEMDRVEMUL_SET_DPATH_STATUS (GEMDRVEMUL_DTA_RELEASE + 4)        // dta release + 4 bytes
#define GEMDRVEMUL_FOPEN_HANDLE (GEMDRVEMUL_SET_DPATH_STATUS + 4)       // set dpath status + 4 bytes

#define GEMDRVEMUL_READ_BYTES (GEMDRVEMUL_FOPEN_HANDLE + 4)                            // fopen handle + 4 bytes.
#define GEMDRVEMUL_READ_BUFF (GEMDRVEMUL_READ_BYTES + 4)                               // read bytes + 4 bytes
#define GEMDRVEMUL_WRITE_BYTES (GEMDRVEMUL_READ_BUFF + DEFAULT_FOPEN_READ_BUFFER_SIZE) // GEMDRVEMUL_READ_BUFFER + DEFAULT_FOPEN_READ_BUFFER_SIZE bytes
#define GEMDRVEMUL_WRITE_CHK (GEMDRVEMUL_WRITE_BYTES + 4)                              // GEMDRVEMUL_WRITE_BYTES + 4 bytes
#define GEMDRVEMUL_WRITE_CONFIRM_STATUS (GEMDRVEMUL_WRITE_CHK + 4)                     // write check + 4 bytes

#define GEMDRVEMUL_FCLOSE_STATUS (GEMDRVEMUL_WRITE_CONFIRM_STATUS + 4) // read buff + 4 bytes
#define GEMDRVEMUL_DCREATE_STATUS (GEMDRVEMUL_FCLOSE_STATUS + 4)       // fclose status + 2 bytes + 2 bytes padding
#define GEMDRVEMUL_DDELETE_STATUS (GEMDRVEMUL_DCREATE_STATUS + 4)      // dcreate status + 2 bytes + 2 bytes padding
#define GEMDRVEMUL_EXEC_HEADER (GEMDRVEMUL_DDELETE_STATUS + 4)         // ddelete status + 2 bytes + 2 bytes padding. Must be aligned to 4 bytes/32 bits
#define GEMDRVEMUL_FCREATE_HANDLE (GEMDRVEMUL_EXEC_HEADER + 32)        // exec header + 32 bytes
#define GEMDRVEMUL_FDELETE_STATUS (GEMDRVEMUL_FCREATE_HANDLE + 4)      // fcreate handle + 4 bytes
#define GEMDRVEMUL_FSEEK_STATUS (GEMDRVEMUL_FDELETE_STATUS + 4)        // fdelete status + 4 bytes
#define GEMDRVEMUL_FATTRIB_STATUS (GEMDRVEMUL_FSEEK_STATUS + 4)        // fseek status + 4
#define GEMDRVEMUL_FRENAME_STATUS (GEMDRVEMUL_FATTRIB_STATUS + 4)      // fattrib status + 4 bytes
#define GEMDRVEMUL_FDATETIME_DATE (GEMDRVEMUL_FRENAME_STATUS + 4)      // frename status + 4 bytes
#define GEMDRVEMUL_FDATETIME_TIME (GEMDRVEMUL_FDATETIME_DATE + 4)      // fdatetime date + 4
#define GEMDRVEMUL_FDATETIME_STATUS (GEMDRVEMUL_FDATETIME_TIME + 4)    // fdatetime time + 4 bytes
#define GEMDRVEMUL_DFREE_STATUS (GEMDRVEMUL_FDATETIME_STATUS + 4)      // fdatetime status + 4 bytes
#define GEMDRVEMUL_DFREE_STRUCT (GEMDRVEMUL_DFREE_STATUS + 4)          // dfree status + 4 bytes

#define GEMDRVEMUL_PEXEC_MODE (GEMDRVEMUL_DFREE_STRUCT + 32)     // dfree struct + 32 bytes
#define GEMDRVEMUL_PEXEC_STACK_ADDR (GEMDRVEMUL_PEXEC_MODE + 4)  // pexec mode + 4 bytes
#define GEMDRVEMUL_PEXEC_FNAME (GEMDRVEMUL_PEXEC_STACK_ADDR + 4) // pexec stack addr + 4 bytes
#define GEMDRVEMUL_PEXEC_CMDLINE (GEMDRVEMUL_PEXEC_FNAME + 4)    // pexec fname + 4 bytes
#define GEMDRVEMUL_PEXEC_ENVSTR (GEMDRVEMUL_PEXEC_CMDLINE + 4)   // pexec cmd line + 4 bytes

#define GEMDRVEMUL_SHARED_VARIABLES (GEMDRVEMUL_PEXEC_ENVSTR + 4) // pexec envstr + 4 bytes

#define GEMDRVEMUL_EXEC_PD (GEMDRVEMUL_SHARED_VARIABLES + 256) // shared variables + 256 bytes

// Start of the SIDETNFS config-protocol response block. Placed
// right after the PD copy written by GEMDRVEMUL_PEXEC_CALL/SAVE_BASEPAGE
// (memcpy(pexec_pd, origin, sizeof(PD)) at GEMDRVEMUL_EXEC_PD,
// sizeof(PD) == 256 bytes -- see struct _pd in this file: p_cmdlin[128] at
// offset 0x80 is the last member, so the struct ends exactly at 0x100/256).
// GEMDRVEMUL_EXEC_PD is the last shared-memory offset used anywhere in
// gemdrvemul.c (verified: no macro or literal offset beyond it is read or
// written); this is therefore the first provably-free, 4-byte-aligned
// offset in the 64KB ROM3 shared-memory window (offset 0x4398/17304 of
// 65536 -- ~48KB of headroom remains after this whole block).
#define GEMDRVEMUL_SIDETNFS_CONFIG (GEMDRVEMUL_EXEC_PD + 256) // exec pd + sizeof(PD) bytes

// GET_CONFIG_INFO response block -- protocol version bumped to 2,
// fields renamed/extended from the never-committed server-list
// model (max_servers/server_count -> max_drives/drive_count, plus the new
// config_drive_letter field). All five fields are 32-bit swapped longs
// (WRITE_AND_SWAP_LONGWORD), same proven convention as .
#define GEMDRVEMUL_SIDETNFS_CONFIG_VERSION (GEMDRVEMUL_SIDETNFS_CONFIG + 0)                        // uint32_t, protocol version (3)
#define GEMDRVEMUL_SIDETNFS_CONFIG_MAX_DRIVES (GEMDRVEMUL_SIDETNFS_CONFIG_VERSION + 4)             // uint32_t, SIDETNFS_MAX_DRIVES
#define GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT (GEMDRVEMUL_SIDETNFS_CONFIG_MAX_DRIVES + 4)         // uint32_t, configured (DISABLED+ENABLED) ordinary-drive count
#define GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_LETTER (GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT + 4)       // uint32_t, config drive letter (ASCII)
#define GEMDRVEMUL_SIDETNFS_CONFIG_STATUS (GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_LETTER + 4)            // uint32_t, status code (0 = OK)
// Block ends at GEMDRVEMUL_SIDETNFS_CONFIG_STATUS + 4 (20 bytes total).

// Bumped 2 -> 3. The byte layout below is UNCHANGED (same
// offsets, same field widths) -- only the value range/meaning of the
// first drive-record field changed (DRIVE_USED: 0/1 -> DRIVE_STATE:
// 0/1/2, see sidetnfs_drive_slot_state_t in sidetnfs_config.h). That
// value-space change is incompatible in a way a byte-layout diff alone
// wouldn't show: an old client's `used=1` would silently read back as the
// new `state=1` (DISABLED), not ENABLED. Bumping the protocol version
// makes an old SIDETNFS.PRG (still checking `protocol_version == 2`) fail
// an explicit, visible mismatch instead of silently writing every drive
// back as disabled -- see report ("waarom protocolversie omhoog").
#define SIDETNFS_CONFIG_PROTOCOL_VERSION 3

// GET_DRIVE/SET_DRIVE/DELETE_DRIVE/SET_CONFIG_DRIVE/SAVE_CONFIG
// share this one block, immediately after the 20-byte GET_CONFIG_INFO
// block above. GEMDRVEMUL_SIDETNFS_DRIVE_STATUS is GET_DRIVE's status
// field AND the sole response field written by SET_DRIVE/DELETE_DRIVE/
// SET_CONFIG_DRIVE/SAVE_CONFIG (none of those four ever need the rest of
// this block at the same time). state/drive_letter/type/transport/port are
// single 16-bit words (WRITE_WORD, no swap needed -- same convention as
// e.g. GEMDRVEMUL_REENTRY_TRAP elsewhere in this file); STATUS follows the
// proven WRITE_AND_SWAP_LONGWORD 32-bit convention. String fields are
// written byte-by-byte then corrected in place with
// CHANGE_ENDIANESS_BLOCK16, the same pattern populate_dta() already uses
// for Pico->Atari string transfer. SET_DRIVE's request payload mirrors
// this exact field order (minus STATUS), read via payloadPtr the same way
// GEMDRVEMUL_FOPEN_CALL/DSETPATH_CALL already read string arguments. See
// docs/sidetnfs-config-protocol.md.
#define GEMDRVEMUL_SIDETNFS_DRIVE (GEMDRVEMUL_SIDETNFS_CONFIG_STATUS + 4)
#define GEMDRVEMUL_SIDETNFS_DRIVE_STATUS (GEMDRVEMUL_SIDETNFS_DRIVE + 0)                                // uint32_t, swapped long
// Renamed from GEMDRVEMUL_SIDETNFS_DRIVE_USED -- same offset,
// same uint16_t plain-word wire type, now carries a
// sidetnfs_drive_slot_state_t value (0 EMPTY/1 DISABLED/2 ENABLED)
// instead of a 0/1 boolean. See SIDETNFS_CONFIG_PROTOCOL_VERSION's own
// comment above for why this required a protocol version bump.
#define GEMDRVEMUL_SIDETNFS_DRIVE_STATE (GEMDRVEMUL_SIDETNFS_DRIVE_STATUS + 4)                          // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_LETTER (GEMDRVEMUL_SIDETNFS_DRIVE_STATE + 2)                          // uint16_t, plain word (ASCII)
#define GEMDRVEMUL_SIDETNFS_DRIVE_TYPE (GEMDRVEMUL_SIDETNFS_DRIVE_LETTER + 2)                           // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_TRANSPORT (GEMDRVEMUL_SIDETNFS_DRIVE_TYPE + 2)                        // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_PORT (GEMDRVEMUL_SIDETNFS_DRIVE_TRANSPORT + 2)                        // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME (GEMDRVEMUL_SIDETNFS_DRIVE_PORT + 2)                         // char[SIDETNFS_NICKNAME_LEN]
#define GEMDRVEMUL_SIDETNFS_DRIVE_HOST (GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME + SIDETNFS_NICKNAME_LEN)     // char[SIDETNFS_HOST_LEN]
#define GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH (GEMDRVEMUL_SIDETNFS_DRIVE_HOST + SIDETNFS_HOST_LEN)       // char[SIDETNFS_MOUNTPATH_LEN]
#define GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH (GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH + SIDETNFS_MOUNTPATH_LEN) // char[SIDETNFS_SDPATH_LEN]
// Block ends at GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH + SIDETNFS_SDPATH_LEN (198 bytes total).

// Round a shared-memory offset up to the next 4-byte boundary.
// Scoped name (not a generic ALIGN4) to avoid any collision with unrelated
// same-named macros elsewhere in the tree (e.g. pico-sdk's btstack ASF
// ports, never included by this build, but not worth risking).
#define SIDETNFS_NETWORK_ALIGN4(value) (((value) + 3u) & ~3u)

// GET/SET/SAVE_NETWORK_CONFIG response/request block, placed
// after the 198-byte drive record above, rounded up to the next 4-byte
// boundary (offset 17524/0x4474 of the 64KB ROM3 shared-memory window --
// ~46KB of headroom remains). Every field's offset is computed
// independently, field-by-field, exactly like the drive record above --
// never a raw struct-cast of sidetnfs_network_config_t (see
// sidetnfs_netconfig.h) onto this shared memory. GET writes every field
// below; SET reads every field except STATUS from payloadPtr in this same
// order (mirrors the GET_DRIVE/SET_DRIVE relationship); SAVE reads and
// writes only STATUS, like SAVE_CONFIG above. AUTH_MODE/USE_DHCP are plain
// 16-bit words (WRITE_WORD/GET_PAYLOAD_PARAM16, no swap -- same convention
// as the drive record's USED/LETTER/TYPE/TRANSPORT/PORT fields); the seven
// string fields use the same byte-copy + CHANGE_ENDIANESS_BLOCK16
// (Pico->Atari) / COPY_AND_CHANGE_ENDIANESS_BLOCK16 (Atari->Pico)
// convention GET_DRIVE/SET_DRIVE already use -- see that block's own
// comment for the hardware-proven rationale. Never change this swap based
// on host-only testing. See docs/sidetnfs-config-protocol.md.
//
// Alignment fix: the 198-byte drive record's own size is not a
// multiple of 4, so the network block's unrounded base was only 2-byte
// aligned -- STATUS's WRITE_AND_SWAP_LONGWORD then performed an unaligned
// 32-bit store, which Cortex-M0+ cannot do in hardware (HardFault,
// hardware-confirmed root cause). SIDETNFS_NETWORK_ALIGN4() inserts up to
// 3 padding bytes (2, in practice) so every field below -- and everything
// SET/DELETE/etc. derive from it -- is automatically safe.
#define GEMDRVEMUL_SIDETNFS_NETWORK SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH + SIDETNFS_SDPATH_LEN)
#define GEMDRVEMUL_SIDETNFS_NETWORK_STATUS (GEMDRVEMUL_SIDETNFS_NETWORK + 0)                                  // uint32_t, swapped long
#define GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE (GEMDRVEMUL_SIDETNFS_NETWORK_STATUS + 4)                        // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP (GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE + 2)                      // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_NETWORK_SSID (GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP + 2)                           // char[MAX_SSID_LENGTH] (36)
#define GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD (GEMDRVEMUL_SIDETNFS_NETWORK_SSID + MAX_SSID_LENGTH)             // char[MAX_PASSWORD_LENGTH] (68)
#define GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY (GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD + MAX_PASSWORD_LENGTH)      // char[SIDETNFS_NET_COUNTRY_LEN] (4)
#define GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS (GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY + SIDETNFS_NET_COUNTRY_LEN)   // char[IPV4_ADDRESS_LENGTH] (16)
#define GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK (GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS + IPV4_ADDRESS_LENGTH)        // char[IPV4_ADDRESS_LENGTH] (16)
#define GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY (GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK + IPV4_ADDRESS_LENGTH)           // char[IPV4_ADDRESS_LENGTH] (16)
#define GEMDRVEMUL_SIDETNFS_NETWORK_DNS (GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY + IPV4_ADDRESS_LENGTH)               // char[IPV4_ADDRESS_LENGTH] (16)
// Block ends at GEMDRVEMUL_SIDETNFS_NETWORK_DNS + IPV4_ADDRESS_LENGTH (182 bytes total incl. 2 bytes of leading padding: 4 status + 176 sidetnfs_network_config_t + 2 padding).

// Compile-time alignment/bounds guarantees for the network
// block. NETWORK_STATUS must be 4-byte aligned (the only uint32_t
// WRITE_AND_SWAP_LONGWORD field); AUTH_MODE/USE_DHCP must be 2-byte
// aligned (WRITE_WORD); every string field's byte length must be even,
// since CHANGE_ENDIANESS_BLOCK16/COPY_AND_CHANGE_ENDIANESS_BLOCK16 process
// them as whole uint16_t words; the block must fit within the 64KB ROM3
// window (ROM_SIZE_BYTES is a runtime `const uint32_t`, not usable in a
// _Static_assert, so the literal 0x10000 is asserted directly and cross-
// checked against ROM_SIZE_BYTES wherever it's read at runtime instead).
_Static_assert(GEMDRVEMUL_SIDETNFS_NETWORK_STATUS % 4 == 0, "GEMDRVEMUL_SIDETNFS_NETWORK_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE % 2 == 0, "GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP % 2 == 0, "GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP must be 2-byte aligned for WRITE_WORD");
_Static_assert(MAX_SSID_LENGTH % 2 == 0, "MAX_SSID_LENGTH must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(MAX_PASSWORD_LENGTH % 2 == 0, "MAX_PASSWORD_LENGTH must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(SIDETNFS_NET_COUNTRY_LEN % 2 == 0, "SIDETNFS_NET_COUNTRY_LEN must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(IPV4_ADDRESS_LENGTH % 2 == 0, "IPV4_ADDRESS_LENGTH must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert((GEMDRVEMUL_SIDETNFS_NETWORK_DNS + IPV4_ADDRESS_LENGTH) <= 0x10000u, "GEMDRVEMUL_SIDETNFS_NETWORK block must fit within the 64KB ROM3 window");

// GET/SET/SAVE_RTC_CONFIG response/request block ("Set Atari
// clock using NTP" / NTP server / UTC offset), placed directly after the
// network block above. GEMDRVEMUL_SIDETNFS_NETWORK_DNS + IPV4_ADDRESS_LENGTH
// (0x4528) is already 4-byte aligned, but this still goes through
// SIDETNFS_NETWORK_ALIGN4() -- same structural guarantee as the network
// block, not an assumption. Every field's offset is computed
// independently, field-by-field -- never a raw struct-cast of
// sidetnfs_rtc_config_t (see sidetnfs_rtcconfig.h) onto this shared
// memory. GET writes every field below; SET reads every field except
// STATUS from payloadPtr in this same order; SAVE reads and writes only
// STATUS. ENABLED is a plain 16-bit word (WRITE_WORD/GET_PAYLOAD_PARAM16,
// no swap -- same convention as NETWORK_AUTH_MODE/NETWORK_USE_DHCP); the
// two string fields use the same byte-copy + CHANGE_ENDIANESS_BLOCK16
// (Pico->Atari) / COPY_AND_CHANGE_ENDIANESS_BLOCK16 (Atari->Pico)
// convention the network block already uses.
#define GEMDRVEMUL_SIDETNFS_RTC SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_SIDETNFS_NETWORK_DNS + IPV4_ADDRESS_LENGTH)
#define GEMDRVEMUL_SIDETNFS_RTC_STATUS (GEMDRVEMUL_SIDETNFS_RTC + 0)                          // uint32_t, swapped long
#define GEMDRVEMUL_SIDETNFS_RTC_ENABLED (GEMDRVEMUL_SIDETNFS_RTC_STATUS + 4)                  // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_RTC_NTP_SERVER (GEMDRVEMUL_SIDETNFS_RTC_ENABLED + 2)              // char[SIDETNFS_RTC_NTP_SERVER_LEN] (64)
#define GEMDRVEMUL_SIDETNFS_RTC_UTC_OFFSET (GEMDRVEMUL_SIDETNFS_RTC_NTP_SERVER + SIDETNFS_RTC_NTP_SERVER_LEN) // char[SIDETNFS_RTC_UTC_OFFSET_LEN] (4)
// Block ends at GEMDRVEMUL_SIDETNFS_RTC_UTC_OFFSET + SIDETNFS_RTC_UTC_OFFSET_LEN (74 bytes total: 4 status + 70 sidetnfs_rtc_config_t).

// Compile-time alignment/bounds guarantees for the RTC block,
// same shape as the network block's own assertions above. RTC_STATUS
// must be 4-byte aligned (the only uint32_t WRITE_AND_SWAP_LONGWORD
// field); RTC_ENABLED must be 2-byte aligned (WRITE_WORD); both string
// fields' byte lengths must be even, since
// CHANGE_ENDIANESS_BLOCK16/COPY_AND_CHANGE_ENDIANESS_BLOCK16 process them
// as whole uint16_t words; the block must fit within the 64KB ROM3
// window; the SET request payload (everything except STATUS) must be
// exactly 70 bytes, matching sidetnfs_rtc_config_t exactly (its own
// _Static_assert in sidetnfs_rtcconfig.h cross-checks the struct side).
_Static_assert(GEMDRVEMUL_SIDETNFS_RTC_STATUS % 4 == 0, "GEMDRVEMUL_SIDETNFS_RTC_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_SIDETNFS_RTC_ENABLED % 2 == 0, "GEMDRVEMUL_SIDETNFS_RTC_ENABLED must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_SIDETNFS_RTC_NTP_SERVER % 2 == 0, "GEMDRVEMUL_SIDETNFS_RTC_NTP_SERVER must be 2-byte aligned for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(GEMDRVEMUL_SIDETNFS_RTC_UTC_OFFSET % 2 == 0, "GEMDRVEMUL_SIDETNFS_RTC_UTC_OFFSET must be 2-byte aligned for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(SIDETNFS_RTC_NTP_SERVER_LEN % 2 == 0, "SIDETNFS_RTC_NTP_SERVER_LEN must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(SIDETNFS_RTC_UTC_OFFSET_LEN % 2 == 0, "SIDETNFS_RTC_UTC_OFFSET_LEN must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert((GEMDRVEMUL_SIDETNFS_RTC_UTC_OFFSET + SIDETNFS_RTC_UTC_OFFSET_LEN) <= 0x10000u, "GEMDRVEMUL_SIDETNFS_RTC block must fit within the 64KB ROM3 window");
_Static_assert((2 + SIDETNFS_RTC_NTP_SERVER_LEN + SIDETNFS_RTC_UTC_OFFSET_LEN) == 70, "SET_RTC_CONFIG request payload (enabled + ntp_server + utc_offset) must be exactly 70 bytes");

// GEMDRVEMUL_SIDETNFS_CHECK_UPDATE (commands.h) response block --
// read-only, no request payload. STATUS is one of
// SIDETNFS_UPDATE_STATUS_UP_TO_DATE/AVAILABLE/ERROR
// (sidetnfs_update_check.h); LATEST_VERSION is only meaningful when
// STATUS is AVAILABLE or UP_TO_DATE (the version string actually read
// from version.txt, e.g. "v1.0.2"), left as an empty string on ERROR.
// INSTALLED_VERSION is always this build's own RELEASE_VERSION
// (CMakeLists.txt, from version.txt at build time) -- included so the
// caller can show "installed vs. latest" without a second command.
#define GEMDRVEMUL_SIDETNFS_UPDATE SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_SIDETNFS_RTC_UTC_OFFSET + SIDETNFS_RTC_UTC_OFFSET_LEN)
#define GEMDRVEMUL_SIDETNFS_UPDATE_STATUS (GEMDRVEMUL_SIDETNFS_UPDATE + 0)                          // uint32_t, swapped long
#define GEMDRVEMUL_SIDETNFS_UPDATE_LATEST_VERSION (GEMDRVEMUL_SIDETNFS_UPDATE_STATUS + 4)           // char[SIDETNFS_UPDATE_VERSION_LEN] (16)
#define GEMDRVEMUL_SIDETNFS_UPDATE_INSTALLED_VERSION (GEMDRVEMUL_SIDETNFS_UPDATE_LATEST_VERSION + SIDETNFS_UPDATE_VERSION_LEN) // char[SIDETNFS_UPDATE_VERSION_LEN] (16)
// Block ends at GEMDRVEMUL_SIDETNFS_UPDATE_INSTALLED_VERSION + SIDETNFS_UPDATE_VERSION_LEN (36 bytes total).

_Static_assert(GEMDRVEMUL_SIDETNFS_UPDATE_STATUS % 4 == 0, "GEMDRVEMUL_SIDETNFS_UPDATE_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_SIDETNFS_UPDATE_LATEST_VERSION % 2 == 0, "GEMDRVEMUL_SIDETNFS_UPDATE_LATEST_VERSION must be 2-byte aligned for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(GEMDRVEMUL_SIDETNFS_UPDATE_INSTALLED_VERSION % 2 == 0, "GEMDRVEMUL_SIDETNFS_UPDATE_INSTALLED_VERSION must be 2-byte aligned for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(SIDETNFS_UPDATE_VERSION_LEN % 2 == 0, "SIDETNFS_UPDATE_VERSION_LEN must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert((GEMDRVEMUL_SIDETNFS_UPDATE_INSTALLED_VERSION + SIDETNFS_UPDATE_VERSION_LEN) <= 0x10000u, "GEMDRVEMUL_SIDETNFS_UPDATE block must fit within the 64KB ROM3 window");

// Formerly GEMDRVEMUL_FLOPPY_CONFIG/GEMDRVEMUL_FLOPPY_PROFILE (FLOPPY.PRG
// server-profile config blocks, SideTNFS-Floppy-emulation project Step
// 1). Removed in the mixed-source Favorites/Carousel redesign -- the
// firmware no longer persists or knows about floppy source/mount
// profiles at all; FLOPPY.PRG owns its own Browser source configuration
// on local disk now. GEMDRVEMUL_FLOPPY_BROWSE below chains directly from
// the CHECK_UPDATE block above instead.

// FLOPPY.PRG LFN directory browser response blocks (SideTNFS-Floppy-
// emulation project, Step 2). Immediately follow the CHECK_UPDATE block
// above, same ALIGN4(previous block's own end) placement every block in
// this chain uses. See romemul/include/sidetnfs_floppy_browse.h for the
// backend/paging logic these two blocks carry the results of.
//
// GEMDRVEMUL_FLOPPY_BROWSE: response to BROWSE_OPEN/BROWSE_CHANGE_DIR --
// status + generation + the resulting CWD. BROWSE_OPEN's own request
// (backend+host+port+start_directory) travels via send_write_sync bulk
// transfer, not through this block; BROWSE_CHANGE_DIR's request
// (generation+go_up+name) is small and travels through the normal
// payload channel.
#define GEMDRVEMUL_FLOPPY_BROWSE SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_SIDETNFS_UPDATE_INSTALLED_VERSION + SIDETNFS_UPDATE_VERSION_LEN)
#define GEMDRVEMUL_FLOPPY_BROWSE_STATUS (GEMDRVEMUL_FLOPPY_BROWSE + 0)                    // uint32_t, swapped long -- sidetnfs_floppy_browse_status_t
#define GEMDRVEMUL_FLOPPY_BROWSE_GENERATION (GEMDRVEMUL_FLOPPY_BROWSE_STATUS + 4)         // uint32_t, swapped long
#define GEMDRVEMUL_FLOPPY_BROWSE_CWD (GEMDRVEMUL_FLOPPY_BROWSE_GENERATION + 4)            // char[FLOPPY_BROWSE_CWD_LEN] (256)
// Block ends at GEMDRVEMUL_FLOPPY_BROWSE_CWD + FLOPPY_BROWSE_CWD_LEN (264 bytes total).

// GEMDRVEMUL_FLOPPY_PAGE: response to GET_PAGE -- small status/metadata
// header, followed by up to FLOPPY_BROWSE_PAGE_ENTRIES (15) fixed-size
// name slots, then a parallel is_dir[] word array (Step 3: one combined
// page, dirs listed before files, GEMDRVEMUL_FLOPPY_PAGE_IS_DIR[slot]
// tells the Atari which is which -- a page can now hold both kinds at
// once, see sidetnfs_floppy_browse_get_page()'s own comment for the
// dirs-then-files walk that fills it). Only one page (the most recently
// requested one) is ever "current" at a time, since the Atari always
// fully consumes a page synchronously before requesting the next, so
// there is no concurrent-access hazard from sharing it. Publication is
// atomic from the Atari's point of view: the whole block is only ever
// read AFTER the firmware's write_random_token() at the end of the
// command handler, the same producer/consumer handshake every other
// command in this protocol already relies on -- there is no separate
// "half-written page" state the Atari could ever observe.
#define GEMDRVEMUL_FLOPPY_PAGE SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_FLOPPY_BROWSE_CWD + FLOPPY_BROWSE_CWD_LEN)
#define GEMDRVEMUL_FLOPPY_PAGE_STATUS (GEMDRVEMUL_FLOPPY_PAGE + 0)                        // uint32_t, swapped long
#define GEMDRVEMUL_FLOPPY_PAGE_GENERATION (GEMDRVEMUL_FLOPPY_PAGE_STATUS + 4)             // uint32_t, swapped long
#define GEMDRVEMUL_FLOPPY_PAGE_INDEX (GEMDRVEMUL_FLOPPY_PAGE_GENERATION + 4)              // uint32_t, swapped long -- echoes the request's page_index
#define GEMDRVEMUL_FLOPPY_PAGE_COUNT (GEMDRVEMUL_FLOPPY_PAGE_INDEX + 4)                   // uint16_t, plain word -- entries actually filled, 0..15
#define GEMDRVEMUL_FLOPPY_PAGE_HAS_PREV (GEMDRVEMUL_FLOPPY_PAGE_COUNT + 2)                // uint16_t, plain word -- 0/1
#define GEMDRVEMUL_FLOPPY_PAGE_HAS_NEXT (GEMDRVEMUL_FLOPPY_PAGE_HAS_PREV + 2)              // uint16_t, plain word -- 0/1
#define GEMDRVEMUL_FLOPPY_PAGE_RESERVED (GEMDRVEMUL_FLOPPY_PAGE_HAS_NEXT + 2)              // uint16_t, unused -- keeps ENTRIES 4-byte aligned
#define GEMDRVEMUL_FLOPPY_PAGE_ENTRIES (GEMDRVEMUL_FLOPPY_PAGE_RESERVED + 2)               // char[15][256] -- 16-byte header above, entries start 4-aligned
#define GEMDRVEMUL_FLOPPY_PAGE_IS_DIR (GEMDRVEMUL_FLOPPY_PAGE_ENTRIES + (unsigned long)FLOPPY_BROWSE_PAGE_ENTRIES * (unsigned long)FLOPPY_BROWSE_NAME_LEN) // uint16_t[15], plain word each -- 1=dir/0=file, index-matched with ENTRIES
// Block ends at GEMDRVEMUL_FLOPPY_PAGE_IS_DIR + 15*2 (30 bytes) -- header(16) + entries(15*256=3840) + is_dir(30) = 3886 bytes total.

_Static_assert(GEMDRVEMUL_FLOPPY_BROWSE_STATUS % 4 == 0, "GEMDRVEMUL_FLOPPY_BROWSE_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_BROWSE_GENERATION % 4 == 0, "GEMDRVEMUL_FLOPPY_BROWSE_GENERATION must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_BROWSE_CWD % 2 == 0, "GEMDRVEMUL_FLOPPY_BROWSE_CWD must be 2-byte aligned for CHANGE_ENDIANESS_BLOCK16");
_Static_assert((GEMDRVEMUL_FLOPPY_BROWSE_CWD + (unsigned long)FLOPPY_BROWSE_CWD_LEN) <= 0x10000u, "GEMDRVEMUL_FLOPPY_BROWSE block must fit within the 64KB ROM3 window");
_Static_assert(GEMDRVEMUL_FLOPPY_PAGE_STATUS % 4 == 0, "GEMDRVEMUL_FLOPPY_PAGE_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_PAGE_GENERATION % 4 == 0, "GEMDRVEMUL_FLOPPY_PAGE_GENERATION must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_PAGE_INDEX % 4 == 0, "GEMDRVEMUL_FLOPPY_PAGE_INDEX must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_PAGE_COUNT % 2 == 0, "GEMDRVEMUL_FLOPPY_PAGE_COUNT must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_PAGE_ENTRIES % 4 == 0, "GEMDRVEMUL_FLOPPY_PAGE_ENTRIES must be 4-byte aligned (16-byte header above it)");
_Static_assert(GEMDRVEMUL_FLOPPY_PAGE_IS_DIR % 2 == 0, "GEMDRVEMUL_FLOPPY_PAGE_IS_DIR must be 2-byte aligned for WRITE_WORD");
_Static_assert((GEMDRVEMUL_FLOPPY_PAGE_IS_DIR + (unsigned long)FLOPPY_BROWSE_PAGE_ENTRIES * 2UL) <= 0x10000u, "GEMDRVEMUL_FLOPPY_PAGE block must fit within the 64KB ROM3 window");

// BROWSE_CHANGE_DIR request payload size, excluding the 4-byte token:
// generation(4) + go_up(2) + name(FLOPPY_BROWSE_NAME_LEN) = 262 bytes.
// GET_DIR_PAGE/GET_FILE_PAGE's own request (generation(4) + page_index(4)
// = 8 bytes) is far smaller and needs no dedicated constant.
#define FLOPPY_BROWSE_CHANGE_DIR_PAYLOAD_BYTES (4UL + 2UL + (unsigned long)FLOPPY_BROWSE_NAME_LEN)
_Static_assert(FLOPPY_BROWSE_CHANGE_DIR_PAYLOAD_BYTES == 262UL, "FLOPPY_BROWSE_CHANGE_DIR_PAYLOAD_BYTES drifted from the documented request payload size");
_Static_assert(FLOPPY_BROWSE_CHANGE_DIR_PAYLOAD_BYTES <= (MAX_PROTOCOL_PAYLOAD_SIZE - 64UL), "BROWSE_CHANGE_DIR request payload must fit within the protocol's payload channel");

// ---------------------------------------------------------------------
// SideTNFS floppy emulator (Phase 2): packed Favorites session +
// floppy-emulation runtime state. Two separate blocks, one concern each
// (same convention every GEMDRVEMUL_SIDETNFS_*/FLOPPY_* block above
// already follows) -- neither is ever persisted to flash; both are pure
// ROM3 RAM, valid only for the duration of one floppy-emulation session.
// See docs/sidetnfs-floppy-protocol.md for the full command/wire writeup.
// ---------------------------------------------------------------------

// GEMDRVEMUL_FLOPPY_FAVORITES: packed Favorites/Carousel table FLOPPY.PRG
// uploads via GEMDRVEMUL_FLOPPY_FAVORITES_WRITE_CHUNK/_WRITE_CHECK/_COMMIT
// before GEMDRVEMUL_FLOPPY_SESSION_START. Deliberately NOT
// SIDETNFS_FLOPPY_FAVORITES_MAX_COUNT fixed per-entry buffers (that would
// be ~15KB for 256-byte entries, or 30KB for 512-byte ones) -- a compact
// fixed-size record per slot pointing into one shared packed-string area,
// per explicit instruction. SIDETNFS_FLOPPY_FAVORITES_STRINGS_MAX (16 KiB)
// is a deliberately generous, round budget -- NOT computed from
// MAX_COUNT * any per-path maximum -- real TNFS paths captured elsewhere
// in this project run ~40-90 bytes, so 16 KiB comfortably covers 60
// favorites (paths AND hostnames both packed into the same blob) with
// room to spare.
//
// Mixed-source redesign: each entry is now fully self-contained --
// backend + (TNFS only) host/port + path -- no source IDs, no slot
// numbers, no session-wide "active profile" concept. This is what makes a
// single Favorites list or Carousel able to mix SD and TNFS entries, and
// even multiple different TNFS servers, freely. TNFS entries always mount
// "/" on their own server (same fixed convention GEMDRVEMUL_FLOPPY_BROWSE_OPEN
// now uses) -- there is no per-entry mount path any more, only the
// complete path from that server's root.
#define SIDETNFS_FLOPPY_FAVORITES_MAX_COUNT 60u
#define SIDETNFS_FLOPPY_FAVORITE_PATH_MAX 512u        // one full TNFS/SD path, NUL included -- matches FLOPPY.PRG's own 256(dir)+256(name) favorite fields joined
#define SIDETNFS_FLOPPY_FAVORITES_STRINGS_MAX 16384u  // 16 KiB packed-string budget (fixed; see comment above)
#define SIDETNFS_FLOPPY_FAVORITES_EMPTY_OFFSET 0xFFFFu // strings-blob sentinel: no string (host_offset when backend==SD, or any offset field in an empty slot)
#define SIDETNFS_FLOPPY_FAVORITES_CHUNK_MAX 2048u     // matches the Atari driver's own BUFFER_WRITE_SIZE (gemdrive.s) -- reuses the proven WRITE_BUFF_CALL/CHECK chunk size

// Per-entry backend selector -- 0 means "empty slot" (replaces the old
// path-offset-based emptiness sentinel: emptiness is now a property of
// the whole entry, checked once via this one field, not implied by a
// string offset). Values otherwise match sidetnfs_floppy_emul.h's
// SIDETNFS_FLOPPY_SOURCE_TNFS/_SD exactly, so Pico-side code can use one
// value directly as the other with no translation.
#define SIDETNFS_FLOPPY_FAVORITE_BACKEND_EMPTY 0u

// One fixed-size table record: backend(2) + port(2, TNFS only, 0 for SD)
// + host_offset(2, into the shared strings blob, EMPTY_OFFSET for SD) +
// path_offset(2, into the shared strings blob, always valid unless the
// slot is empty). 8 bytes/entry -- small next to the 16 KiB strings
// budget even at all 60 slots (480 bytes total), and every string
// (host AND path both) still lives exactly once in the shared blob, never
// duplicated into a fixed-size per-entry buffer.
#define SIDETNFS_FLOPPY_FAVORITE_ENTRY_SIZE 8u
#define SIDETNFS_FLOPPY_FAVORITE_ENTRY_BACKEND 0u     // uint16_t, plain word, relative to this entry's own base
#define SIDETNFS_FLOPPY_FAVORITE_ENTRY_PORT 2u        // uint16_t, plain word
#define SIDETNFS_FLOPPY_FAVORITE_ENTRY_HOST_OFFSET 4u // uint16_t, plain word
#define SIDETNFS_FLOPPY_FAVORITE_ENTRY_PATH_OFFSET 6u // uint16_t, plain word

#define GEMDRVEMUL_FLOPPY_FAVORITES SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_FLOPPY_PAGE_IS_DIR + (unsigned long)FLOPPY_BROWSE_PAGE_ENTRIES * 2UL)
#define GEMDRVEMUL_FLOPPY_FAVORITES_MAGIC (GEMDRVEMUL_FLOPPY_FAVORITES + 0)                       // uint32_t, swapped long -- "FAVS"
#define GEMDRVEMUL_FLOPPY_FAVORITES_VERSION (GEMDRVEMUL_FLOPPY_FAVORITES_MAGIC + 4)               // uint32_t, swapped long (2 -- mixed-source entry format)
#define GEMDRVEMUL_FLOPPY_FAVORITES_COUNT (GEMDRVEMUL_FLOPPY_FAVORITES_VERSION + 4)               // uint16_t, plain word -- non-empty favorite count, valid only after COMMIT
#define GEMDRVEMUL_FLOPPY_FAVORITES_ACTIVE_INDEX (GEMDRVEMUL_FLOPPY_FAVORITES_COUNT + 2)          // uint16_t, plain word -- 0..59, meaningful only if COUNT > 0
#define GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS_USED (GEMDRVEMUL_FLOPPY_FAVORITES_ACTIVE_INDEX + 2)   // uint32_t, swapped long -- bytes of the packed-string area actually populated, valid only after COMMIT
#define GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_OFFSET (GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS_USED + 4)   // uint32_t, swapped long -- WRITE_CHUNK request: byte offset into the strings area this chunk lands at
#define GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_LENGTH (GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_OFFSET + 4)   // uint16_t, plain word -- WRITE_CHUNK request: bytes in this chunk, <= SIDETNFS_FLOPPY_FAVORITES_CHUNK_MAX
#define GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_CHECKSUM (GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_LENGTH + 2) // uint16_t, plain word -- WRITE_CHUNK response: Pico's own running-word checksum of the bytes it received, same algorithm/width as the existing GEMDRVEMUL_WRITE_CHK the Atari driver already computes locally and compares against, so WRITE_CHECK can reuse that exact compare-then-retry idiom
// Phase 5: repurposed from an unused alignment placeholder into the
// shared status field for WRITE_CHUNK/WRITE_CHECK/COMMIT (same
// established convention this protocol already uses elsewhere). Same
// ROM3 offset as before, so no layout change; only the meaning was ever
// "reserved".
#define GEMDRVEMUL_FLOPPY_FAVORITES_STATUS (GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_CHECKSUM + 2)       // uint32_t, swapped long -- 0 = OK, nonzero = error (also keeps TABLE 4-byte aligned; header = 28 bytes)
#define GEMDRVEMUL_FLOPPY_FAVORITES_TABLE (GEMDRVEMUL_FLOPPY_FAVORITES_STATUS + 4)                // SIDETNFS_FLOPPY_FAVORITE_ENTRY_SIZE-byte record[SIDETNFS_FLOPPY_FAVORITES_MAX_COUNT] -- see SIDETNFS_FLOPPY_FAVORITE_ENTRY_* above
#define GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS (GEMDRVEMUL_FLOPPY_FAVORITES_TABLE + (unsigned long)SIDETNFS_FLOPPY_FAVORITES_MAX_COUNT * (unsigned long)SIDETNFS_FLOPPY_FAVORITE_ENTRY_SIZE) // uint8_t[SIDETNFS_FLOPPY_FAVORITES_STRINGS_MAX] -- NUL-terminated host/path strings, packed consecutively
// Block ends at GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS + SIDETNFS_FLOPPY_FAVORITES_STRINGS_MAX
// (28 + 480 + 16384 = 16892 bytes total).

_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_MAGIC % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_MAGIC must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_VERSION % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_VERSION must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_COUNT % 2 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_COUNT must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_ACTIVE_INDEX % 2 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_ACTIVE_INDEX must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS_USED % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS_USED must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_OFFSET % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_OFFSET must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_LENGTH % 2 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_LENGTH must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_CHECKSUM % 2 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_CHUNK_CHECKSUM must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_STATUS % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_TABLE % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_TABLE must be 4-byte aligned (header above it is exactly 28 bytes)");
_Static_assert(SIDETNFS_FLOPPY_FAVORITE_ENTRY_SIZE % 4 == 0, "SIDETNFS_FLOPPY_FAVORITE_ENTRY_SIZE must be 4-byte aligned so every entry in the table stays 4-byte aligned too");
_Static_assert(GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS % 4 == 0, "GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS must be 4-byte aligned (table above it is SIDETNFS_FLOPPY_FAVORITES_MAX_COUNT * SIDETNFS_FLOPPY_FAVORITE_ENTRY_SIZE bytes)");
_Static_assert(SIDETNFS_FLOPPY_FAVORITE_PATH_MAX % 2 == 0, "SIDETNFS_FLOPPY_FAVORITE_PATH_MAX must be even for CHANGE_ENDIANESS_BLOCK16");
_Static_assert((GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS + (unsigned long)SIDETNFS_FLOPPY_FAVORITES_STRINGS_MAX) <= 0x10000u, "GEMDRVEMUL_FLOPPY_FAVORITES block must fit within the 64KB ROM3 window");

// GEMDRVEMUL_FLOPPY_SESSION: mounted image/geometry, current favorite,
// sector I/O staging, the requested Atari BOOT configuration, and the
// long-SELECT exit handshake. Never persisted; valid only while a floppy
// session is prepared/active.
//
// IMPORTANT (Phase 2B correction): the Pico firmware itself never has a
// GEMDRIVE-vs-FLOPPY "mode" -- it always runs the same single firmware
// image, always ready to answer both the GEMDOS-relay command set and
// this floppy-session command set. What varies is only which
// FUNCTIONALITY THE ATARI'S OWN BOOT-TIME CODE INSTALLS after the next
// reset, controlled by two independent flags the Atari's cartridge-init
// reads at boot:
//   INSTALL_GEMDRIVE -- install the GEMDOS-relay driver (TNFS/SD drives)
//   INSTALL_FLOPPY    -- install the hdv_bpb/hdv_rw/hdv_mediach/XBIOS
//                        floppy hooks for virtual drive A: or B: (see
//                        DRIVE_NUMBER below -- exactly one, never both)
// All four combinations are valid (YES/NO = normal SideTNFS, NO/YES =
// clean floppy-only boot, NO/NO = neither installed, YES/YES = both).
// YES/NO is the mandatory fail-safe default, set in RAM on every Pico
// power-cycle (never read from or written to flash -- these are pure
// session/launch state, owned by FLOPPY.PRG for the duration of one
// requested session) and restored on a long-SELECT exit
// (floppy_select_trigger_exit() in gemdrvemul.c) -- the user then presses
// Atari RESET manually; see RESET_REQUESTED's own comment for why this
// isn't automatic.
//
// EXIT_ACK_SEEN: reserved, unused field. An earlier design had an
// Atari-side VBL callback poll a "reset requested" flag and trigger an
// automatic warm reset, acknowledged via GEMDRVEMUL_FLOPPY_EXIT_ACK.
// Abandoned on real hardware -- TOS's own one-time VBL-queue setup
// (nvbls/vblqueue) turned out to run later, and less predictably, than
// any tested trigger point could reliably wait for (boot-time install,
// first real Getbpb, first real Mediach all still saw an uninitialized
// queue). Manual Atari RESET is the current, intentional design. Left
// defined rather than removed/renumbered to avoid reshuffling the
// working ROM3 layout for no benefit -- a future feature could still
// reclaim it, but nothing does today. Its neighbor (the old
// RESET_REQUESTED slot) has already been reclaimed this way, for
// DRIVE_NUMBER below.
//
// DRIVE_NUMBER (reclaimed from the former RESET_REQUESTED slot, same
// offset -- see above): which Atari floppy unit INSTALL_FLOPPY emulates,
// 0 = drive A: (the default, matching this block's own power-cycle
// default policy), 1 = drive B:. The user picks A or B exclusively, never
// both, in the Carousel's Start dialog -- floppy.s's hdv_bpb/hdv_rw/
// hdv_mediach/XBIOS hooks compare the caller's disk_number against this
// field instead of a hardcoded 0.
#define GEMDRVEMUL_FLOPPY_SESSION SIDETNFS_NETWORK_ALIGN4(GEMDRVEMUL_FLOPPY_FAVORITES_STRINGS + (unsigned long)SIDETNFS_FLOPPY_FAVORITES_STRINGS_MAX)
#define GEMDRVEMUL_FLOPPY_SESSION_STATUS (GEMDRVEMUL_FLOPPY_SESSION + 0)                          // uint32_t, swapped long -- status/error code, 0 = OK
#define GEMDRVEMUL_FLOPPY_SESSION_GENERATION (GEMDRVEMUL_FLOPPY_SESSION_STATUS + 4)               // uint32_t, swapped long -- bumped by SESSION_START, lets a stale READ_SECTOR response be detected
// Formerly GEMDRVEMUL_FLOPPY_SESSION_ACTIVE_SLOT (which of the 8 TNFS/SD
// source profiles the whole session used) -- removed in the mixed-source
// redesign: there is no session-wide source any more, each Favorite/
// Carousel entry carries its own backend+host+port (see
// GEMDRVEMUL_FLOPPY_FAVORITES_TABLE's SIDETNFS_FLOPPY_FAVORITE_ENTRY_*
// fields above).
#define GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE (GEMDRVEMUL_FLOPPY_SESSION_GENERATION + 4)    // uint16_t, plain word -- 0=NO/1=YES, requested Atari boot: install the GEMDOS-relay driver. Power-cycle default (and long-SELECT-exit restore value) is 1 (YES).
#define GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY (GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE + 2) // uint16_t, plain word -- 0=NO/1=YES, requested Atari boot: install the floppy hdv_*/XBIOS hooks. Power-cycle default (and long-SELECT-exit restore value) is 0 (NO).
#define GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER (GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY + 2)      // uint16_t, plain word -- 0=drive A:/1=drive B:, which unit INSTALL_FLOPPY emulates. Reclaimed from the former RESET_REQUESTED slot (same offset) -- see this block's own comment above GEMDRVEMUL_FLOPPY_SESSION's #define.
#define GEMDRVEMUL_FLOPPY_SESSION_EXIT_ACK_SEEN (GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER + 2)       // uint16_t, plain word -- RESERVED, UNUSED. Was: set once GEMDRVEMUL_FLOPPY_EXIT_ACK was received. Same rationale as the abandoned automatic-reset design this block's own comment describes.
#define GEMDRVEMUL_FLOPPY_SESSION_IMAGE_PATH (GEMDRVEMUL_FLOPPY_SESSION_EXIT_ACK_SEEN + 2)        // char[SIDETNFS_FLOPPY_FAVORITE_PATH_MAX] -- full path of the mounted image
// SIDES/SECTORS_PER_TRACK/TRACKS are uint16_t, not uint8_t (Phase 3
// correction) -- this codebase's shared-memory write macros
// (WRITE_WORD/WRITE_AND_SWAP_LONGWORD, memfunc.h) only ever operate on
// 16- or 32-bit granularity; no single-byte ROM3 write primitive exists
// anywhere in this protocol, and none of this block's values need more
// than a byte of range, so matching the established word-only convention
// costs 3 bytes and avoids inventing a new write helper nothing else
// uses.
#define GEMDRVEMUL_FLOPPY_SESSION_SIDES (GEMDRVEMUL_FLOPPY_SESSION_IMAGE_PATH + (unsigned long)SIDETNFS_FLOPPY_FAVORITE_PATH_MAX) // uint16_t, plain word -- 1 or 2
#define GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK (GEMDRVEMUL_FLOPPY_SESSION_SIDES + 2)          // uint16_t, plain word -- 9, 10 or 11
#define GEMDRVEMUL_FLOPPY_SESSION_TRACKS (GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK + 2)         // uint16_t, plain word -- 80..85, derived from file size (req #2), not trusted from the BPB alone
#define GEMDRVEMUL_FLOPPY_SESSION_BYTES_PER_SECTOR (GEMDRVEMUL_FLOPPY_SESSION_TRACKS + 2)          // uint16_t, plain word -- always NUM_BYTES_PER_SECTOR (512), validated not assumed (req #2)
#define GEMDRVEMUL_FLOPPY_SESSION_CURRENT_FAVORITE (GEMDRVEMUL_FLOPPY_SESSION_BYTES_PER_SECTOR + 2) // uint16_t, plain word -- index into the FAVORITES table, drives short-SELECT switching
#define GEMDRVEMUL_FLOPPY_SESSION_RESERVED2 (GEMDRVEMUL_FLOPPY_SESSION_CURRENT_FAVORITE + 2)       // uint16_t, unused -- keeps SECTOR_LBA 4-byte aligned
// Phase 4 correction: SECTOR_LBA is RESPONSE-only (an echo of the LBA the
// Pico actually served), not a request field -- ROM3 is not Atari-writable
// on real hardware (confirmed Phase 3B/gemdrive-85: every Atari->Pico byte
// is an address-encoded read, never a plain store), so the Atari cannot
// pre-populate this field before sending GEMDRVEMUL_FLOPPY_READ_SECTOR.
// The actual LBA travels as the command's own small payload (matching
// every other GEMDRVEMUL_* request), read via GET_PAYLOAD_PARAM32() in
// the dispatch handler -- see gemdrvemul.c's own case comment.
#define GEMDRVEMUL_FLOPPY_SESSION_SECTOR_LBA (GEMDRVEMUL_FLOPPY_SESSION_RESERVED2 + 2)             // uint32_t, swapped long -- READ_SECTOR response: echoes the LBA just served, for the Atari's own desync sanity-check only
#define GEMDRVEMUL_FLOPPY_SESSION_SECTOR_DATA (GEMDRVEMUL_FLOPPY_SESSION_SECTOR_LBA + 4)           // uint8_t[NUM_BYTES_PER_SECTOR] -- READ_SECTOR response: the 512-byte sector payload
// Phase 4: the Atari-side hdv_bpb/hdv_rw/hdv_mediach hooks must fall
// through to whatever vector was installed before ours for any
// disk_number != 0 (drive B:, hard disks, ...). Since ROM4 (where the
// driver's own code/data lives) is likewise not Atari-writable (confirmed
// by this project's own existing GEMDOS-trap install: see gemdrive.s's
// old_handler comment, "we can't modify this address because it's in
// ROM, but we can modify it in the RP2040 memory"), the three old vector
// values are sent to the Pico ONCE at install time
// (GEMDRVEMUL_FLOPPY_SAVE_VECTORS) and stored here -- every subsequent
// fall-through is then a fast, ordinary LOCAL ROM3 read on the Atari
// side, no command round-trip per disk access.
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB (GEMDRVEMUL_FLOPPY_SESSION_SECTOR_DATA + (unsigned long)NUM_BYTES_PER_SECTOR)     // uint32_t, swapped long -- original hdv_bpb vector, saved at install
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW (GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB + 4)           // uint32_t, swapped long -- original hdv_rw vector, saved at install
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH (GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW + 4)       // uint32_t, swapped long -- original hdv_mediach vector, saved at install
#define GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR (GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH + 4) // uint32_t, swapped long -- the floppy XBIOS trap's OWN chain-through value (separate from GEMDRIVE's own GEMDRVEMUL_OLD_XBIOS-style slot elsewhere -- each installer only ever touches its own field, no collision even when both install in the YES/YES case)
// Phase 4: the in-memory GEMDOS BPB record hdv_bpb must return a pointer
// to (9 words: recsize/clsiz/clsizb/rdlen/fsiz/fatrec/datrec/numcl/bflags
// -- the standard Atari BPB shape, NOT the same layout as the on-disk
// boot-sector BPB bytes 11-29 used for Phase 3's own geometry validation).
// Built by SESSION_START from the validated image's own on-disk BPB
// fields (re-read via the existing, unmodified
// sidetnfs_floppy_emul_read_sector() public API -- no backend change),
// published here so the Atari's hdv_bpb can just return a pointer
// straight into this ROM3 field -- it never needs to construct or store
// the struct itself, sidestepping the same ROM4-not-writable constraint.
#define GEMDRVEMUL_FLOPPY_SESSION_BPB (GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR + 4)             // uint16_t[9], plain words -- recsize,clsiz,clsizb,rdlen,fsiz,fatrec,datrec,numcl,bflags
#define GEMDRVEMUL_FLOPPY_SESSION_BPB_WORDS 9u
// Phase 4A: proper media-change state, replacing the earlier "always
// report 2" placeholder. 0 = unchanged, nonzero = a new image was just
// mounted (SESSION_START) or (future Phase 6) a short-SELECT favorite
// switch happened -- the Atari's mediach hook reports 2 exactly once
// then sends GEMDRVEMUL_FLOPPY_MEDIA_CHANGE_ACK to clear it back to 0,
// matching how a real floppy's disk-change line behaves (asserted once
// per actual change, not held forever). Deliberately a plain ROM3 field,
// not Atari-side RAM -- ROM4 is not Atari-writable, same constraint as
// everywhere else in this protocol.
#define GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED (GEMDRVEMUL_FLOPPY_SESSION_BPB + (unsigned long)GEMDRVEMUL_FLOPPY_SESSION_BPB_WORDS * 2UL) // uint16_t, plain word
// Block ends at GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED + 2
// (4+4+4 + 2+2+2+2 + 512 + 2+2+2+2+2+2 + 4 + 512 + 4+4+4+4 + 18 + 2 = 1102 bytes total).

_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_STATUS % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_STATUS must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_GENERATION % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_GENERATION must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_INSTALL_GEMDRIVE must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_INSTALL_FLOPPY must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_EXIT_ACK_SEEN % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_EXIT_ACK_SEEN must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_SIDES % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_SIDES must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_SECTORS_PER_TRACK must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_TRACKS % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_TRACKS must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_IMAGE_PATH % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_IMAGE_PATH must be 2-byte aligned for CHANGE_ENDIANESS_BLOCK16");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_BYTES_PER_SECTOR % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_BYTES_PER_SECTOR must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_CURRENT_FAVORITE % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_CURRENT_FAVORITE must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_SECTOR_LBA % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_SECTOR_LBA must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_BPB must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_RW must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_MEDIACH must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR % 4 == 0, "GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR must be 4-byte aligned for WRITE_AND_SWAP_LONGWORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_BPB % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_BPB must be 2-byte aligned for WRITE_WORD");
_Static_assert(GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED % 2 == 0, "GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED must be 2-byte aligned for WRITE_WORD");
_Static_assert((GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED + 2UL) <= 0x10000u, "GEMDRVEMUL_FLOPPY_SESSION block must fit within the 64KB ROM3 window");

// Atari ST FATTRIB flag
#define FATTRIB_INQUIRE 0x00
#define FATTRIB_SET 0x01

// Atari ST FDATETIME flag
#define FDATETIME_INQUIRE 0x00
#define FDATETIME_SET 0x01

// Atari ST GEMDOS error codes
#define GEMDOS_EOK 0       // OK
#define GEMDOS_ERROR -1    // Generic error
#define GEMDOS_EDRVNR -2   // Drive not ready
#define GEMDOS_EUNCMD -3   // Unknown command
#define GEMDOS_E_CRC -4    // CRC error
#define GEMDOS_EBADRQ -5   // Bad request
#define GEMDOS_E_SEEK -6   // Seek error
#define GEMDOS_EMEDIA -7   // Unknown media
#define GEMDOS_ESECNF -8   // Sector not found
#define GEMDOS_EPAPER -9   // Out of paper
#define GEMDOS_EWRITF -10  // Write fault
#define GEMDOS_EREADF -11  // Read fault
#define GEMDOS_EWRPRO -13  // Device is write protected
#define GEMDOS_E_CHNG -14  // Media change detected
#define GEMDOS_EUNDEV -15  // Unknown device
#define GEMDOS_EINVFN -32  // Invalid function
#define GEMDOS_EFILNF -33  // File not found
#define GEMDOS_EPTHNF -34  // Path not found
#define GEMDOS_ENHNDL -35  // No more handles
#define GEMDOS_EACCDN -36  // Access denied
#define GEMDOS_EIHNDL -37  // Invalid handle
#define GEMDOS_ENSMEM -39  // Insufficient memory
#define GEMDOS_EIMBA -40   // Invalid memory block address
#define GEMDOS_EDRIVE -46  // Invalid drive specification
#define GEMDOS_ENSAME -48  // Cross device rename
#define GEMDOS_ENMFIL -49  // No more files
#define GEMDOS_ELOCKED -58 // Record is already locked
#define GEMDOS_ENSLOCK -59 // Invalid lock removal request
#define GEMDOS_ERANGE -64  // Range error
#define GEMDOS_EINTRN -65  // Internal error
#define GEMDOS_EPLFMT -66  // Invalid program load format
#define GEMDOS_EGSBF -67   // Memory block growth failure
#define GEMDOS_ELOOP -80   // Too many symbolic links
#define GEMDOS_EMOUNT -200 // Mount point crossed (indicator)

#define DTA_HASH_TABLE_SIZE 512

#define PDCLSIZE 0x80 /*  size of command line in bytes  */
#define MAXDEVS 16    /* max number of block devices */

typedef struct
{
    /* No. of Free Clusters */
    uint32_t b_free;
    /* Clusters per Drive */
    uint32_t b_total;
    /* Bytes per Sector */
    uint32_t b_secsize;
    /* Sectors per Cluster */
    uint32_t b_clsize;
} TOS_DISKINFO;

typedef struct
{
    char d_name[12];         /* file name: filename.typ     00-11   */
    uint32_t d_offset_drive; /* dir position                12-15   */
    uint16_t d_curbyt;       /* byte pointer within current cluster 16-17 */
    uint16_t d_curcl;        /* current cluster number for file	   18-19 */
    uint8_t d_attr;          /* attributes of file          20      */
    uint8_t d_attrib;        /* attributes of f file 21 */
    uint16_t d_time;         /* time from file date 22-23 */
    uint16_t d_date;         /* date from file date 24-25 */
    uint32_t d_length;       /* file length in bytes 26-29 */
    char d_fname[14];        /* file name: filename.typ 30-43 */
} DTA;

typedef struct DTANode
{
    uint32_t key;
    uint32_t attribs;
    DTA data;
    DIR *dj;
    FILINFO *fno;
    TCHAR *pat; /* Pointer to the name matching pattern. Hack for dir_findfirst().  */
    struct DTANode *next;
} DTANode;

// Which backend actually opened this descriptor. fobject is only
// ever populated/touched for GEMDRIVE_FILE_BACKEND_SD; tnfs_handle only for
// GEMDRIVE_FILE_BACKEND_TNFS. Routing itself stays compile-time
// (SIDETNFS_USE_TNFS_LISTING, like every other gemdrive_backend_* helper in
// gemdrvemul.c) -- this tag is an additional runtime confirmation checked
// before any TNFS-specific field is read, not the routing mechanism itself.
typedef enum
{
    GEMDRIVE_FILE_BACKEND_SD = 0,
    GEMDRIVE_FILE_BACKEND_TNFS,
    // Read-only, root-only virtual drive serving the
    // flash-embedded SIDETNFS.PRG/README.TXT directly from their const
    // arrays -- see romemul/sidetnfs_config_drive_backend.c. This is the
    // always-present SETTINGS disk's backend (sidetnfs_runtime_drives_init()),
    // never gated by a build-time switch.
    GEMDRIVE_FILE_BACKEND_CONFIG_FLASH,
    // Read-only, root-only virtual
    // file for an ENABLED TNFS drive whose backend isn't ready right now
    // (see sidetnfs_probe_classify_slot_error()) -- generated fresh at
    // Fopen time into FileDescriptors.net_err_text, then served exactly
    // like GEMDRIVE_FILE_BACKEND_CONFIG_FLASH's generic offset/size
    // buffer contract for Fread/Fseek/Fclose (config_flash_data points at
    // this descriptor's own net_err_text, config_flash_size at its
    // generated length) -- kept as its own distinct tag rather than
    // reusing GEMDRIVE_FILE_BACKEND_CONFIG_FLASH so SETTINGS and this
    // virtual file are never conflated (diagnostics, future maintenance).
    GEMDRIVE_FILE_BACKEND_NET_ERR,
    // Read-only, root-only virtual file
    // for an ENABLED SD drive whose backend isn't READY right now (see
    // sidetnfs_sd_get_drive_status()) -- generated fresh at Fopen time
    // into FileDescriptors.sd_error_text, served via the exact same
    // generic offset/size buffer contract as CONFIG_FLASH/NET_ERR. Its
    // own distinct tag, never reused from NET_ERR/CONFIG_FLASH -- SD is
    // never routed through the TNFS-error/SETTINGS machinery.
    GEMDRIVE_FILE_BACKEND_SD_ERROR
} GemdriveFileBackend;

typedef struct FileDescriptors
{
    char fpath[128];
    int fd;
    FIL fobject;
    struct FileDescriptors *next;
    uint32_t offset;
    GemdriveFileBackend backend;
    uint8_t tnfs_handle; // valid only when backend == GEMDRIVE_FILE_BACKEND_TNFS
    // Whether this handle was opened for writing (Fopen mode 1/2,
    // or Fcreate). Only meaningful/checked for GEMDRIVE_FILE_BACKEND_TNFS --
    // the SD/FatFS backend already enforces this itself (f_write() on an
    // FA_READ-only FIL returns FR_DENIED), so no equivalent check is added
    // there. Lets GEMDRVEMUL_WRITE_BUFF_CALL deny a write to a read-only
    // TNFS handle locally, before ever contacting the server.
    bool tnfs_writable;
    // The runtime slot (0=N:,
    // 1=O:, ...) this handle's TNFS session belongs to -- set once at
    // Fopen/Fcreate time (see gemdrive_backend_fopen()/
    // GEMDRVEMUL_FCREATE_CALL) and read back by every later call that
    // needs this handle's own host/port/session_id (Fread/Fwrite/Fseek/
    // Fclose/Fdatime
    // this is the field they will read from once they are). Meaningful
    // only when backend == GEMDRIVE_FILE_BACKEND_TNFS.
    int runtime_slot;
    // Direct pointer into the existing const flash
    // array (sidetnfs_config_prg/sidetnfs_config_readme) -- never a copy.
    // Valid only when backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH.
    // Also reused (unchanged meaning: base pointer + size for a
    // plain offset-bounded read) for GEMDRIVE_FILE_BACKEND_NET_ERR --
    // there config_flash_data always points at this SAME descriptor's own
    // net_err_text[] below (set once at Fopen time), never a shared/
    // static buffer, so concurrently open NET_ERR.TXT handles (e.g. two
    // failing drives, test D) never alias each other.
    const uint8_t *config_flash_data;
    uint32_t config_flash_size;
    // This handle's own generated
    // body text, filled in once at Fopen time by
    // sidetnfs_build_net_err_text() -- valid only when
    // backend == GEMDRIVE_FILE_BACKEND_NET_ERR.
    char net_err_text[SIDETNFS_NET_ERR_TEXT_MAX];
    // This handle's own generated body text,
    // filled in once at Fopen time by sidetnfs_build_sd_error_text() --
    // valid only when backend == GEMDRIVE_FILE_BACKEND_SD_ERROR.
    // config_flash_data/config_flash_size are reused for this backend too
    // (pointed at THIS descriptor's own sd_error_text -- never a shared
    // buffer, so two concurrently failing SD drives never alias, same
    // reasoning as net_err_text above).
    char sd_error_text[SIDETNFS_SD_ERROR_TEXT_MAX];
} FileDescriptors;

typedef struct _pd PD;
struct _pd
{
    /* 0x00 */
    char *p_lowtpa;  /* pointer to start of TPA */
    char *p_hitpa;   /* pointer to end of TPA+1 */
    char *p_tbase;   /* pointer to base of text segment */
    uint32_t p_tlen; /* length of text segment */

    /* 0x10 */
    char *p_dbase;   /* pointer to base of data segment */
    uint32_t p_dlen; /* length of data segment */
    char *p_bbase;   /* pointer to base of bss segment */
    uint32_t p_blen; /* length of bss segment */

    /* 0x20 */
    DTA *p_xdta;
    PD *p_parent;      /* parent PD */
    uint32_t p_hflags; /* see below */
    char *p_env;       /* pointer to environment string */

    /* 0x30 */
    uint32_t p_1fill[2]; /* (junk) */
    uint16_t p_curdrv;   /* current drive */
    uint16_t p_uftsize;  /* number of OFD pointers at p_uft */
    void **p_uft;        /* ptr to my uft (allocated after env.) */

    /* 0x40 */
    uint p_curdir[MAXDEVS]; /* startcl of cur dir on each drive */

    /* 0x60 */
    ulong p_3fill[2]; /* (junk) */
    ulong p_dreg[1];  /* dreg[0] */
    ulong p_areg[5];  /* areg[3..7] */

    /* 0x80 */
    char p_cmdlin[PDCLSIZE]; /* command line image */
};

typedef struct ExecHeader
{
    uint16_t magic;
    uint16_t text_h;
    uint16_t text_l;
    uint16_t data_h;
    uint16_t data_l;
    uint16_t bss_h;
    uint16_t bss_l;
    uint16_t syms_h;
    uint16_t syms_l;
    uint16_t reserved1_h;
    uint16_t reserved1_l;
    uint16_t prgflags_h;
    uint16_t prgflags_l;
    uint16_t absflag;
} ExecHeader;

typedef void (*IRQInterceptionCallback)();

extern int read_addr_rom_dma_channel;
extern int lookup_data_rom_dma_channel;

// Interrupt handler callback for DMA completion
void __not_in_flash_func(gemdrvemul_dma_irq_handler_lookup_callback)(void);

// Function Prototypes
void init_gemdrvemul(bool safe_config_reboot);

#endif // GEMDRVEMUL_H
