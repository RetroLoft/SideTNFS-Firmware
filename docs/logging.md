# Classified firmware logging (F30)

The former `SELOG`/`SETH:` messages now use `LOG(component, level, ...)`.
UART remains GP0, 115200 8N1. Example format (timestamp is boot milliseconds):

```
SIDETNFS 23276 [WIFI] INFO: R13D net ms=23276 state=2 attempt=1 reason=0 sd_mounted=1
```

Historical R/F markers in the message body are retained for diagnostic continuity;
the prefix identifies component/severity and the startup banner identifies the build.
This is a logging format, not a network syslog implementation.

## Defaults and classification

F30 compiles through TRACE and starts every component at INFO. Levels are cumulative:
0 OFF, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG, 5 TRACE.

- ERROR: failed mounts, exhausted transport/reply failures, and unexpected negative
  SD/TNFS operation results, including uncertain writes.
- WARN: retries, unavailable DNS, Wi-Fi failure/backoff, session retirement,
  unsuccessful NTP attempts, and newly observed SIDEETH drops/invalid packets.
- INFO: startup, drive activation, connection state changes, mounts/remounts,
  recovered retries, SIDEETH interface requests and NTP synchronization.
- DEBUG: radio internals, session parameters, protocol return codes and periodic
  service/NAT summaries. File-not-found, path-not-found, directory exhaustion and
  unsupported-operation results are diagnostic, not ERROR.
- TRACE: individual TNFS transmissions and successful filesystem operations/chunks.

Components: BOOT, GEMDOS, SD, TNFS, WIFI, NTP, SIDEETH, LEGACY. Existing bounded
retry scheduling, transition checks and periodic summary limits remain. This pass
adds no universal warning rate limiter or queue; UART emission is still synchronous.
SIDEETH drop warnings share its existing five-second summary limit and report only
when cumulative drop/invalid counts change.

## Controls

CMake cache settings accept numeric levels 0..5:

```
-DSIDETNFS_LOG_COMPILED_LEVEL=5 -DSIDETNFS_LOG_DEFAULT_LEVEL=3
```

The default cannot exceed the ceiling. `tools/build_falcon.py` defaults to
5/3 and explicitly overrides the CMake cache. To select another build policy:

```
python3 tools/build_falcon.py --log-level TRACE
python3 tools/build_falcon.py --log-level INFO --log-ceiling INFO
python3 tools/build_falcon.py --log-level OFF --log-ceiling OFF
```

The manifest records the selected settings. Each build uses the same F30 output
filename; retain separate copies/manifests when comparing verbosity variants. A build
with ceiling 3 excludes DEBUG/TRACE calls; use 0/0 to compile out classified logs.
Unrelated legacy direct `printf` output is not controlled by these thresholds.

Firmware callers may change thresholds with:

```c
sidetnfs_log_set_level(SC_TNFS, SL_TRACE);
sidetnfs_log_set_level(SC_SIDEETH, SL_WARN);
sidetnfs_log_set_all(SL_INFO);
```

Settings live in RAM, reset at boot, and cannot enable levels beyond the compiled
ceiling. Set them from the main loop, not interrupts or concurrent cores. There is
no new SIDETNFS.PRG dialog or remote command for these controls yet.

`LOG(TNFS, TRACE, "offset=%lu\r\n", offset)` checks the ceiling and threshold
before evaluating arguments or formatting. Dynamic severity uses `LOG_AT`, e.g.
`LOG_AT(TNFS, rc ? SL_ERROR : SL_INFO, "mount result=%d\r\n", rc)`.
Callers supply their newline, preserving existing split debug messages.

## Existing debugging

`DPRINTF` and `DPRINTFRAW` remain disabled unless `_DEBUG != 0`. When compiled in,
they route through LEGACY/DEBUG, retaining file/line/function context and raw
continuations. To see them, also set the LEGACY threshold (or build default) to
DEBUG. Production F30 keeps `_DEBUG=0`. Enabling TNFS TRACE does not enable these
legacy macros. The existing RAM event recorder is unchanged and remains separate;
consolidating its records/output and introducing buffering are future work.

Host tests cover argument suppression, component independence, OFF, invalid settings,
severity selection, emitted prefixes, legacy wrappers and an INFO compile ceiling.
Hardware regression and performance measurements are still required: quieter
logging changes timing, and no throughput improvement is claimed without measurement.
