# F20 Falcon SIDEETH local-gateway coexistence candidate

This is the first SIDEETH integration stage, not Internet/TNFS-over-STinG.
The normal Pico TNFS/Wi-Fi path remains available. SIDEETH exposes only a
local ARP/ICMP gateway at 192.168.254.1; routed TCP/UDP is deliberately disabled
pending NAT validation. The Atari SIDEETH address is 192.168.254.2/24, MTU 576.

1. Keep the tested F19 UF2 and your old SIDEETH.STX as rollback copies. Flash
   sidetnfs_f20_falcon.uf2. UART must say F20 SIDEETH local-only Falcon.
2. First boot with SIDEETH disabled in STinG. Check AUTO/accessories, P:/S:,
   TNFS and the unchanged R14ATEST.TOS (after NTP sync). Save UART and log.
3. Replace the old SIDEETH.STX in your STinG module directory with this kit's
   SIDEETH.STX. Enable only this Ethernet module; keep SCSILINK disabled.
   Cold boot. It must report F20 local gateway only. The old STX is NOT
   compatible with this firmware's new memory layout. This module refuses
   firmware without the SE20 bus-arbitration cookie and protocol version 2.
4. Check the same GEMDRIVE functions with SIDEETH enabled but idle. If that
   passes, use your STinG ping utility to ping 192.168.254.1. Save UART.
   Do not use a public IP/hostname as the SIDEETH test: routing is disabled.
5. While pinging, browse P:/O:/M:/S:, Show text files, and load SYSINFO or
   EVEREST from P: and TNFS. Then repeat R14ATEST. Record responsiveness,
   any drive failures, packet loss and whether gateway ping resumes after
   a long TNFS operation. GEMDRIVE has priority; SIDEETH may defer/drop.
6. If this fails, disable the new module or restore F19 plus your previous
   module configuration. Save complete UART before rebooting.

The interface has one queued receive frame (two slots with one empty).
Timer callbacks perform no GEMDOS/XBIOS calls and never wait for a Pico ACK.
Commands enqueue asynchronously; bus framing and RX snapshots share the
native GEMDRIVE gate. A stuck mailbox is not automatically replayed: save
UART and reboot if it stops acknowledging. This stage does not validate
routed networking, NAT, ST hardware, Pico 2 W or network throughput.

No configuration flash reset is needed. S: retains the tested configuration
utility; install SIDEETH.STX manually from this kit rather than from S:.
