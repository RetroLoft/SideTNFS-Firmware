/* Host-test stand-in for lwIP's DNS API. Mirrors only what
 * sidetnfs_probe.c uses; no resolver behaviour is implemented here. */
#ifndef LWIP_HDR_DNS_H
#define LWIP_HDR_DNS_H

#include "lwip/ip_addr.h"
#include "lwip/udp.h"   /* err_t */

#ifndef ERR_INPROGRESS
#define ERR_INPROGRESS (-5)
#endif

typedef void (*dns_found_callback)(const char *name, const ip_addr_t *ipaddr, void *callback_arg);

err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr, dns_found_callback found, void *callback_arg);

#endif /* LWIP_HDR_DNS_H */
