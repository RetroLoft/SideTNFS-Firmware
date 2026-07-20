/* Fase 11A host-test stub for lwip/ip_addr.h -- the real lwIP stack isn't
 * host-compilable in this project's tree (deeply tied into the Pico SDK
 * build). ip_addr_t here is a bare placeholder (sidetnfs_netconfig.c never
 * inspects its contents, only passes a pointer through to ipaddr_aton()).
 * ipaddr_aton() itself is implemented in test_netconfig.c as a plain,
 * strict decimal-dotted-quad parser -- not lwIP's exact ip4addr_aton()
 * (which additionally accepts hex/octal and shortened forms), but
 * sufficient to test sidetnfs_netconfig_validate()'s own accept/reject
 * behavior for ordinary dotted-quad IPv4 strings and obviously-invalid
 * input, which is all this validation layer relies on.
 */
#ifndef LWIP_IP_ADDR_H
#define LWIP_IP_ADDR_H

typedef struct
{
    unsigned int addr;
} ip_addr_t;

int ipaddr_aton(const char *cp, ip_addr_t *addr);

#endif // LWIP_IP_ADDR_H
