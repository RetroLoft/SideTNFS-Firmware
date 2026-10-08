# F35 resource audit

F35 resource-audit test build (Pico W / RP2040)

Flash sidetnfs_f35_falcon.uf2. UART identifies F35; native ROM remains F33.
Keep existing SIDEETH.STX, config utility and RLOAD4. No retry policy changes.
Run RLOAD4 against M:\GAMES\RTYPE and log to C:\ or P:\.
Capture UART from power-on through failure, recovery, and another minute.
Retain the RLOAD log, even when all cycles succeed.

Once per minute and at faults:
F35 HEAP: allocator arena bytes, used bytes, free_in_arena bytes, free_chunks.
This is libc allocator accounting, NOT total free RAM or largest contiguous
allocation. Separate fixed lwIP pools, Atari RAM and server memory are not
measured. Compare stable post-mount baselines and long-run trends.
F35 AUDIT: per-slot cumulative logical RPCs, fault reports, PCB alloc/free,
TX-buffer alloc/free, pbuf/PCB allocation failures, UDP send errors and
server out-of-memory/too-many-files response counts (resource_rc).
F35 HANDLES: acknowledged file/directory opens/closes, current outstanding
balances and balances abandoned when a session was retired. These are
client observations, not a query of server handles. A lost OPEN reply may
have created a server handle we cannot count. Lost CLOSE replies may leave
our balance nonzero even if the server closed it. Faults count diagnostic
calls, not necessarily distinct failed guest operations.

Normal: TX alloc/free match; PCB alloc minus free equals live native TNFS
endpoints (usually one per mounted mapping). Temporary outstanding files
and directories can be legitimate. Counters survive retirement. Increasing
heap usage or handle imbalance across otherwise completed reads is evidence
to investigate, not proof by itself of a leak.

Full native host regressions passed, including allocation-failure injection,
TX buffer/PCB balance, handle accounting and persistence across retirement.
UF2 structure and embedded ROM/config verification passed. Hardware testing
is pending. Diagnostics add bounded UART overhead; successful requests are
not individually logged by this audit. No game files are included.
