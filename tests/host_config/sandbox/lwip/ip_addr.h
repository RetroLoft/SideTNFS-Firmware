#ifndef LWIP_IP_ADDR_H
#define LWIP_IP_ADDR_H
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint32_t addr; } ip_addr_t;

#define IPADDR_NONE 0xffffffffUL

bool ipaddr_aton(const char *cp, ip_addr_t *addr);
char *ipaddr_ntoa(const ip_addr_t *addr);
static inline int ip_addr_cmp(const ip_addr_t *a, const ip_addr_t *b) { return a->addr == b->addr; }

#endif
