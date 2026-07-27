/**
 * File: rtcemul.c
 * Author: Diego Parrilla Santamaría
 * Date: November 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: Multi format RTC emulator
 */

#include "include/rtcemul.h"

// NTP and RTC variables
static datetime_t rtc_time = {0};
static NTP_TIME net_time;
static long utc_offset_seconds = 0;
static char *ntp_server_host = NULL;
static int ntp_server_port = NTP_DEFAULT_PORT;

datetime_t *get_rtc_time()
{
    return &rtc_time;
}

NTP_TIME *get_net_time()
{
    return &net_time;
}

long get_utc_offset_seconds()
{
    return utc_offset_seconds;
}

void set_utc_offset_seconds(long offset)
{
    utc_offset_seconds = offset;
}

void host_found_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    if (name == NULL)
    {
        DPRINTF("NTP host name is NULL\n");
        return;
    }

    NTP_TIME *ntime = (NTP_TIME *)(arg);
    if (ntime == NULL)
    {
        DPRINTF("NTP_TIME argument is NULL\n");
        ntime->ntp_error = true;
        return;
    }

    if (ipaddr != NULL && !ntime->ntp_server_found)
    {
        ntime->ntp_server_found = true;
        ntime->ntp_ipaddr = *ipaddr;
        DPRINTF("NTP Host found: %s\n", name);
        DPRINTF("NTP Server IP: %s\n", ipaddr_ntoa(&ntime->ntp_ipaddr));
    }
    else if (ipaddr == NULL)
    {
        DPRINTF("IP address for NTP Host '%s' not found.\n", name);
        ntime->ntp_error = true;
    }
}

static void ntp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    // Logging the entry into the callback
    DPRINTF("ntp_recv_callback\n");

    // Validate the NTP response
    if (p == NULL || p->tot_len != NTP_MSG_LEN)
    {
        DPRINTF("Invalid NTP response size\n");
        if (p != NULL)
        {
            pbuf_free(p);
        }
        return;
    }

    // Ensure we are getting the response from the server we expect
    if (!ip_addr_cmp(&net_time.ntp_ipaddr, addr) || port != NTP_DEFAULT_PORT)
    {
        DPRINTF("Received response from unexpected server or port\n");
        pbuf_free(p);
        return;
    }

    // Extract relevant fields from the NTP message
    uint8_t mode = pbuf_get_at(p, 0) & 0x07; // mode should be 4 for server response
    uint8_t stratum = pbuf_get_at(p, 1);     // stratum should not be 0

    // Check if the message has the correct mode and stratum
    if (mode != 4 || stratum == 0)
    {
        DPRINTF("Invalid mode or stratum in NTP response\n");
        pbuf_free(p);
        return;
    }

    // Extract the Transmit Timestamp (field starting at byte 40)
    uint32_t transmit_timestamp_secs;
    pbuf_copy_partial(p, &transmit_timestamp_secs, sizeof(transmit_timestamp_secs), 40);
    transmit_timestamp_secs = lwip_ntohl(transmit_timestamp_secs) - NTP_DELTA + utc_offset_seconds;

    // Convert NTP time to a `struct tm`
    time_t utc_sec = transmit_timestamp_secs;
    struct tm *utc = gmtime(&utc_sec);
    if (utc == NULL)
    {
        DPRINTF("Error converting NTP time to struct tm\n");
        pbuf_free(p);
        return;
    }

    // Fill the rtc_time structure
    rtc_time.year = utc->tm_year + 1900;
    rtc_time.month = utc->tm_mon + 1;
    rtc_time.day = utc->tm_mday;
    rtc_time.hour = utc->tm_hour;
    rtc_time.min = utc->tm_min;
    rtc_time.sec = utc->tm_sec;
    rtc_time.dotw = utc->tm_wday; // Day of the week, Sunday is day 0

    // Set the RTC with the received time
    if (!rtc_set_datetime(&rtc_time))
    {
        DPRINTF("Cannot set internal RTC!\n");
    }
    else
    {
        DPRINTF("RP2040 RTC set to: %02d/%02d/%04d %02d:%02d:%02d UTC+0\n",
                rtc_time.day, rtc_time.month, rtc_time.year, rtc_time.hour, rtc_time.min, rtc_time.sec);
    }

    // Free the packet buffer
    pbuf_free(p);
}

void ntp_init()
{
    // Attempt to allocate a new UDP control block.
    net_time.ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (net_time.ntp_pcb == NULL)
    {
        DPRINTF("Failed to allocate a new UDP control block.\n");
        return;
    }

    // Set up the callback function that will be called when an NTP response is received.
    udp_recv(net_time.ntp_pcb, ntp_recv_callback, &net_time);

    // Initialization success, set flag.
    net_time.ntp_server_found = false;
    net_time.ntp_error = false;
    DPRINTF("NTP UDP control block initialized and callback set.\n");
}

void set_internal_rtc()
{
    // Begin LwIP operation
    cyw43_arch_lwip_begin();

    // Allocate a pbuf for the NTP request.
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    if (!pb)
    {
        DPRINTF("Failed to allocate pbuf for NTP request.\n");
        cyw43_arch_lwip_end();
        return; // Early exit if pbuf allocation fails
    }

    // Prepare the NTP request.
    uint8_t *req = (uint8_t *)pb->payload;
    memset(req, 0, NTP_MSG_LEN);
    req[0] = 0x1b; // NTP request header for a client request

    // Send the NTP request.
    err_t err = udp_sendto(net_time.ntp_pcb, pb, &net_time.ntp_ipaddr, ntp_server_port);
    if (err != ERR_OK)
    {
        DPRINTF("Failed to send NTP request: %s\n", lwip_strerr(err));
        pbuf_free(pb); // Clean up the pbuf
        cyw43_arch_lwip_end();
        return; // Early exit if sending fails
    }

    // Free the pbuf after sending.
    pbuf_free(pb);

    // End LwIP operation.
    cyw43_arch_lwip_end();

    DPRINTF("NTP request sent successfully.\n");
}

void set_ikb_datetime_msg(uint32_t mem_shared_addr, 
                        uint16_t rtcemul_datetime_bcd_idx, 
                        uint16_t rtcemul_y2k_patch_idx, 
                        uint16_t rtcemul_datetime_msdos_idx, 
                        uint16_t gemdos_version,
                        bool y2k_patch)
{
    uint8_t *rtc_time_ptr = (uint8_t *)(mem_shared_addr + rtcemul_datetime_bcd_idx);
    DPRINTF("GEMDOS version: %x\n", gemdos_version);
    rtc_get_datetime(&rtc_time);

    DPRINTF("RP2040 RTC set to: %02d/%02d/%04d %02d:%02d:%02d UTC+0\n",
                    rtc_time.day, 
                    rtc_time.month, 
                    rtc_time.year, 
                    rtc_time.hour, 
                    rtc_time.min, 
                    rtc_time.sec);

    // Now set the MSDOS time format after the BCD format
    uint32_t msdos_datetime = 0;

    // Convert the RTC time to MSDOS datetime format
    uint16_t msdos_date = ((rtc_time.year - 1980) << 9) | (rtc_time.month << 5) | (rtc_time.day);
    uint16_t msdos_time = (rtc_time.hour << 11) | (rtc_time.min << 5) | (rtc_time.sec / 2);

    // Change order for the endianess
    rtc_time_ptr[1] = 0x1b;

    // If negative number, it is EmuTOS
    if ((gemdos_version >= 0) && (y2k_patch)) {
        DPRINTF("Applying Y2K fix in the date\n");
        rtc_time_ptr[0] = add_bcd(to_bcd((rtc_time.year % 100)), to_bcd((2000 - 1980) + (80 - 30))); // Fix Y2K issue
    } else {
        DPRINTF("Not applying Y2K fix in the date\n");
        rtc_time_ptr[0] = to_bcd(rtc_time.year % 100); // EmuTOS already handles the Y2K issue 
        // If the TOS is EmuTOS, then we disable the Y2K fix
        *((volatile uint32_t *)(mem_shared_addr + rtcemul_y2k_patch_idx)) = 0;
    }
    rtc_time_ptr[3] = to_bcd(rtc_time.month);
    rtc_time_ptr[2] = to_bcd(rtc_time.day);
    rtc_time_ptr[5] = to_bcd(rtc_time.hour);
    rtc_time_ptr[4] = to_bcd(rtc_time.min);
    rtc_time_ptr[7] = to_bcd(rtc_time.sec);
    rtc_time_ptr[6] = 0x0;

    // Store MSDOS datetime into shared memory
    msdos_datetime = (msdos_date << 16) | msdos_time;
    WRITE_AND_SWAP_LONGWORD(mem_shared_addr, rtcemul_datetime_msdos_idx, msdos_datetime);
    DPRINTF("MSDOS datetime: 0x%08x\n", msdos_datetime);
}

inline uint8_t to_bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

// Function to add two BCD values
inline uint8_t add_bcd(uint8_t bcd1, uint8_t bcd2)
{
    uint8_t low_nibble = (bcd1 & 0x0F) + (bcd2 & 0x0F);
    uint8_t high_nibble = (bcd1 & 0xF0) + (bcd2 & 0xF0);

    if (low_nibble > 9)
    {
        low_nibble += 6;
    }

    high_nibble += (low_nibble & 0xF0); // Add carry to high nibble
    low_nibble &= 0x0F;                 // Keep only the low nibble

    if ((high_nibble & 0x1F0) > 0x90)
    {
        high_nibble += 0x60;
    }

    return (high_nibble & 0xF0) | (low_nibble & 0x0F);
}
