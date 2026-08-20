# SideTNFS

<img src="https://retroloft.net/images/sidetnfs/hero-sidetnfs.jpg" alt="SideTNFS" width="600">

Modern network storage for the Atari ST.

SideTNFS transforms the Raspberry Pi Pico W Sidecartridge into a network-enabled hard disk for the Atari ST family. Instead of relying on SD cards, your Atari can access files directly from a TNFS server over your local network.

No floppy swapping.  
No SD card management.  
Just power on and access your files.

---

## Features

- TNFS network storage
- Up to eight independent network and/or microSD card drives
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

### Running Your Own TNFS Server

**TNFS** (Trivial Network File System) is a lightweight network filesystem protocol built for retro computers — simple enough to run comfortably over a modest network link, and easy to implement on both server and client. It's what lets SideTNFS treat a folder on a machine elsewhere on your network as if it were a local Atari drive.

Running your own TNFS server on your LAN is the recommended way to use SideTNFS day-to-day: unlike the read-only public demo server, your own server gives you full **read and write** access, so you can save files, copy programs over, and work with your Atari the same way you would with any other network drive.

A popular, actively maintained TNFS server implementation is **tnfsd**, part of the [FujiNet](https://github.com/FujiNetWIFI) project:

https://github.com/FujiNetWIFI/tnfsd

It runs on Linux, macOS, and Windows, and only needs a folder on your computer to share — point it at that folder, start it, and then add it as a drive from **SIDETNFS.PRG** using your computer's IP address on your LAN. See that project's own documentation for installation and configuration details.

### A Note on TNFS Security

TNFS is a lightweight protocol and does not offer encryption. Don't use it to store or transfer confidential or sensitive data, whether on the public RetroLoft server or your own.

### Adding an SD Card Drive

Besides TNFS network drives, SideTNFS can also read and write directly from a **microSD card** inserted in the cartridge — handy if you'd rather not depend on the network for a particular drive, or just want a fast local drive alongside your TNFS drives.

- **Formatting:** format the card as **FAT16**, **FAT32**, or **exFAT** — FAT32 offers the widest compatibility with older Atari software, exFAT is a good choice for very large cards. A regular quick format from Windows, macOS, or Linux is enough, no Atari-specific tools needed. Use a single-partition layout; multi-partition cards are not supported.
- **Adding the drive:** from **SIDETNFS.PRG**, add a new drive and choose **SD** as its type instead of TNFS, pick a free drive letter, and enter the folder on the card that drive should point to (`/` for the whole card, or a subfolder such as `/games`). Save, then restart the Atari for the change to take effect, same as for TNFS drives.
- **Speed and compatibility:** the SD card is read over SPI at a configurable speed (`SD_BAUD_RATE_KB` in SIDETNFS.PRG, 12.5 MHz by default). If a particular card gives read/write errors or feels unreliable, try a different value here. In general, a modern, reputable-brand microSDHC/microSDXC card works best — very old or unbranded cards are more likely to cause trouble.

### Factory Reset

If your configuration ever becomes unreachable or you just want to start over, you can reset it back to the factory default at any time — no computer or SIDETNFS.PRG needed:

1. Switch the Atari off, then on again, so the Pico goes through a fresh boot.
2. Press and hold the **SELECT** button on the SideTNFS cartridge as the Atari powers back on, and keep holding it.
3. The onboard LED lights up to confirm the reset is in progress.
4. Keep holding SELECT for a full **10 seconds**. SideTNFS then rewrites its configuration back to the factory default (drive N: → the public RetroLoft TNFS server) and restarts on its own.

Releasing SELECT before the 10 seconds are up cancels the reset — nothing is changed.

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