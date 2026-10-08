# F21 SIDEETH routed Falcon candidate

F20A is the hardware-tested local-gateway checkpoint. F21 retains its exact native guest ROM and deferred cookie publication, adding bounded Pico forwarding for SIDEETH IPv4 TCP, UDP and ICMP echo. Firmware and the included STX are for Pico W / Falcon testing; hardware validation remains outstanding.

## Install

Flash sidetnfs_f21_falcon.uf2. Replace SIDEETH.STX with the supplied copy (same mailbox protocol, corrected routed-build banner). Keep other unused network drivers disabled. Cold boot and confirm HDDRIVER/BlueSCSI, AUTO and accessories.

Keep Atari address 192.168.254.2, mask 255.255.255.0 and MTU 576. STinG's default route must use SideETH via 192.168.254.1. Set STinG NAMESERVER to a DNS server reachable from the Pico Wi-Fi network (for example your existing LAN resolver). The gateway address is not a DNS proxy. Pico Wi-Fi must be configured and connected; the SideTNFS utility still controls that independently.

## Test

1. Ping 192.168.254.1, then a known reachable external IP. A host/firewall may choose not to answer ICMP.
2. Run CAB with its STinG-compatible CAB.OVL. First open a small known plain HTTP page from a reachable server by IP, then by hostname. Use an existing HTTP server; HTTPS/TLS support depends on CAB and its overlay and is not added by this firmware.
3. Read a larger page/image or download a file. Then access P:, S: and TNFS, run R14ATEST.TOS, and load a known application. Save UART from boot through browser and drive tests.
4. Repeat a cold boot. Disconnect/reconnect Wi-Fi and start a new browser connection; existing connections may need reopening.

Keep the F20A kit for rollback. Do not install the new ping accessory for the first routing test; validate one new component at a time. On classic single-tasking TOS, alternate browser and desktop checks; no simultaneous foreground applications are assumed.

The guest RPC locking, SD/TNFS retry behavior and firmware configuration layout are unchanged. NAT tables/queues are bounded; heavy traffic can cause drops and TCP retransmission. No incoming port forwarding is provided.

## Routing limits

There are 16 translation entries, at most eight per protocol, and four packets in each direction's queue. TCP MSS is capped at 536 for the 576-byte Atari MTU. IPv4 fragments and forwarded ICMP error messages are not supported in this first routing candidate; ICMP echo request/reply is supported. Ordinary small DNS replies and TCP web traffic are the intended first tests. This is not a general VPN or an inbound server bridge.
