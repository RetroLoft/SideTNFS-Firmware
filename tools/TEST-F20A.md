# F20A: delayed SIDEETH cookie publication

F20 failed hardware boot: HDDRIVER ran but did not detect BlueSCSI. F20A moves publication of the SE20 cookie from cartridge entry to the first native boot-drive AUTO/ACC search. The early cookie-table replacement is a suspected boot-order problem, not a confirmed root cause. No network/backend or STX behavior changed.

1. Keep SIDEETH.STX disabled. Flash sidetnfs_f20a_falcon.uf2 and cold boot. Confirm HDDRIVER detects BlueSCSI and the boot drive, AUTO programs and accessories work.
2. If detection fails, stop and return to the tested F19 UF2. Capture UART and the HDDRIVER display details.
3. If baseline boots repeatedly, run the unchanged R14ATEST.TOS and load SYSINFO/EVEREST from P: and an application from TNFS.
4. Only then enable the matching F20 SIDEETH.STX included here. This remains local-only: Atari 192.168.254.2/24, gateway 192.168.254.1, MTU 576. Check idle coexistence, then gateway ping alongside SD/TNFS reads. Internet routing remains disabled.

SIDEETH.STX and the TOS test binaries are unchanged from F20. F19 remains the stable recovery build. F20A is unverified on hardware.
