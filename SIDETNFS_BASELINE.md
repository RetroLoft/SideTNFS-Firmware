# SIDETNFS — Baseline v1.0.1

## 1. Repository
https://github.com/sidecartridge/atarist-sidecart-raspberry-pico

## 2. Basis-tag
`v1.0.1`

## 3. Actieve branch
`sidetnfs-v101-baseline`

## 4. Commit hash
`10f1355868dfa2e3bb37d20b9b77ae23b127fb63` — "Changelog v1.0.1"
(bevestigd met `git describe --tags` → `v1.0.1`, working tree clean op broncode)

## 5. Buildstatus
Geslaagd, ongewijzigde v1.0.1-broncode.

- Submodules geïnitialiseerd en gepind conform `build.sh`:
  - `pico-sdk` → `1.5.1`
  - `pico-extras` → `sdk-1.5.1`
  - `fatfs-sdk` → `v1.2.4`
- Build uitgevoerd via `./build.sh` (board `pico_w`, build type `release`)
- Output: `dist/sidecart-pico_w.uf2` — 1.072.640 bytes

## 6. Flashstatus
UF2 gekopieerd naar de Pico in BOOTSEL-modus (`/media/*/RPI-RP2`). Board herstart
automatisch en presenteert zich daarna als opslagvolume (`SIDECART`) — teken dat de
firmware succesvol is opgestart.

## 7. Hardwaretest
- Getest op echte Atari Mega STE.
- Originele SD-GEMDRIVE werkt.
- Een `.PRG` kan gestart worden vanaf GEMDRIVE.

## 8. Testdata
- De SD/GEMDRIVE-inhoud op de Mega STE is exact gelijk aan de inhoud van de
  TNFS-servermap `Atari.ST`.
- TNFS-servergegevens voor later gebruik:
  - IP: `192.168.178.10`
  - protocol: UDP
  - port: `16384`
  - mount/root: `Atari.ST`

## 9. Referentieregel

> SD-GEMDRIVE is the reference implementation. Any change is only accepted if
> SD-GEMDRIVE still works unless the test build explicitly selects another backend.
