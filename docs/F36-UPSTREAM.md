# F36 integration and F37 configuration candidate

These are coordinated source submissions to RetroLoft/SideTNFS-Firmware,
RetroLoft/SideTNFS-Gemdrive and RetroLoft/SideTNFS-Config. Firmware and
GEMDRIVE use the native protocol together; use the matching Config changes.
The legacy upstream build remains the default. Historical Falcon names in
build switches also cover the tested Mega STE setup. Native firmware is
restricted to Pico W / RP2040; Pico 2 W is not claimed.

F36 was hardware-tested by the user on Mega STE: HighWire launches from the
TNFS mapping after accepting its 0x17 PRG flags. Earlier checkpoints cover
Falcon/HDDRIVER/BlueSCSI coexistence and Mega STE standalone boot. These are
specific observations, not a claim that every configuration is validated.
F37 embeds Config rebased onto current upstream, retaining upstream's wait
notice and initial Wi-Fi setup alongside our cartridge transport and AES
window fixes. F37 settings/status/edit/save/reopen needs hardware validation.

Historical docs describe individual bring-up stages; this document states
current submission scope. Raw UART captures and old binary kits are excluded.

## Firmware scope

Native cooperative SD/TNFS endpoint, settings drive, NTP, bounded recovery,
resource diagnostics, and SIDEETH IPv4 NAT over Wi-Fi. Uncertain writes are
not blindly replayed. The Atari uses 192.168.254.2/24 via 192.168.254.1;
MTU 576, MSS 536, bounded queues and translation entries. No IPv4 fragment
forwarding, forwarded ICMP errors or incoming port forwarding. Same-session
TNFS sequence reuse after 256 sequence values remains a known limitation.

## Build and test

Initialize this repository's SDK submodules. Build the companion guest first.
Use python3 tools/test_native.py and python3 tools/build_falcon.py.
The scripts accept SIDETNFS_GUEST_DIR (guest checkout), SIDETNFS_CONFIG_DIR
(config checkout for tests), SIDETNFS_CONFIG_PRG (exact embedded PRG for build
verification), and SIDETNFS_SDK_SOURCE (checkout supplying SDK submodules).
PICO_SDK_PATH, PICO_EXTRAS_PATH and FATFS_SDK_PATH can override each SDK path.
Defaults expect sibling SideTNFS-Gemdrive-integration and SideTNFS-Config.
Host tests need a C compiler. Guest/STX builds use an offline cached Docker
image with m68k-atari-mint-gcc, vasm and vlink; set SIDETNFS_TOOLCHAIN_IMAGE
to your equivalent image (the historical local default is 304fea9c3e8e).
For tools/build_sideeth.py also set STING_INCLUDE to STinG's include directory.

To refresh native/config_drive.c, build the companion SIDETNFS.PRG, then run
romemul/download_sidetnfs_config_drive.py with --prg, --readme and --output
romemul/native/config_drive.c. Commit the generated asset for offline builds.
The unchanged legacy target retains its existing configuration asset.

Validation: full native host regressions, guest ROM relocation/copy/clear
checks and native firmware build pass. PR preparation reproduced F36's UF2
SHA256 75623b96387ad41fcd5b401a798dfd898f9050beea44fbc35bb691e8d6b82213
before the F37 Config refresh. F37 is a new hardware-test candidate.
