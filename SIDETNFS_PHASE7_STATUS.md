# SIDETNFS Phase 7 status

## 1. Current working build

Branch: `sidetnfs-v101-baseline`. Backend under active development: TNFS
(`SIDETNFS_BACKEND_TYPE == SIDETNFS_BACKEND_TNFS`). The SD/FatFS backend
(`SIDETNFS_BACKEND_SD`) remains the reference implementation and is not
knowingly regressed by any of the work below.

## 2. Build defines

```c
#define SIDETNFS_BACKEND_TYPE SIDETNFS_BACKEND_TNFS
#define SIDETNFS_DEBUG_FILE_ENABLED 1
#define SIDETNFS_DEBUG_DUMP_ON_SELECT 1
#define SIDETNFS_READDIRX_MAX_ENTRIES 1
```

Optional, temporary (default off, meant to be removed once Fread diagnosis
is no longer needed):

```c
#define SIDETNFS_DEBUG_FOCUS_FILE_IO 0
```

## 3. What works (hardware-confirmed)

- TNFS root directory listing.
- TNFS subdirectory listing.
- TNFS `Fopen` (GEMDOS mode 0 / read-only only; mode 1/2 cleanly denied).
- TNFS `Fread`.
- TNFS `Fclose`.
- Copying a TNFS file (`YAART.TXT`) to the internal hard disk — byte-identical.
- Copying `SIDETNFS.PRG` from TNFS to the internal hard disk.
- The copied `SIDETNFS.PRG` starts correctly from the internal hard disk.
- SD backend (`SIDETNFS_BACKEND_SD`) still builds and behaves as before all
  of the Fase 6/7 work (mechanical file-descriptor-table changes only, no
  behavior change to the SD code paths).

## 4. Key technical fixes so far

- Introduced `SIDETNFS_BACKEND_TYPE` (`SIDETNFS_BACKEND_SD` /
  `SIDETNFS_BACKEND_TNFS` / `SIDETNFS_BACKEND_FLASH`, the last not yet
  implemented) as the single compile-time backend switch, replacing a
  collection of ad-hoc experimental-listing/cache/repeat-Fsfirst/direct-scan
  switches from earlier phases (all removed, not left as dead code).
- Made `GEMDRVEMUL_DTA_EXIST_CALL`/`GEMDRVEMUL_DTA_RELEASE_CALL`
  backend-aware (previously FatFS-DTA-only, the actual root cause of
  `Fsnext` never being dispatched under TNFS).
- Real TNFS `CLOSEDIR` for directory handles, always sent, preventing a
  directory-handle leak across repeated Fsfirst/refresh cycles.
- All GEMDOS traps that mutate the filesystem (`Fcreate`, `Fwrite`/
  `WRITE_BUFF_CALL`, `Fdelete`, `Frename`, `Dcreate`, `Ddelete`, `Fattrib`
  set, `Fdatetime` set) are blocked with `GEMDOS_EACCDN` while
  `SIDETNFS_BACKEND_TYPE == SIDETNFS_BACKEND_TNFS`, so the TNFS backend can
  never silently fall through to a real SD/FatFS mutation.
- Corrected the TNFS file-operation command IDs after checking against the
  actual server command-dispatch table (the original guess, based on the
  same numbering family as the directory opcodes, was wrong):
  - `OPEN = 0x29` (not the deprecated `0x20`, and not the originally-guessed `0x02`)
  - `READ = 0x21` (not the originally-guessed `0x03`)
  - `CLOSE = 0x23` (not the originally-guessed `0x05`)
- `sidetnfs_tnfs_file_read()` now loops internally over
  `SIDETNFS_TNFS_READ_CHUNK_MAX`-sized (200 byte) TNFS `READ` round-trips,
  bounded by `SIDETNFS_TNFS_READ_MAX_ROUNDS` (128), until the full
  requested buffer is filled or EOF is reached — matching the SD route's
  `f_read()` contract (fill the whole requested `buff_size`, short only at
  real EOF). The earlier one-chunk-per-call behavior silently truncated any
  read request larger than 200 bytes without an error, which is what
  corrupted file copies before this fix.
- A backend-aware file-descriptor table: `FileDescriptors` (linked list)
  gained `backend` (`GEMDRIVE_FILE_BACKEND_SD`/`_TNFS`) and `tnfs_handle`
  fields, mechanically added alongside the existing `fpath`/`fd`/`fobject`/
  `next`/`offset` fields — no change to how the SD backend uses `fobject`.
- A RAM-only, SELECT-button-dump-only diagnostic eventlog
  (`sidetnfs_diag_log()`/`sidetnfs_diag_dump_on_select()`) covering
  directory listing, DTA registry, and now file-I/O
  (Fopen/Fread/Fclose) at both a routing level and (for Fread) a raw TNFS
  wire-rc level, with noise-reduction (rate-limited repeated
  `TNFS_READDIRX_EOF`, optional `SIDETNFS_DEBUG_FOCUS_FILE_IO` focus mode)
  added once the fixed-size event budget started being crowded out by
  directory-scan events.

## 5. Known limitations (not yet fixed)

- `Dsetpath` is still unconditionally hard-SD (`scfs_directory_exists()` on
  the physical SD card) regardless of backend — not yet TNFS-aware.
- `Dgetpath`/current-directory (`dpath_string`) is a single global, not
  backend-specific — interacts with the Dsetpath issue above.
- Copying a whole TNFS subdirectory (e.g. `N:\CONFIG\`) shows incorrect
  root/subdirectory behavior (TNFS-root contents appearing under the
  hard-disk root) during the copy; a manual refresh afterwards shows a more
  correct picture. Suspected to be a consequence of the Dsetpath/current-directory
  limitation above, not a new Fread defect.
- `Fseek` on a TNFS-backed handle returns `GEMDOS_EINTRN` (deliberate guard,
  not implemented).
- `Fattrib` inquiry via TNFS is not yet correct (still reads SD-side
  `scfs_stat()` regardless of backend).
- `Fdatetime` inquiry via TNFS is not yet correct (same `scfs_stat()`
  limitation as Fattrib).
- `GEMDRVEMUL_DFREE_CALL` has a pre-existing missing-`break` fall-through
  bug into `GEMDRVEMUL_DGETPATH_CALL` (present before this work, not fixed).
- TNFS write/create/delete/rename are not implemented (deliberately denied,
  see above).
- `SIDETNFS_BACKEND_FLASH` is not implemented (compile-time `#error` guard).
- Multi-drive is not implemented.

## 6. Next recommended phase

**Fase 7E — make Dsetpath/Dgetpath/current-directory backend-aware.**

Motivation: directory-copy of `N:\CONFIG` shows root/subdir confusion;
`Dsetpath` still uses `scfs_directory_exists()` against the physical SD
card; the current-directory string is a single global not scoped per
backend. This is the most likely explanation for the copy-directory
observation in Fase 7D6's test, and blocks reliably testing
`SIDETNFS.PRG`-from-a-subdirectory execution.
