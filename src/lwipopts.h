#ifndef LWIPOPTS_H_
#define LWIPOPTS_H_

// ─── lwIP configuration for SideTNFS ─────────────────────────────────────────
// Bare-metal (no RTOS).  DHCP for IP assignment.
// UDP is the production protocol (TNFS uses UDP port 16384).
// TCP + DNS are enabled for the debug HTTP connectivity test.

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Memory
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000
#define PBUF_POOL_SIZE              24
#define MEMP_NUM_ARP_QUEUE          4

// Protocols
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    1
#define LWIP_RAW                    0

// TCP sizing — standard Pico W values; only actively used by the debug HTTP test
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define MEMP_NUM_TCP_SEG            32

// DNS
#define DNS_TABLE_SIZE              4
#define DNS_MAX_SERVERS             2

// Callbacks used by the CYW43 driver
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// DHCP: skip ARP check for speed
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Checksum algorithm
#define LWIP_CHKSUM_ALGORITHM       3

// Stats — disabled to save RAM
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0

// Debug — all off
#define LWIP_DEBUG                  0

#endif // LWIPOPTS_H_
