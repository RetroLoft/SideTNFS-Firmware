#ifndef NET_TEST_H_
#define NET_TEST_H_

// Debug-only HTTP connectivity test.
// Triggered once by net_wifi.c after WiFi connects.
// Resolves example.com, opens a TCP connection, sends an HTTP GET,
// and logs the first response line.  Proves DNS + TCP + HTTP all work.

#ifdef SIDETNFS_DEBUG
void net_test_start(void);
// Log the most recent test result unconditionally (replays last known state).
void net_test_log_result(void);
#endif

#endif // NET_TEST_H_
