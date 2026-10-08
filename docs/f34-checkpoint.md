# F34 checkpoint — 2 October 2026

F33 compact ROM plus F34 64-bit TNFS remount cooldown. Native host tests,
UF2/embedded-image checks passed; old timer fails the long-idle regression.
Configuration and SIDEETH binaries are unchanged from F32.

Latest RLOAD4 source/binary is in tools/rload; viewer in tools/rload-viewer.
RLOAD4 selects a separate log directory (default P:\), preserving CRC workload,
cycle timing and the ten-second failure recovery probe from RLOAD3.

Performance investigation remains open. Earlier F33/RLOAD3 averaged 67.186s
across 84 successful cycles. F34/RLOAD3 averaged 135.204s across five; reverting
to F33/RLOAD3 still averaged 130.515s across six. All completed cycles passed
CRC. Retransmissions recovered but cannot explain the full slowdown.
Only one AP is enabled; channel congestion/interference remains a hypothesis.
Next: inspect AP radio conditions and compare controlled channel tests.
Do not claim F34 hardware recovery validation from these successful transfers.
