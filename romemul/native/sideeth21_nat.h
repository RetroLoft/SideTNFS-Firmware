/* F21 bounded IPv4 NAT. All calls run on the polling lwIP main-loop core. */
#ifndef SIDEETH21_NAT_H
#define SIDEETH21_NAT_H
#include <stdbool.h>
#include <stdint.h>
struct netif;
struct pbuf;
void s21_nat_init(void);
/* NULL/down WiFi, disabled endpoint, or changed WiFi address flushes all state. */
void s21_nat_set_link(struct netif *wifi, bool enabled);
uint16_t s21_nat_tx(const uint8_t *frame, uint16_t length);
void s21_nat_poll(void);
/* Consumes/frees p only when returning 1; unmatched local traffic stays in lwIP. */
int s21_nat_ip4_input(struct pbuf *p, struct netif *inp);
/* Endpoint callback: 0 accepted; nonzero means ring full. Must not call lwIP. */
int s21_nat_deliver(const uint8_t *frame, uint16_t length);
struct s21_nat_stats {
    uint32_t tx_packets,rx_packets,tx_dropped,rx_dropped,invalid,expired;
    uint16_t mappings,tx_pending,rx_pending;
};
const struct s21_nat_stats *s21_nat_get_stats(void);
#endif
