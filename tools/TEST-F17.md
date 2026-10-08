# F17 Falcon integration test

This is the first upstream-v1.1.0-based Falcon build, not a release.
Keep the tested R16 UF2 available as a fallback. SIDEETH/SCSILINK stay disabled;
keep 00SRECP.PRG removed and the normal native boot drive attached.

1. Flash sidetnfs_f17_falcon.uf2 and cold boot. UART should say
   `F17 upstream native Falcon`. The unchanged cartridge banner may say R16.
2. Confirm AUTO, accessories, P:/M:/O:/S: and open SIDETNFS.PRG from S:.
   Check that your saved mappings are correct. Do not reset configuration.
3. Wait for `R14 NTP sync=1`, then run the unchanged R14ATEST.TOS from the
   native boot disk. Save C:\R14ATEST.LOG and the matching full UART.
4. Run SYSINFO/EVEREST from P: and your previously working TNFS application
   or game. Copy/read back/delete a small expendable folder on P: and O:.
5. Repeat a cold boot. If anything fails, save UART and checker output before
   rebooting; a stopped/flushed network capture helps for TNFS failures.

S: contains the tested SIDETNFS.PRG and README.TXT, with no SIDEETH.STX.
Do not test the upstream floppy utility with this native guest yet.
Unsupported TNFS metadata setters (-32) remain expected checker results.
The long-copy timeout is not claimed fixed. No new Atari test binary is needed.
