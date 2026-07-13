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
