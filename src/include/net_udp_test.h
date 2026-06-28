#ifndef NET_UDP_TEST_H_
#define NET_UDP_TEST_H_

// Debug-only UDP connectivity test toward the TNFS server.
// Sends a TNFS mount probe and logs the server reply.
// Proves UDP send/receive from Pico W before real TNFS is implemented.

#include <stdbool.h>

#ifdef SIDETNFS_DEBUG
void net_udp_test_start(void);
void net_udp_test_poll(void);
void net_udp_test_log_result(void);
bool net_udp_test_ok(void);  // true once a reply has been received from the TNFS server
#endif

#endif // NET_UDP_TEST_H_
