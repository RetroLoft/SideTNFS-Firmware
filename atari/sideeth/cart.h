/*
 * cart.h — Sidecartridge ROM3 command channel for SideETH
 * See docs/sideeth-protocol.md and sidecartridge gemdrive send_sync.
 */
#ifndef SIDEETH_CART_H
#define SIDEETH_CART_H

#include <stdint.h>
#include "transprt.h"

#define ROM3_BASE 0x00FB0000UL

#define SIDEETH_CMD_GET_INFO  0x041Du
#define SIDEETH_CMD_SET_STATE 0x041Eu
#define SIDEETH_CMD_POLL      0x041Fu
#define SIDEETH_CMD_RX_ACK    0x0420u
#define SIDEETH_CMD_TX        0x0421u
#define SIDEETH_CMD_GET_MAC   0x0422u

/* Offsets within ROM3 (must match romemul/include/sidetnfs_sideeth.h). */
#define SIDEETH_OFF_MAGIC         0xF240u
#define SIDEETH_OFF_PROTO         0xF244u
#define SIDEETH_OFF_STATUS        0xF248u
#define SIDEETH_OFF_FLAGS         0xF24Cu
#define SIDEETH_OFF_MAC           0xF250u
#define SIDEETH_OFF_MTU           0xF258u
#define SIDEETH_OFF_MAX_FRAME     0xF25Au
#define SIDEETH_OFF_RX_SLOTS      0xF25Cu
#define SIDEETH_OFF_TX_MODE       0xF25Eu
#define SIDEETH_OFF_HOST_IP       0xF260u
#define SIDEETH_OFF_ATARI_IP      0xF264u
#define SIDEETH_OFF_NETMASK       0xF268u
#define SIDEETH_OFF_GATEWAY       0xF26Cu
#define SIDEETH_OFF_RX_PENDING    0xF270u
#define SIDEETH_OFF_TX_BUSY       0xF272u
#define SIDEETH_OFF_RX_DROPPED    0xF274u
#define SIDEETH_OFF_TX_DROPPED    0xF276u
#define SIDEETH_OFF_RING_META     0xF280u
#define SIDEETH_OFF_RX_PROD       0xF280u
#define SIDEETH_OFF_RX_CONS       0xF282u
#define SIDEETH_OFF_RX_SLOT_LEN   0xF284u
#define SIDEETH_OFF_RX_RING       0xF2A0u

#define SIDEETH_SLOT_SIZE 600u
#define SIDEETH_RX_SLOTS  2u
#define SIDEETH_MAGIC     0x53455448UL /* 'SETH' */

#define SIDEETH_FLAG_ENABLED (1u << 0)
#define SIDEETH_FLAG_LINK_UP (1u << 1)
#define SIDEETH_FLAG_WIFI_OK (1u << 2)

/* Send command with optional payload words after the random token.
 * payload_words may be NULL if payload_bytes == 0.
 * Returns 0 on ACK, non-zero on timeout. Must run in supervisor mode
 * or via cart_cmd() which uses Supexec. */
int16 cart_cmd_super(uint16_t cmd, const uint16_t *payload_words, uint16_t payload_bytes);

/* Supexec wrappers */
int16 cart_cmd(uint16_t cmd, const uint16_t *payload_words, uint16_t payload_bytes);
int16 cart_cmd_u16(uint16_t cmd, uint16_t value);
/* Same as cart_cmd / cart_cmd_u16 but caller is already in supervisor
 * (STinG 200 Hz poll_ports). Do not Supexec from that path. */
int16 cart_cmd_u16_super(uint16_t cmd, uint16_t value);
int16 cart_tx_frame(const uint8_t *frame, uint16_t len);

uint16_t cart_read_u16(uint32_t offset);
uint16_t cart_read_u16_super(uint32_t offset);
/* Read a WRITE_AND_SWAP_LONGWORD field as a native Atari long (no extra swap). */
uint32_t cart_read_u32_swapped(uint32_t offset);
void cart_read_bytes(uint32_t offset, uint8_t *dst, uint16_t len);
void cart_read_bytes_super(uint32_t offset, uint8_t *dst, uint16_t len);

int16 cart_receive_frame(uint8_t *dst, uint16_t capacity);
int16 cart_tx_ready(void);

#endif /* SIDEETH_CART_H */
