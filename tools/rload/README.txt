RLOAD v4 - R-Type installation read stress test

Run RLOAD4.TOS, enter M:\GAMES\RTYPE (default) or O:\GAMES\RTYPE.
This is an Atari drive path, not a tnfs:// URL.
Press ESC to stop between files/cycles. A current network read must finish
or time out before the key is handled. Press a key at the final screen.

Each cycle allocates RAM separately for each file, opens it read-only,
checks length, reads the entire file in a single GEMDOS Fread, verifies
CRC32 against this installation, checks EOF, closes and frees memory.
A successful cycle reports PASS with a decimal cycle count.
On the first failure the loop stops, reports file/stage/result, and
attempts to record the failure. A TNFS outage may prevent that last log
write; preserve the screen/UART output in that case.

Logs: a NEW RLOAD000.LOG ... RLOAD999.LOG in the chosen log folder.
Existing logs are not overwritten. Logs are updated and closed after
every cycle. Refresh/reopen them on the Mac to see progress. Counters
start again at 1 for each launch. If all 1000 names exist, test won't start.
The log is the only file written by the test. Log-write failure stops it.

Files checked in order (436571 bytes/cycle):
 RUNME.TOS           3678
 HAGA               13448
 FRT228.HST         61104  ST/STE intro
 RT240D.BMP         77878  Falcon/TT intro
 FILES\RTYPE.FIC    56371
 FILES\RRD         224092  game archive
Checksums are specific to your installation as inspected 2 October 2026.
Different releases or edited assets intentionally fail validation.

Coverage:
This reads and validates all six executable/intro/game-data assets each
cycle, including both machine-specific intros and the entire archive.
It does NOT execute/decompress the game, perform Pexec relocation, alter
hardware, or precisely replay the game's runtime seek/read sequence.
README, walkthrough, savestates and unrelated netusbee.zip are excluded.
No copyrighted game files are bundled or modified.

Why this file set:
RUNME.TOS contains a packing stub. Offline unpacking yields a TOS image
with text=0x2fa4, data=0x16, bss=0x1b266. Disassembly of unpacked text:
 0x000-0x044 opens HAGA (local fallback) and reads up to 0x36b0 bytes.
 0xb22-0xb5c reads FRT228.HST, 0xeeb0 bytes.
 0x2e22-0x2e58 reads RT240D.BMP, 0x13036 bytes.
 0x248 changes to FILES; 0x28c-0x2bc reads RTYPE.FIC, 0xdc33 bytes.
RRD is covered by a complete archive sweep; its precise runtime access
pattern is not claimed to have been reconstructed.

Validation:
Built offline for 68000 using cached toolchain image 304fea9c3e8e.
Host tests exercise the actual read/check routine with injected allocation,
open, length, seek, short-read, CRC corruption, EOF, close and free faults;
all pass, with no leaked test handles/buffers. CRC32 known-vector passes.
Hardware execution remains to be tested on your Atari.

Sources/build.py/test_host.py/assets.json are included for reproducibility.
No firmware update is required.

Timing (v2): cycle_s and total_s are decimal seconds from the Atari
200 Hz system counter, read through a supervisor-mode XBIOS callback.
Resolution is 5 ms. Cycle timing includes allocation, opens, size checks,
reads, checksums, EOF, closes and frees. It excludes the progress-log
write for that cycle. Total starts after the START log and includes prior
cycle log writes. NTP/clock changes do not alter these elapsed timings.
Unsigned subtraction handles counter wrap for intervals under 248 days.
FAIL lines include partial-cycle and total timing; STOP includes total.

Open the companion RLOAD viewer HTML in a browser and select log files.
No installation, server, network access or data upload required.

Failure reporting (v3): the original failing cycle, path, stage, error and
elapsed times are preserved. The app displays the failure, waits 10 seconds
using the 200 Hz counter, then performs Fsfirst on source\*.* with a private
DTA, draining up to 4096 entries and restoring the prior DTA. This fresh
directory access allows the firmware to remount a retired TNFS session.
The failure is then appended to the original log, including probe_rc,
completed_cycles, failed_cycles and attempted_cycles. The test stops; it
does not replay the failed cycle. If recovery or logging still fails,
retain the on-screen report. No unbounded retry loop is used.

The viewer shows recorded failure rate. Only saved FAIL records contribute;
the old unsaved cycle-17 failure cannot be recovered from its log alone.
Host recovery tests cover wait order/duration/wraparound, saved original
error/timing, preserved success count, and unsuccessful recovery/log writes.
Actual recovery on the Atari remains a hardware test.

Log destination (v4): after the source-directory prompt, enter a separate
existing writable log directory. Default is P:\ (SD); C:\TESTS or another
local HDD directory also works. Use a TNFS path only if you explicitly want
network-based logging. Source reads and remount probes still target the
source directory, while every START/PASS/FAIL/STOP write uses the chosen
log directory. Subdirectories must already exist. S: is read-only.

Local/SD logs are not automatically visible on the Mac. Copy the finished
log to TNFS after testing, or transfer it via SD, then open it in the viewer.
No firmware update is included; this does not fix the discovered firmware
32-bit recovery cooldown issue after long idle periods.
