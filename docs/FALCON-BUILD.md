# Native firmware and SIDEETH build

The native cartridge option combines SD/TNFS drives and SIDEETH IPv4 NAT.
Build it with the matching native SideTNFS-Gemdrive guest and SideTNFS-Config
utility. The legacy target remains the default. Native firmware currently
supports Pico W / RP2040; Pico 2 W is unsupported.

## Dependencies and build

Initialize the firmware SDK submodules and install the Pico ARM toolchain,
CMake, Python 3 and a host C compiler. Build the companion guest first using
`python3 tools/build_native.py` in its repository. Guest and SIDEETH builds
need Docker with a cached image containing m68k-atari-mint-gcc, vasm and
vlink; the guest also needs Python 3 inside that image. Set
`SIDETNFS_TOOLCHAIN_IMAGE` to your image name (the default local image ID
is `304fea9c3e8e`). The builders do not download toolchains.

In this firmware repository, run:

```sh
python3 tools/test_native.py
python3 tools/build_falcon.py
```

Set `SIDETNFS_GUEST_DIR` to the companion guest checkout and
`SIDETNFS_CONFIG_DIR` to the Config checkout for tests. Defaults expect
siblings named SideTNFS-Gemdrive-integration and SideTNFS-Config.
`SIDETNFS_CONFIG_PRG` selects the exact settings executable checked against
the embedded asset. `SIDETNFS_SDK_SOURCE` selects a checkout supplying SDK
submodules; `PICO_SDK_PATH`, `PICO_EXTRAS_PATH` and `FATFS_SDK_PATH` override
individual SDK paths. Outputs go to dist/falcon. Nothing flashes automatically.

To build SIDEETH.STX, set `STING_INCLUDE` to the STinG header directory and
run `python3 tools/build_sideeth.py`. Use it with the matching native guest
and firmware. Its source is in atari/sideeth.

Build Config with `make SIDETNFS.PRG` in its repository using the Atari
compiler and GEMlib. To refresh the embedded settings drive, run
romemul/download_sidetnfs_config_drive.py with `--prg`, `--readme` and
`--output romemul/native/config_drive.c`. Retain the generated C asset for
offline firmware builds.

## Runtime and testing

SIDEETH uses 192.168.254.2/24 through gateway 192.168.254.1, MTU 576 and
MSS 536. NAT supports TCP, UDP and ICMP echo with bounded queues and tables.
IPv4 fragment forwarding, forwarded ICMP errors and inbound port forwarding
are unsupported. Same-session TNFS sequence reuse remains a known limitation.

The host suite covers native guest, storage, configuration and SIDEETH
behavior. For repeatable file-read stress testing, see
[the generic RLOAD utility](../tools/rload/README.txt).
Host checks do not replace hardware validation of boot, network operation,
and settings status/edit/save/reopen on the target Atari.
