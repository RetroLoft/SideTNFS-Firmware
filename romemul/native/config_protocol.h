/**
 * File: tprotocol.h
 * Author: Diego Parrilla Santamaría
 * Date: August 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: Header for the protocol parser for ROM3
 */

#ifndef TPROTOCOL_H
#define TPROTOCOL_H

#include "include/debug.h"
#include "include/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTOCOL_HEADER 0xABCD
/* Falcon send_sync after the first PING: move.b $FBA5A5, then each
 * 16-bit protocol word as two dummy reads of (byte << 1). Even, and
 * outside the 0..510 shifted-byte range, so it cannot alias payload. */
#define PROTOCOL_HEADER_SHIFTED 0xA5A5
#define PROTOCOL_READ_RESTART_MICROSECONDS 10000
#define MAX_PROTOCOL_PAYLOAD_SIZE 2048 + 64 // 1024 bytes of payload plus 64 bytes of overhead for safety

#define SHOW_COMMANDS 0 // Set to 1 to show commands received

/* Last ROM3 address word seen by parse_protocol(). Falcon 16-bit cart
 * often presents odd dummy-read addresses with A0=0 (0xABCC vs 0xABCD). */
extern volatile uint16_t sidetnfs_tp_last_data;
extern volatile uint32_t sidetnfs_tp_hits;
extern volatile uint8_t sidetnfs_tp_step;
/* Set when this protocol message's header arrived as 0xABCC (Falcon). */
extern volatile uint8_t sidetnfs_tp_a0_lost;
/* Set when the header was PROTOCOL_HEADER_SHIFTED (lossless Falcon codec). */
extern volatile uint8_t sidetnfs_tp_shifted;
/* Sticky: this message started as shifted, then a dummy >510 arrived
 * (old send_sync still in the 68030 I-cache). Main loop logs and clears. */
extern volatile uint8_t sidetnfs_tp_shift_fallback;

typedef enum
{
    HEADER_DETECTION,
    COMMAND_READ,
    PAYLOAD_SIZE_READ,
    PAYLOAD_READ_START,
    PAYLOAD_READ_INPROGRESS,
    PAYLOAD_READ_END
} TPParseStep;

typedef struct
{
    uint16_t command_id;    // Command ID
    uint16_t payload_size;  // Size of the payload
    unsigned char *payload; // Pointer to the payload data
    uint16_t bytes_read;    // To keep track of how many bytes of the payload we've read so far.
} TransmissionProtocol;

typedef void (*ProtocolCallback)(const TransmissionProtocol *);

// Function to parse the protocol
void parse_protocol(uint16_t data, ProtocolCallback callback);
void init_protocol_parser();
void terminate_protocol_parser();

#endif // TPROTOCOL_H
