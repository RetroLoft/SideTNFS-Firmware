# SIDETNFS — Progress

## Fase 4A — scfs_directory_exists wrapper

- Added minimal SCFS wrapper layer.
- Added scfs_directory_exists().
- scfs_directory_exists() forwards to existing FatFS-backed directory_exists().
- GEMDRIVE Dsetpath, Dcreate and Ddelete now call scfs_directory_exists().
- No TNFS added.
- No backend selection added.
- Mega STE hardware test passed.
- SD-GEMDRIVE still works.
- Launching .PRG from GEMDRIVE still works.

## Fase 4B — local path wrapper in GEMDRIVE

- Added local wrapper `scfs_get_local_full_pathname()` inside `gemdrvemul.c`.
- Wrapper forwards to existing static `get_local_full_pathname()`.
- Wrapper intentionally remains in `gemdrvemul.c` because the original function depends on file-local/global state:
  - `payloadPtr`
  - `dpath_string`
  - `hd_folder`
- Replaced only Dcreate and Ddelete call-sites.
- Did not touch Fopen, Fcreate, Fdelete, Fattrib, Frename, Fsfirst, Fsnext or DTA handling.
- No TNFS added.
- No backend selection added.
- Mega STE hardware test passed.

## Fase 4C — Dfree / disk-info wrapper

- Added `ScFsDiskInfo`.
- Added `scfs_get_disk_info()`.
- Moved direct `f_getfree()` usage out of `gemdrvemul.c` into `scfs.c`.
- `GEMDRVEMUL_DFREE_CALL` now uses `scfs_get_disk_info()`.
- Values written to the Atari-side Dfree structure remain unchanged:
  - free clusters
  - total clusters
  - bytes per sector
  - sectors per cluster
- No TNFS added.
- No backend selection added.
- Mega STE hardware test passed.
- Baseline comparison confirmed that "Show Information" on GEMDRIVE drive/folders was already not working in original v1.0.1, so this is not a Fase 4C regression.

## Fase 4D — FatFS to GEMDOS error mapping helper

- Added central helper for mapping FatFS `FRESULT` values to GEMDOS error codes.
- Helper is used only in the first low-risk handler(s) selected during this phase.
- No TNFS added.
- No backend selection added.
- No Fsfirst/Fsnext/DTA changes.
- No Fopen/Fread/Fwrite changes.
- Mega STE hardware test passed.
- Normal GEMDRIVE usage still works.
- Creating and deleting a directory still works.
- Error-path testing for deleting a non-existing directory is deferred to a later small test program.

## Fase 4E — minimal SCFS stat wrapper

- Added `ScFsStat`.
- Added `scfs_stat()`.
- `scfs_stat()` currently forwards to FatFS `f_stat()`.
- `Fattrib` inquiry path now uses `scfs_stat()` for reading attributes.
- `Fattrib` set path still uses the original `f_chmod()` logic.
- `directory_exists()` was intentionally not changed to avoid reversing the layering between `filesys.c` and `scfs.c`.
- No TNFS added.
- No backend selection added.
- No Fsfirst/Fsnext/DTA changes.
- No Fopen/Fread/Fwrite changes.
- Mega STE hardware test passed.

## Fase 4F — Fdatime inquiry via SCFS stat

- `GEMDRVEMUL_FDATETIME_CALL` inquiry path now uses `scfs_stat()`.
- `ScFsStat.date` and `ScFsStat.time` replace direct use of `FILINFO.fdate` and `FILINFO.ftime` in the inquiry path.
- The Fdatime set path still uses the original `f_utime()` logic.
- No `scfs_utime()` added.
- No TNFS added.
- No backend selection added.
- No Fsfirst/Fsnext/DTA changes.
- No Fopen/Fread/Fwrite changes.
- Mega STE hardware test passed.

## Fase 5A — UDP probe to TNFS server

- Added a minimal UDP probe to `192.168.178.10:16384`.
- Probe payload is `SIDETNFS_PROBE`.
- Probe is sent only after WiFi is successfully available in the existing GEMDRIVE/WiFi path.
- No TNFS client added.
- No MOUNT/OPENDIR/READDIR/OPEN/READ added.
- No UART/debug/ringbuffer added.
- Normal releasebuild used.
- Server confirmed receipt with tcpdump.
- Server also sent a UDP response.
- GEMDRIVE backend was not changed.

Server tcpdump output:

```text
15:26:46.361174 enp2s0 In  IP 192.168.178.206.50752 > 192.168.178.10.16384: UDP, length 14
15:26:46.361433 enp2s0 Out IP 192.168.178.10.16384 > 192.168.178.206.50752: UDP, length 5
```
