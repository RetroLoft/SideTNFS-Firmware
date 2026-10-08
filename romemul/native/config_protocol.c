/**
 * File: tprotocol.c
 * Author: Diego Parrilla Santamaría
 * Date: August 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: Parse the protocol used to communicate with the ROM3
 */

#include "config_protocol.h"

uint64_t last_header_found = 0;
uint64_t new_header_found = 0;

volatile uint16_t sidetnfs_tp_last_data = 0;
volatile uint32_t sidetnfs_tp_hits = 0;
volatile uint8_t sidetnfs_tp_step = HEADER_DETECTION;
volatile uint8_t sidetnfs_tp_a0_lost = 0;
volatile uint8_t sidetnfs_tp_shifted = 0;
volatile uint8_t sidetnfs_tp_shift_fallback = 0;
static uint8_t s_shift_have_hi = 0;
static uint8_t s_shift_hi = 0;

// Global variable to keep track of current parsing state
TPParseStep nextTPstep = HEADER_DETECTION;

// Placeholder structure for parsed data
TransmissionProtocol transmission;

// Placeholder functions for each step
inline static void __not_in_flash_func(detect_header)(uint16_t data)
{
    /* Guest send_sync does move.b $FBxxxx+$ABCD. 68000 UDS/LDS reconstructs
     * A0 (odd). Falcon 16-bit cart cycles are word-aligned, so that access
     * arrives as 0xABCC. Match either. ST still sends 0xABCD.
     * After the first PING, Falcon GEMDRIVE switches to 0xA5A5 and sends
     * each protocol word as two (byte << 1) dummy reads. */
    s_shift_have_hi = 0;
    if ((data | 1u) == PROTOCOL_HEADER_SHIFTED)
    {
        sidetnfs_tp_a0_lost = 1;
        sidetnfs_tp_shifted = 1;
        nextTPstep = COMMAND_READ;
        return;
    }
    if ((data | 1u) == PROTOCOL_HEADER)
    {
        sidetnfs_tp_a0_lost = (uint8_t)((data & 1u) == 0u);
        sidetnfs_tp_shifted = 0;
        nextTPstep = COMMAND_READ;
    }
}

/* Returns 1 and writes the recovered 16-bit word when both shifted
 * dummy-bytes of a protocol word have arrived.
 *
 * Dummy addresses for (byte<<1) are 0..510. A value above that is a
 * full 16-bit protocol word from the old send_sync. Falcon 030 I-cache
 * can keep that body after Pico patches the ROM header to 0xA5A5, which
 * used to parse GEMDOS as PING with a thousand-byte payload. */
inline static int __not_in_flash_func(take_shifted_word)(uint16_t data, uint16_t *out_word)
{
    if (data > 510u)
    {
        sidetnfs_tp_shifted = 0;
        sidetnfs_tp_shift_fallback = 1;
        s_shift_have_hi = 0;
        *out_word = data;
        return 1;
    }
    uint8_t b = (uint8_t)((data >> 1) & 0xFFu);
    if (!s_shift_have_hi)
    {
        s_shift_hi = b;
        s_shift_have_hi = 1;
        return 0;
    }
    *out_word = (uint16_t)(((uint16_t)s_shift_hi << 8) | b);
    s_shift_have_hi = 0;
    return 1;
}

inline static void __not_in_flash_func(read_command)(uint16_t data)
{
    if (sidetnfs_tp_shifted)
    {
        uint16_t word;
        if (!take_shifted_word(data, &word))
        {
            return;
        }
        data = word;
    }
    transmission.command_id = data;
    nextTPstep = PAYLOAD_SIZE_READ;
}

inline static void __not_in_flash_func(read_payload_size)(uint16_t data)
{
    if (sidetnfs_tp_shifted)
    {
        uint16_t word;
        if (!take_shifted_word(data, &word))
        {
            return;
        }
        data = word;
    }
    if (data > 0)
    {
        transmission.payload_size = data; // Store incoming data as payload size
        nextTPstep = PAYLOAD_READ_START;
    }
    else
    {
        nextTPstep = PAYLOAD_READ_END;
    }
    transmission.bytes_read = 0;
}

inline static void __not_in_flash_func(read_payload)(uint16_t data)
{
    if (sidetnfs_tp_shifted)
    {
        uint16_t word;
        if (!take_shifted_word(data, &word))
        {
            return;
        }
        data = word;
    }
    *((uint16_t *)&transmission.payload[transmission.bytes_read]) = data;
    transmission.bytes_read += 2;

    if (transmission.bytes_read >= transmission.payload_size)
    {
        nextTPstep = PAYLOAD_READ_END;
    }
    else
    {
        nextTPstep = PAYLOAD_READ_INPROGRESS;
    }
}

void init_protocol_parser()
{
    transmission.command_id = 0;
    transmission.payload_size = 0;
    transmission.payload = malloc(MAX_PROTOCOL_PAYLOAD_SIZE);
    transmission.bytes_read = 0;
}

void terminate_protocol_parser()
{
    if (transmission.payload)
    {
        free(transmission.payload);
        transmission.payload = NULL; // Set the pointer to NULL after freeing to avoid potential double freeing and other issues
    }
}

inline void __not_in_flash_func(process_command)(ProtocolCallback callback)
{
#if defined(_DEBUG) && (_DEBUG != 0) && defined(SHOW_COMMANDS) && (SHOW_COMMANDS != 0)
    DPRINTF("COMMAND: %d / PAYLOAD SIZE: %d / PAYLOAD: ", transmission.command_id, transmission.payload_size);
    for (int i = 0; i < transmission.payload_size; i += 2)
    {
        uint16_t value = *((uint16_t *)(&transmission.payload[i]));
        DPRINTFRAW("0x%04X ", value);
    }
    DPRINTFRAW("\n");
#endif
    // Here should pass the transmission message to a function that will handle the different commands
    // I think a good aproach would be to have a callback to custom functions that will handle the different commands

    if (callback)
    {
        callback(&transmission);
    }

    transmission.command_id = 0;
    transmission.payload_size = 0;
    transmission.bytes_read = 0;
    last_header_found = 0;
    s_shift_have_hi = 0;
    // Handle the end of the payload reading. Maybe reset state or take some action based on received data.
    nextTPstep = HEADER_DETECTION; // Resetting to start for the next message.
}

inline void __not_in_flash_func(parse_protocol)(uint16_t data, ProtocolCallback callback)
{
    sidetnfs_tp_last_data = data;
    sidetnfs_tp_hits++;
    new_header_found = (((uint64_t)timer_hw->timerawh) << 32u | timer_hw->timerawl);
    if (new_header_found - last_header_found > PROTOCOL_READ_RESTART_MICROSECONDS)
    {
        nextTPstep = HEADER_DETECTION;
    }
    sidetnfs_tp_step = (uint8_t)nextTPstep;
    switch (nextTPstep)
    {
    case HEADER_DETECTION:
        detect_header(data);
        last_header_found = new_header_found;
        break;

    case COMMAND_READ:
        read_command(data);
        break;

    case PAYLOAD_SIZE_READ:
        read_payload_size(data);
        // If PAYLOAD_READ_END here, means we've finished reading the payload
        if (nextTPstep == PAYLOAD_READ_END)
        {
            process_command(callback);
        }
        break;

    case PAYLOAD_READ_START:
    case PAYLOAD_READ_INPROGRESS:
    case PAYLOAD_READ_END:
        if (transmission.bytes_read < transmission.payload_size)
        {
            read_payload(data);
        }
        // If PAYLOAD_READ_END here, means we've finished reading the payload
        if (nextTPstep == PAYLOAD_READ_END)
        {
            process_command(callback);
        }
        break;
    }
    sidetnfs_tp_step = (uint8_t)nextTPstep;
}
