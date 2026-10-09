/*
 * sideeth.h — SideETH STinG Ethernet port (Sidecartridge / SideTNFS)
 */
#ifndef SIDEETH_H
#define SIDEETH_H

#define ETH_ALEN 6

typedef struct {
	PORT generic;
	uint8 address[ETH_ALEN];
	uint32 gateway;
} MYPORT;

typedef struct eth_hdr {
	uint8 destination[ETH_ALEN];
	uint8 source[ETH_ALEN];
	uint16 type;
	uint8 data[0];
} ETH_HDR;

#define TYPE_IP   0x0800
#define TYPE_ARP  0x0806
#define TYPE_RARP 0x8035

int16 arp_cache(uint32 ip_addr, uint8 ether[ETH_ALEN], int update);
int16 launch_arp(uint32 ip_address, uint32 src_ip, uint8 address[ETH_ALEN], ETH_HDR *ethhdr);
int16 process_arp(uint32 ip_addr, uint8 address[ETH_ALEN], uint8 *buffer, ETH_HDR *ethptr);
void arp_init(void);

int16 xmit_dgram(MYPORT *port, IP_DGRAM *dgram, uint8 *data);
int16 fetch_dgram(MYPORT *port, IP_DGRAM **dgram);
void recve_dgram(MYPORT *port, uint8 *data, int16 length);
void deplete_queue(IP_DGRAM **queue);

/* Cart TX of a complete Ethernet frame (used by tools.c ARP replies too). */
void send_block(int unused, void *cp, short len);

#endif /* SIDEETH_H */
