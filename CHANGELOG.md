# Changelog

All notable changes to SideTNFS are documented here. Older entries here describe the SideTNFS firmware itself, not the original Sidecartridge project it's built on.

## [Unreleased]

### Changed
- The default (factory) TNFS drive now points to the public RetroLoft TNFS server (`retroloft.net`) instead of a local IP address, so a fresh cartridge works out of the box without any network setup. See the README's "Default Server" section for details.

### Added
- Firmware can check for a newer release on demand and show an in-app notice ("Firmware Update Available") from SIDETNFS.PRG's main window, listing the installed and latest version numbers, with a link to the update guide.

### Fixed
- The update check above now uses plain HTTP against `retroloft.net`. An earlier version used HTTPS against GitHub directly; real-hardware testing found that handshake failing against GitHub's CDN, so it was replaced before this feature ever shipped in a release.

## [1.0.2] - 2026-08-06

### Fixed
- **Large TNFS directories could report "file not found" for files that clearly existed.** An exact (non-wildcard) `Fsfirst` lookup has to walk every preceding non-matching directory entry within a single GEMDOS call; that walk was bounded to 32 TNFS round-trips, so looking up a file past the 32nd entry in a directory failed with `EFILNF` even though `Fopen`/`Fread` could read that same file just fine. The bound is now 512.
- The SELECT-button diagnostic eventlog was being recorded on every GEMDOS call in every build, including Production, even though only a Debug build could ever dump it to a file. It's now fully compiled out of Production builds.

### Changed
- Boot-time NTP synchronization now waits up to 15 seconds instead of 5, giving slower networks more time to sync before the clock falls back to "Not Synchronized".
- Collapsed the firmware build variants down to exactly two: Production and Debug.
- Removed a temporary diagnostic module (`mindiag`) added for an isolated copy-failure investigation; no longer needed.

## [1.0.1] - 2026-07-27

First stable release of SideTNFS: turns the Atari Sidecartridge's Raspberry Pi Pico W hardware into network-attached storage for the Atari ST family over TNFS.

### Added
- TNFS network storage over Wi-Fi, with up to eight independent network drives.
- Built-in read-only Settings Disk and on-Atari configuration utility (SIDETNFS.PRG) for managing drives and Wi-Fi settings.
- Drive configuration persisted in the Pico's internal flash memory, surviving power-off and firmware updates.
