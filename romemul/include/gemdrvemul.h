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
#include "hardware/rtc.h"

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

// Fase 1 (multi-drive, ROM side only in this phase): mirrors
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

// Fase 9B1: start of the SIDETNFS config-protocol response block. Placed
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

// Fase 9C: GET_CONFIG_INFO response block -- protocol version bumped to 2,
// fields renamed/extended from the never-committed Fase 9B2 server-list
// model (max_servers/server_count -> max_drives/drive_count, plus the new
// config_drive_letter field). All five fields are 32-bit swapped longs
// (WRITE_AND_SWAP_LONGWORD), same proven convention as Fase 9B1/9B2.
#define GEMDRVEMUL_SIDETNFS_CONFIG_VERSION (GEMDRVEMUL_SIDETNFS_CONFIG + 0)                        // uint32_t, protocol version (2)
#define GEMDRVEMUL_SIDETNFS_CONFIG_MAX_DRIVES (GEMDRVEMUL_SIDETNFS_CONFIG_VERSION + 4)             // uint32_t, SIDETNFS_MAX_DRIVES
#define GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT (GEMDRVEMUL_SIDETNFS_CONFIG_MAX_DRIVES + 4)         // uint32_t, used ordinary-drive count
#define GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_LETTER (GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT + 4)       // uint32_t, config drive letter (ASCII)
#define GEMDRVEMUL_SIDETNFS_CONFIG_STATUS (GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_LETTER + 4)            // uint32_t, status code (0 = OK)
// Block ends at GEMDRVEMUL_SIDETNFS_CONFIG_STATUS + 4 (20 bytes total).

#define SIDETNFS_CONFIG_PROTOCOL_VERSION 2

// Fase 9C: GET_DRIVE/SET_DRIVE/DELETE_DRIVE/SET_CONFIG_DRIVE/SAVE_CONFIG
// share this one block, immediately after the 20-byte GET_CONFIG_INFO
// block above. GEMDRVEMUL_SIDETNFS_DRIVE_STATUS is GET_DRIVE's status
// field AND the sole response field written by SET_DRIVE/DELETE_DRIVE/
// SET_CONFIG_DRIVE/SAVE_CONFIG (none of those four ever need the rest of
// this block at the same time). used/drive_letter/type/transport/port are
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
#define GEMDRVEMUL_SIDETNFS_DRIVE_USED (GEMDRVEMUL_SIDETNFS_DRIVE_STATUS + 4)                           // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_LETTER (GEMDRVEMUL_SIDETNFS_DRIVE_USED + 2)                           // uint16_t, plain word (ASCII)
#define GEMDRVEMUL_SIDETNFS_DRIVE_TYPE (GEMDRVEMUL_SIDETNFS_DRIVE_LETTER + 2)                           // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_TRANSPORT (GEMDRVEMUL_SIDETNFS_DRIVE_TYPE + 2)                        // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_PORT (GEMDRVEMUL_SIDETNFS_DRIVE_TRANSPORT + 2)                        // uint16_t, plain word
#define GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME (GEMDRVEMUL_SIDETNFS_DRIVE_PORT + 2)                         // char[SIDETNFS_NICKNAME_LEN]
#define GEMDRVEMUL_SIDETNFS_DRIVE_HOST (GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME + SIDETNFS_NICKNAME_LEN)     // char[SIDETNFS_HOST_LEN]
#define GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH (GEMDRVEMUL_SIDETNFS_DRIVE_HOST + SIDETNFS_HOST_LEN)       // char[SIDETNFS_MOUNTPATH_LEN]
#define GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH (GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH + SIDETNFS_MOUNTPATH_LEN) // char[SIDETNFS_SDPATH_LEN]
// Block ends at GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH + SIDETNFS_SDPATH_LEN (198 bytes total).

// Fase 11C: round a shared-memory offset up to the next 4-byte boundary.
// Scoped name (not a generic ALIGN4) to avoid any collision with unrelated
// same-named macros elsewhere in the tree (e.g. pico-sdk's btstack ASF
// ports, never included by this build, but not worth risking).
#define SIDETNFS_NETWORK_ALIGN4(value) (((value) + 3u) & ~3u)

// Fase 11A/11C: GET/SET/SAVE_NETWORK_CONFIG response/request block, placed
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
// Fase 11C alignment fix: the 198-byte drive record's own size is not a
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

// Fase 11C: compile-time alignment/bounds guarantees for the network
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

// Fase 12A: GET/SET/SAVE_RTC_CONFIG response/request block ("Set Atari
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

// Fase 12A: compile-time alignment/bounds guarantees for the RTC block,
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

// Fase 7D: which backend actually opened this descriptor. fobject is only
// ever populated/touched for GEMDRIVE_FILE_BACKEND_SD; tnfs_handle only for
// GEMDRIVE_FILE_BACKEND_TNFS. Routing itself stays compile-time
// (SIDETNFS_USE_TNFS_LISTING, like every other gemdrive_backend_* helper in
// gemdrvemul.c) -- this tag is an additional runtime confirmation checked
// before any TNFS-specific field is read, not the routing mechanism itself.
typedef enum
{
    GEMDRIVE_FILE_BACKEND_SD = 0,
    GEMDRIVE_FILE_BACKEND_TNFS,
    // Fase 10B: read-only, root-only virtual drive serving the Fase 10A
    // flash-embedded SIDETNFS.PRG/README.TXT directly from their const
    // arrays -- see romemul/sidetnfs_config_drive_backend.c. Only reached
    // when SIDETNFS_CONFIG_DRIVE_ONLY (compile-time, default 0) selects it
    // as the sole GEMDRIVE backend for a temporary test build.
    GEMDRIVE_FILE_BACKEND_CONFIG_FLASH
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
    // Fase 7K: whether this handle was opened for writing (Fopen mode 1/2,
    // or Fcreate). Only meaningful/checked for GEMDRIVE_FILE_BACKEND_TNFS --
    // the SD/FatFS backend already enforces this itself (f_write() on an
    // FA_READ-only FIL returns FR_DENIED), so no equivalent check is added
    // there. Lets GEMDRVEMUL_WRITE_BUFF_CALL deny a write to a read-only
    // TNFS handle locally, before ever contacting the server.
    bool tnfs_writable;
    // Fase 10 (Fopen/Fcreate slot-aware fix): the runtime slot (0=N:,
    // 1=O:, ...) this handle's TNFS session belongs to -- set once at
    // Fopen/Fcreate time (see gemdrive_backend_fopen()/
    // GEMDRVEMUL_FCREATE_CALL) and read back by every later call that
    // needs this handle's own host/port/session_id (Fread/Fwrite/Fseek/
    // Fclose/Fdatime -- not changed in this phase, still slot-0-only, but
    // this is the field they will read from once they are). Meaningful
    // only when backend == GEMDRIVE_FILE_BACKEND_TNFS.
    int runtime_slot;
    // Fase 10B: direct pointer into the existing Fase 10A const flash
    // array (sidetnfs_config_prg/sidetnfs_config_readme) -- never a copy.
    // Valid only when backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH.
    const uint8_t *config_flash_data;
    uint32_t config_flash_size;
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

// Fase 10B-afronding: true once main()'s cyw43_arch_init() has actually
// succeeded this boot (see main.c) -- defined there, not here. Any
// cyw43_arch_deinit() call site should check this instead of assuming init
// always ran first.
bool sidetnfs_cyw43_arch_is_ready(void);

#endif // GEMDRVEMUL_H
