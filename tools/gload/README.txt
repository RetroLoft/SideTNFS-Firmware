GLOAD: read-only GEMDOS file stress test
======================================

Build and prepare (Python 3 and Docker required):
  python3 generate_fixtures.py
  python3 build.py

The builder uses a local image containing m68k-atari-mint-gcc, vasm and
vlink. Set SIDETNFS_TOOLCHAIN_IMAGE to your toolchain image name; the
fallback image ID is for an existing local toolchain. No image is pulled.

Copy the contents of fixtures/ to an Atari-accessible GLOAD directory,
preserving its FILES subdirectory. Run GLOAD.TOS and enter that directory
(default M:\GLOAD). Choose a separate writable directory for the logs;
P:\ is the default, but a local disk is preferable when testing a network
drive. Press ESC to stop between files or cycles.

The six generated files are 3,678, 13,448, 61,104, 77,878, 56,371 and
224,092 bytes: 436,571 bytes per cycle. The last two are in FILES/.
They exercise small and large reads, including reads above 64 KB, with
about 224 KB needed for the largest buffer. No game assets are required.

Each file contains deterministic SHA-256 counter blocks, unique to its
file number and block position. assets.json records lengths, CRC32 and
SHA-256; assets.h supplies the Atari checker's expected lengths and CRCs.
The generator checks the manifest and refuses to replace differing files.
Generated binaries are excluded from Git; anyone can reproduce them.

Each cycle opens each file read-only, checks its length, rewinds, reads it
in one GEMDOS Fread, checks IEEE CRC32 and EOF, then closes and frees it.
This tests file transport, not executable loading or game compatibility.
Logs use the first available GLOAD000.LOG through GLOAD999.LOG and never
overwrite an existing log. START, PASS, FAIL and STOP include timing.

On the first error, GLOAD retains the original stage and return code,
waits ten seconds, probes the source directory with a private DTA,
restores the previous DTA, logs the original failure and stops. It does
not retry the failed file or hide the failure behind a later success.

Host checks (Python 3 and a C99 compiler named cc):
  python3 test_host.py
  python3 test_recovery.py
  python3 test_fixtures.py

These check the loader's success and failure paths, cleanup, CRCs,
recovery timing and generated fixture consistency. Hardware testing is
still required for the actual GEMDOS and network transport.
