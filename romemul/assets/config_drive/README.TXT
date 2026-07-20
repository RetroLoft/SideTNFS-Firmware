SideTNFS Configuration Drive
=============================

Firmware release    : v1.0.1
Config tool release : SIDETNFS.PRG, see the SideTNFS-Config releases page
Document build date : 2026-07-20

Firmware source:
  https://github.com/RetroLoft/SideTNFS-Firmware

Configuration program source:
  https://github.com/RetroLoft/SideTNFS-Config

WHAT IS THIS DRIVE?
--------------------
This is the SideTNFS configuration drive. It is provided by the
Pico firmware itself, so it is always available, even before any
SD card or TNFS server has been set up.

Run SIDETNFS.PRG from this drive to configure:
  - the drive letter used by this configuration drive itself;
  - the SD and/or TNFS drives that GEMDRIVE emulates.

SAVING CHANGES
--------------
Changes you make and save in SIDETNFS.PRG are written into the
Pico's own flash memory, so they are kept across a power cycle.

Some changes take effect immediately. Others only become active
after you reset the Atari, because that is when the configuration
is safely re-read and the active drives are re-selected.

COMING LATER
-------------
A future CONFIG button is planned to also manage settings such as
the Wi-Fi network, among other things. This is NOT implemented
yet and is not available in this release.

