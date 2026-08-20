# SideTNFS

Modern network storage for the Atari ST.

SideTNFS transforms the Raspberry Pi Pico W Sidecartridge into a network-enabled hard disk for the Atari ST family. Instead of relying on SD cards, your Atari can access files directly from a TNFS server over your local network.

No floppy swapping.  
No SD card management.  
Just power on and access your files.

---

## Features

- TNFS network storage
- Up to eight independent network drives
- Built-in read-only Settings Disk
- Configuration utility running directly on the Atari ST
- Read and write support
- Works with original GEMDOS software
- Open source

---

## Supported Systems

Current development is focused on:

- Atari ST
- Atari STF
- Atari STE
- Mega ST
- Mega STE
- Atari TT030

Falcon support is planned if technically feasible.

---

## Hardware

SideTNFS runs on the Raspberry Pi Pico W version of the Atari Sidecartridge project.

---

## Configuration

The firmware always provides a built-in read-only **Settings Disk**.

From this disk you can run **SIDETNFS.PRG** to configure your cartridge — add, edit, disable, or remove network drives, change Wi-Fi settings, and more. Settings are stored inside the Pico's internal flash memory and remain available after power-off. Installing a firmware update never overwrites an existing, saved configuration.

### Default Server

Out of the box, drive **N:** is pre-configured to connect to a public **RetroLoft TNFS server**, meant for quick tests and demonstrations of SideTNFS — no setup required to get started.

- The server is **read-only**: you can browse, open, and download files, but you cannot modify, upload, rename, or delete anything on it.
- This default drive is just a starting point. From **SIDETNFS.PRG** you can change its server settings, disable it, or remove it entirely, and configure your own TNFS server(s) instead.

### Factory Reset

If your configuration ever becomes unreachable or you just want to start over, you can reset it back to the factory default at any time — no computer or SIDETNFS.PRG needed:

1. Switch the Atari off, then on again, so the Pico goes through a fresh boot.
2. Press and hold the **SELECT** button on the SideTNFS cartridge as the Atari powers back on, and keep holding it.
3. The onboard LED lights up to confirm the reset is in progress.
4. Keep holding SELECT for a full **10 seconds**. SideTNFS then rewrites its configuration back to the factory default (drive N: → the public RetroLoft TNFS server) and restarts on its own.

Releasing SELECT before the 10 seconds are up cancels the reset — nothing is changed.

### A Note on TNFS Security

TNFS is a lightweight protocol and does not offer encryption. Don't use it to store or transfer confidential or sensitive data, whether on the public RetroLoft server or your own.

---

## Planned Features

- Floppy drive emulation
- Advanced real-time clock (RTC) with automatic network time synchronization and time zone support
- Falcon support where technically possible

---

## Downloads

The latest firmware, documentation and configuration utility can always be found at:

https://retroloft.net/sidetnfs

[How to update the SideTNFS firmware](UPDATE.md)

---

## Community

Website

https://retroloft.net/sidetnfs

GitHub

https://github.com/RetroLoft

Discord

https://discord.gg/SaSaAdxfKe

Bug reports, ideas and feature requests are always welcome.

---

## Acknowledgements

SideTNFS is based on the excellent Atari Sidecartridge project and extends it with integrated TNFS support and modern network functionality.

Many thanks to everyone involved in the original Sidecartridge project and to everyone helping test and improve SideTNFS.