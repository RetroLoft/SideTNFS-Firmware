/**
 * File: rtcemul.h
 * Author: Diego Parrilla Santamaría
 * Date: July 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: Header file for the RTC emulator C program.
 */

#ifndef RTCEMUL_H
#define RTCEMUL_H

#include "debug.h"
#include "constants.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include <hardware/watchdog.h>
#include "hardware/structs/bus_ctrl.h"
#include "pico/cyw43_arch.h"
#include "hardware/rtc.h"

#include "time.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "sd_card.h"
#include "f_util.h"
#include "ff.h"

#include "../../build/romemul.pio.h"

#include "tprotocol.h"
#include "commands.h"
#include "config.h"
#include "network.h"
#include "filesys.h"

// Fase 12: the RTCEMUL_* shared-memory offsets belonged to the standalone
// RTC emulator's own protocol and are gone with it. GEMDRIVE uses its own
// GEMDRVEMUL_* layout (see gemdrvemul.h), which is untouched.

#define NTP_DEFAULT_PORT 123 // NTP UDP port
#define NTP_DELTA 2208988800 // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_MSG_LEN 48       // ignore Authenticator (optional)

typedef struct NTP_TIME_T
{
    ip_addr_t ntp_ipaddr;
    struct udp_pcb *ntp_pcb;
    bool ntp_server_found;
    bool ntp_error;
} NTP_TIME;

// Fase 12: RTC_TYPE, DallasClock, the IRQInterceptionCallback/DMA-channel
// plumbing, rtcemul_dma_irq_handler_lookup_callback() and init_rtcemul()
// are removed with the standalone RTC emulator. What remains is only the
// NTP / Pico-RTC / Atari-time helper API the GEMDRIVE flow calls.
void host_found_callback(const char *name, const ip_addr_t *ipaddr, void *arg);
void set_internal_rtc();
void ntp_init();
datetime_t *get_rtc_time();
NTP_TIME *get_net_time();
long get_utc_offset_seconds();
void set_utc_offset_seconds(long offset);
uint8_t to_bcd(uint8_t val);
uint8_t add_bcd(uint8_t bcd1, uint8_t bcd2);
void set_ikb_datetime_msg(uint32_t mem_shared_addr, 
                        uint16_t rtcemul_datetime_bcd_idx, 
                        uint16_t rtcemul_y2k_patch_idx, 
                        uint16_t rtcemul_datetime_msdos_idx, 
                        uint16_t gemdos_version,
                        bool y2k_patch);

#endif // RTCEMUL_H
