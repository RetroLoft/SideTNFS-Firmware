/* Fase 11A host-test stub: the real romemul/include/network.h pulls in
 * Pico SDK/lwIP/cyw43 hardware headers a host toolchain can't build.
 * sidetnfs_netconfig.c only needs these three field-size constants
 * (matching the real header's values exactly) and get_country_code()'s
 * declaration. Picked up instead of the real network.h because
 * sidetnfs_netconfig.c/.h are symlinked into this sandbox -- see
 * ../sidetnfs_netconfig.c and ../include/sidetnfs_netconfig.h.
 */
#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>

#define MAX_SSID_LENGTH 36
#define MAX_PASSWORD_LENGTH 68
#define IPV4_ADDRESS_LENGTH 16

// Real signature from romemul/network.c -- faithful copy of its logic
// lives in test_netconfig.c (network.c itself is not host-compilable).
uint32_t get_country_code(char *c, char **valid_country_str);

#endif // NETWORK_H
