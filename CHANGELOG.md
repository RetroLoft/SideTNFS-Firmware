# Changelog

All notable changes to SideTNFS are documented here. Older entries here describe the SideTNFS firmware itself, not the original Sidecartridge project it's built on.

## [Unreleased]

## [1.1.0] - 2026-09-07

### Added
- Floppy drive emulation: FLOPPY.PRG can now load an ordinary `.ST` disk image from a TNFS server or microSD card and present it to the Atari as a real, bootable virtual floppy drive, using standard GEMDOS/BIOS disk vectors -- no physical floppy drive needed. Includes a persistent, up-to-60-slot Favorites list and a temporary, up-to-8-image Carousel for multi-disk games; a short press of the cartridge's SELECT button switches to the next Carousel entry while the game is running. Floppy emulation can run on its own ("Floppy Only") or alongside the normal SideTNFS TNFS/SD drives.
- Choice of emulated drive letter, A: (the default) or B:, picked per Carousel session from FLOPPY.PRG's Start dialog.
- A long SELECT press exits floppy mode, confirmed by the onboard LED; the user then does a manual Atari reset to return to normal SideTNFS operation.

### Changed
- FLOPPY.PRG's Sources/Favorites/Carousel architecture reworked: Browser Sources are now local-only shortcuts for browsing, while each Favorite and Carousel entry carries its own complete backend/server/path. A single Favorites list or Carousel can therefore freely mix images from different TNFS servers and a microSD card at once, rather than being tied to one shared source.

### Fixed
- A directory-corruption bug in the virtual floppy drive's BPB calculation (`fatrec`/`bflags`) that could scramble the emulated floppy's root directory.
- TNFS DTA-registry exhaustion that could empty drive M: while FLOPPY.PRG saved a Favorite.
- TNFS RX buffers unified into a single 768-byte buffer (read chunk size restored to 512) for more consistent network reads.

## [1.0.5] - 2026-08-29

### Added
- Raspberry Pi Pico 2 W (RP2350) support alongside the Pico W, including the higher overclock and PIO timing it needs. Both boards use the same firmware source; `build.sh` picks the right one.
- FLOPPY.PRG: a companion Atari-side tool for browsing real TNFS/SD directories by their actual long filenames (no more 8.3 conversion) and managing a small set of saved server profiles, so switching between floppy-image collections doesn't require reconfiguring SideTNFS itself.

### Fixed
- TNFS file and directory operations (opening, reading, writing, closing, seeking, renaming, deleting, listing) could fail outright on a single dropped or delayed network packet, with no retry -- on a large file this meant one bad moment anywhere across hundreds of read round trips could abort the whole load ("An error occurred while reading from the source file"), even though the file itself was never corrupted. Every TNFS operation now retries a few times before giving up.

## [1.0.4] - 2026-08-24

### Added
- README sections on running your own TNFS server and on adding a microSD card drive, plus a link to the community Discord server.

### Fixed
- "Illegal Instruction" crashes (four bombs) that could occur while loading a large .PRG file from a TNFS drive. The cause: TNFS file-listing responses (open/read/write/seek/close and directory operations) were correlated to their request using only the command byte and an 8-bit sequence counter shared across the whole channel, ignoring the source address, port and TNFS session id. A response could arrive late enough to be misattributed to a later, unrelated request once the 8-bit sequence space wrapped around -- something a large program's 600-800+ read round trips did repeatedly, silently splicing stale bytes into the file being loaded. Responses are now also validated against the exact server address, port and session id they're expected to come from.

### Removed
- The ROM-cartridge-emulation feature and the online ROM catalog it depended on (the `roms.sidecartridge.com` service is no longer online). SideTNFS only ever uses GEMDRIVE with a TNFS or SD card backend.

## [1.0.3] - 2026-08-20

### Added
- Firmware can check for a newer release on demand and show an in-app notice ("Firmware Update Available") from SIDETNFS.PRG's main window, listing the installed and latest version numbers, with a link to the update guide.
- SIDETNFS.PRG shows a "please wait" notice while it loads the cartridge's configuration at startup, and opens the WiFi setup screen automatically when no network is configured yet, instead of leaving the screen blank or expecting the user to find the CONFIG button themselves.
- The boot screen now shows "[NA] Not available!" instead of a generic "[KO] Timeout!" when the firmware already knows there's no WiFi to connect to (no SSID configured, or every connect attempt already exhausted) -- so the message matches what actually happened instead of implying something timed out.

### Changed
- The default (factory) TNFS drive now points to the public RetroLoft TNFS server (`retroloft.net`) instead of a local IP address, so a fresh cartridge works out of the box without any network setup. See the README's "Default Server" section for details.
- The default WiFi security type is now WPA2/AES instead of Open -- the vast majority of home networks use WPA2, and defaulting to Open on a blank configuration just guaranteed the first real connection attempt would fail anyway.

### Fixed
- The firmware-update check above uses plain HTTP against `retroloft.net`. An earlier version used HTTPS against GitHub directly; real-hardware testing found that handshake failing against GitHub's CDN, so it was replaced before this feature ever shipped in a release.
- A blank/factory-reset configuration no longer wastes up to ~15 seconds retrying a WiFi connection the firmware already knew would fail instantly (no SSID configured). The Atari's own boot-time network-wait countdown had the same problem one level up -- it always ran its full local timeout regardless of how fast the Pico had already given up -- and is fixed the same way.
- The firmware-update check no longer attempts a DNS lookup when WiFi isn't connected, instead of letting that fail slowly on its own.

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
