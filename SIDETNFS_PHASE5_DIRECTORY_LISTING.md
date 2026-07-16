# SIDETNFS — Phase 5: TNFS directory listing (Fsfirst/Fsnext)

Status: first stable working state, reached in Fase 5AA/5AB.

## Working build configuration

```c
#define SIDETNFS_EXPERIMENTAL_FS_LISTING 1
#define SIDETNFS_DEBUG_FILE_ENABLED 1
#define SIDETNFS_DEBUG_DUMP_ON_SELECT 1
#define SIDETNFS_FS_DIAG_ENABLED 1
#define SIDETNFS_DIR_CACHE_ENABLED 0
#define SIDETNFS_DIRECT_SCAN_ENABLED 1
#define SIDETNFS_READDIRX_MAX_ENTRIES 1
#define SIDETNFS_FSFIRST_REPEAT_CONTINUE 0
#define SIDETNFS_TNFS_CLOSEDIR_ENABLED 1
```

All of the above are the current header defaults in
`romemul/include/sidetnfs_probe.h` — no build-system overrides are
required to reproduce this state.

## What works

- TNFS root listing works (all entries visible, not just the first one).
- TNFS subdirectory listing works.
- Refresh (ESC and re-open) works repeatedly, without the listing
  degrading or emptying out over time.
- Directory entries no longer disappear after repeated opening/refreshing.
- No SD/FatFS directory listing is used at all while
  `SIDETNFS_EXPERIMENTAL_FS_LISTING == 1` — `Fsfirst`/`Fsnext` never call
  `f_findfirst()`/`f_findnext()`, never read SD directory entries, and
  never fall back to SD for any reason (missing path, timeout, network
  error).

## The working structure

```text
Fsfirst:
  OPENDIRX
  READDIRX max_entries=1 until first match
  TNFS_DTA_INSERT (registers the search under ndta)
  fill DTA
  return OK

DTA_EXIST:
  sees TNFS DTA-registry state (and the fake no-network search table)
  returns ndta if a search is active

Fsnext:
  TNFS_DTA_LOOKUP_OK (looks up the search by ndta)
  READDIRX max_entries=1 until next match
  fill DTA
  return OK

DTA_RELEASE / EOF / Fsfirst replacing an existing search:
  CLOSEDIR (closes the TNFS directory handle on the server)
  TNFS_DTA_RELEASE (local bookkeeping cleared)
```

This mirrors the proven-working SD/FatFS backend's own shape:
`insertDTA()`/`lookupDTA()`/`releaseDTA()`, keyed by `ndta`. The TNFS side
has its own registry (`insertTnfsDTA()`/`lookupTnfsDTA()`/
`releaseTnfsDTA()` in `sidetnfs_probe.c`) instead of reusing the FatFS
table — never a FatFS `DIR*`/`FILINFO*` for TNFS searches — but the
insert-on-Fsfirst / lookup-on-Fsnext / release-on-EOF contract is the
same.

## Crucial fixes that got this working

1. **`DTA_EXIST` now sees TNFS-DTA state** (Fase 5Z). Before this, the 68k
   side would query `GEMDRVEMUL_DTA_EXIST_CALL` right after `Fsfirst` to
   decide whether to bother issuing a real `Fsnext` at all — but
   `DTA_EXIST` only ever checked the FatFS DTA table (`lookupDTA()`),
   which TNFS `Fsfirst` never populates. This was the reason real
   `Fsnext` calls never reached `GEMDRVEMUL_FSNEXT_CALL` in any earlier
   phase, no matter how the TNFS search itself was implemented (cache,
   live-search, or DTA-registry).
2. **`DTA_RELEASE` now releases TNFS-DTA state too** (Fase 5Z), so the
   68k-side "did release actually happen" bookkeeping stays consistent
   for TNFS the same way it does for SD.
3. **TNFS directory searches are keyed by `ndta`** (Fase 5Y), the same way
   SD's `insertDTA`/`lookupDTA` are — replacing earlier cache-slot and
   unregistered "live search" models that worked by coincidence for a
   single search but didn't generalize.
4. **`READDIRX` uses `max_entries=1`** (Fase 5W/5Y) — one GEMDOS call in,
   one directory entry out, deliberately as close as possible to
   `f_findfirst()`/`f_findnext()`'s own one-entry-at-a-time contract.
   No batch fetching, no entry cache.
5. **`CLOSEDIR` closes TNFS directory handles** (Fase 5AA) on EOF, on
   `Fsnext` exhaustion, on `DTA_RELEASE`, on a repeated `Fsfirst`
   replacing an existing search, and on error. Without this, every
   `Fsfirst`/refresh/subdirectory-open leaked one open directory handle
   on the TNFS server; after enough leaked handles the server/session
   stopped returning usable listings at all — exactly the "works at
   first, then everything goes empty" symptom seen before this fix.

## Why not a directory cache

The cache-based approach (Fase 5O–5R) could fetch and store entries
correctly, but didn't fit cleanly onto the existing GEMDOS/DTA/Fsnext
flow — the mismatch was architectural (batching directory entries in RAM
vs. GEMDOS's own one-entry-per-call contract), not a bug that could be
patched. The stable solution instead follows the already-proven-working
SD structure: `Fsfirst` registers search state on `ndta`, `Fsnext` looks
that state up via `ndta`, and each call does exactly one round of TNFS
I/O. No RAM-resident directory cache exists in this path.

## What deliberately doesn't work yet

- `Fopen`/`Fread`/`Fwrite`/`Fcreate`/`Fdelete`/`Frename` are still
  SD/FatFS-backed — TNFS is not wired into file I/O yet, only directory
  listing.
- Opening/launching a `.PRG` found via the TNFS listing may still depend
  on SD for the actual file read.
- No long-filename mapping — only names that are already valid 8.3
  GEMDOS names are shown; anything else is skipped.
- No `~1`-style shortname generation for long names.
- No TNFS write/delete/rename.
- No directory cache, no batch-fetching optimization (`READDIRX` is
  deliberately `max_entries=1`).

## Diagnostics

- `DEBUG.TXT` is written only on SELECT-button press (edge-triggered) —
  never automatically, never during `Fsfirst`/`Fsnext`, never from a
  network callback.
- No SD writes happen during `Fsfirst`/`Fsnext` themselves.
- The RAM-only diagnostic eventlog (`SIDETNFS_FS_DIAG_ENABLED`) and the
  SELECT-dump mechanism remain enabled in this build; they can be
  disabled independently later without affecting the directory-listing
  logic itself.

## Next steps (not started)

- Wire TNFS into `Fopen`/`Fread`/`Fwrite` (and the rest of file I/O).
- Decide on long-filename handling, if needed.
- Consider whether/when to disable the diagnostic eventlog and SELECT
  dump for a non-diagnostic release build.
