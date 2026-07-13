/**
 * File: sidetnfs_probe.h
 * Description: One-shot, fire-and-forget UDP reachability probe toward the
 * TNFS server. Not a TNFS client -- no mount, no session, no reply handling.
 * Only meant to prove network reachability (visible via tcpdump on the
 * server) without touching GEMDRIVE or its timing.
 */
#ifndef SIDETNFS_PROBE_H
#define SIDETNFS_PROBE_H

// Send a single UDP packet to the TNFS server. Must only be called after
// WiFi is confirmed connected. Never blocks, never retries, never waits for
// a reply, never logs. Silently does nothing on any failure.
void sidetnfs_send_udp_probe(void);

#endif // SIDETNFS_PROBE_H
