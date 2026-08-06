# SideTNFS

Modern network storage for the Atari ST.

SideTNFS transforms the Raspberry Pi Pico W Sidecartridge into a network-enabled hard disk for the Atari ST family. Instead of relying on SD cards, your Atari can access files directly from a TNFS server over your local network.

No floppy swapping.  
No SD card management.  
Just power on and access your files.

---

## Features

- TNFS network storage
- Multiple network drives
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

From this disk you can run **SIDETNFS.PRG** to configure your cartridge. Settings are stored inside the Pico's internal flash memory and remain available after power-off.

---

## Planned Features

- Floppy drive emulation
- Advanced real-time clock (RTC) with automatic network time synchronization and time zone support
- Optional ROM support if there is sufficient community interest
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

https://discord.gg/yuuE9wbt6E

Bug reports, ideas and feature requests are always welcome.

---

## Acknowledgements

SideTNFS is based on the excellent Atari Sidecartridge project and extends it with integrated TNFS support and modern network functionality.

Many thanks to everyone involved in the original Sidecartridge project and to everyone helping test and improve SideTNFS.