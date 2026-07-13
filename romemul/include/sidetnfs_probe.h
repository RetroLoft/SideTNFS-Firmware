/**
 * File: sidetnfs_probe.h
 * Description: One-shot, fire-and-forget UDP reachability probe toward the
 * TNFS server. Not a TNFS client -- no mount, no session, no reply handling.
 * Only meant to prove network reachability (visible via tcpdump on the
 * server) without touching GEMDRIVE or its timing.
 */
#ifndef SIDETNFS_PROBE_H
#define SIDETNFS_PROBE_H

#include <stdbool.h>

// Fase 5B: create a UDP PCB and udp_connect() it to the TNFS server, then
// immediately remove it again. Sends no payload at all -- udp_connect() is a
// local lwIP operation (sets the PCB's remote ip/port filter) and does not
// put anything on the wire. Must only be called after WiFi is confirmed
// connected. Never blocks, never retries, never waits for a reply, never
// logs. Returns true if the PCB was created and udp_connect() succeeded.
bool sidetnfs_udp_connect_test(void);

// Fase 5A: send a single UDP packet to the TNFS server. Kept for reference;
// not called automatically as of Fase 5B. Must only be called after WiFi is
// confirmed connected. Never blocks, never retries, never waits for a
// reply, never logs. Silently does nothing on any failure.
void sidetnfs_send_udp_probe(void);

#endif // SIDETNFS_PROBE_H
