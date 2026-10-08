# F21 routed SIDEETH candidate

Built on hardware-tested F20A, with its native guest ROM unchanged. Adds a bounded main-loop NAT adapter for TCP, UDP and ICMP echo. lwIP input interception is compiled only for the native Falcon variant; reserved PCB ports isolate forwarded traffic from native services. Local ARP/echo and the CRC mailbox/gate remain intact. See tools/TEST-F21.md for CAB setup and validation.

# F20A boot regression candidate

SE20 cookie publication is deferred until native AUTO/ACC activation. F20 failed BlueSCSI detection on hardware; early cookie replacement is a hypothesis. Validate disk boot with SIDEETH disabled first.

# F20 SIDEETH local-gateway stage

This isolated branch retains F19 safe TNFS recovery and adds a protocol-2
SIDEETH mailbox in f240..f9b6, after upstream and native reply allocations.
It does not use the old overlapping 4598 SIDEETH map or legacy NAT hooks.
Only local gateway ARP and ICMP echo are implemented; no routed traffic.

Native resident publishes SE20 cookie with a shared RAM bus gate. Native
GEMDRIVE holds it through framing, waiting and response copy. SIDEETH timer
callbacks defer when occupied, enqueue one asynchronous request, and never
wait for its reply. They consume at most one TX/RX frame per callback.
No timer GEMDOS/XBIOS calls. Firmware processes mailbox requests only on
idle native service passes. Source-level host tests exercise the actual STX
mailbox against the actual Pico parser, including the busy gate and ACKs.

Build guest with make falcon; firmware tools/build_sideeth.py compiles the
module using cached offline toolchain and existing local STinG headers.
Firmware tools/build_falcon.py uses read-only SDKs in the original integration
checkout; outputs stay in this worktree. No submodule fetches or ROM patching.
The module cannot be used with F19; use the packaged pair. Hardware pending.

# F19: TNFS recovery capture build

F19 retains the F18 guest ROM. Firmware-side recovery retires failed mount
and read-only sessions without replaying the failed guest operation. A later
request may remount after a one-second cooldown. All old directory cursors
are invalidated. Uncertain mutations remain poisoned, including when a later
caller attempts a read. The interrupted read returns -2 rather than the fatal
-65 that permanently latches the guest mapping. Existing guest file handles
are path/offset records, not retained server file descriptors.

Per-request UART diagnostics support packet correlation. Underlying packet
loss and same-session sequence wrap remain unresolved. Hardware testing is
pending. S: README is refreshed; SIDETNFS.PRG is unchanged.

## F18 console restoration history

F18 is a console-restoration hardware candidate. F17 passed Falcon hardware
checks and application loading; it is tagged falcon-f17-tested in both repos. This branch starts at upstream v1.1.0
(e580d145 firmware; 6fca62c2 GEMDRIVE) and adds the R16 native guest as an
explicit build option. F17 remains the hardware-tested fallback.

## Build selection

In SideTNFS-Gemdrive-integration: `make falcon`.
In SideTNFS-Firmware-integration: `make test-falcon`, then `make falcon`.
CMake option SIDETNFS_NATIVE_FALCON defaults OFF; upstream's ST ROM remains
selected by default. The native option is currently limited to Pico W.
SDK submodules are pinned by upstream; once initialized, these builders run
offline. Guest compilation uses cached Docker image 304fea9c3e8e with no
network or host mounts. No firmware is flashed automatically.

F18 restores the upstream-style SideTNFS title, core/TOS version, GEMDOS hook
status, SD mount result and published drive letters on the Atari console.
Network/NTP is identified as background initialization, not completed sync.
The guest uses GEMDOS Cconws through the existing reentrancy bypass; it adds
no network wait, key prompt or boot routing change. Allocation failure gets
a failure message rather than a success banner. The UART identifies F18.
The resident image changes for these messages; F17 had exact R16 parity.
No source substitution or ROM instruction patching is used. Native modules
are in romemul/native and src/native. Guarded hooks in upstream gemdrvemul.c
route native packets and defer Wi-Fi/NTP until SD activation. Upstream's
legacy configuration handlers remain available through the tested shifted
Falcon codec. CMake assertions reject a shared-memory overlap.

The S: image retains the tested SIDETNFS.PRG and README.TXT. SIDEETH.STX is
no longer exposed. Configuration flash and API version 3 are unchanged;
verify existing mappings in the utility before changing configuration.
Floppy emulation hooks are not installed by the native guest; the new
upstream floppy feature remains supported only by the upstream guest path.
SIDEETH remains parked. No Config repository source changes were required.

## Shared fixes needed to build

Generated PIO headers use the CMake include path instead of a fixed build
folder. ROM DMA copying preserves a trailing halfword (the R16 fix).
The descriptor sorter now uses a signed int comparator and consistently
16-bit array elements; host tests exercise unsorted allocation gaps.
Default upstream builds include these small corrections as well.

## Validation and limitations

Host regression tests cover the actual native guest/SD/TNFS/NTP code,
S: file content, configuration framing, ROM DMA and descriptor allocation.
Both Pico W firmware modes and both guest build modes compile. The upstream
ST path still needs hardware validation. F18 still needs a fresh Falcon hardware test for the new console calls.
F17 passed the R14A checker and application loading; that does not validate
every native or upstream feature.

Upstream sidetnfs_probe.c fixes remain in the upstream backend. Native TNFS
uses tnfs_transport.h; it already checks peer/session/sequence and repeats
identical requests. This port does not automatically transfer every newer
upstream TNFS policy into that independent transport. Same-session sequence
reuse and the long-copy timeout remain open. Do not interpret F18 as resolving
them. Forced pre-mount DNS recovery and standalone cartridge-only activation
also remain unvalidated/unsupported as documented in R16.
