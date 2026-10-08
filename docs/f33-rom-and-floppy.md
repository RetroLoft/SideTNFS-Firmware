# F33 compact ROM and virtual-floppy integration review

## Checkpoint

Before this work, F32 was committed locally:

- Config: 52392bb (feature/falcon-shifted-send)
- GEMDRIVE: 0d6f187 (feature/standalone-gemdrive)
- Firmware: 0c8eb05 (feature/standalone-gemdrive)

No remote push was requested for this checkpoint. F32 includes the
Mega STE-tested configuration transport repair and DEBUG diagnostics.
Falcon cache-mask correction still needs explicit hardware regression.

## Compact trace storage

F32 stored 49,152 zero bytes in its cartridge ROM for 512 diagnostic
records. F33 keeps the 64-byte header and hook code in ROM, allocates
the same 49,542-byte recorder RAM block, copies the header, clears the
record area in RAM, then copies the hook after it. The hook's runtime
base calculation and installation pointer include the omitted records.

The existing record offset (64), capacity (512), size (96), XBRA chain,
header pointers, freeze flag and native diagnostic pointer are preserved.
No diagnostic capability is intentionally removed; Atari RAM use is not
reduced. Initialization still happens before publishing the trap vector.

ROM: 65,396 -> 16,264 bytes, net reduction 49,132 bytes. New initialization
instructions cost 20 bytes; remaining room in the 64 KiB window is 49,272.
The native executable and relocation mechanism remain unchanged apart
from the F33 display label.

The build runs tools/test_trace_layout.py: a small interpreter executes
actual emitted MOVE.W, CLR.L and DBF initialization instructions. Checks
cover all destination bytes, zeroed records, post-allocation canaries,
XBRA placement, and the emitted install/self-base offset immediates.
Hardware acceptance: cold boot with HDDRIVER and without a hard disk,
AUTO/accessories, S:/P:/TNFS/SIDEETH smoke test, and an SREC dump confirming
valid records. F33 retains DEBUG logging and the F32 config utility.

## Virtual floppy review: retained code is not a ready native feature

Reusable local sources:

- GEMDRIVE src/inc/floppy.s: Getbpb/Rwabs/Mediach hooks, XBIOS
  Floprd/Flopwr/Flopfmt/Flopver interception, A/B selection, read-only.
- Firmware romemul/sidetnfs_floppy_emul.c: SD/TNFS .ST backend, geometry
  validation, image session and candidate-image lifecycle.
- Firmware gemdrvemul.c: browse/favorites/session/sector handlers and
  SELECT image switching. Boot initializes install_floppy=false.

Dependencies that must be adapted before enabling the old hooks:

1. The assembly hooks depend on upstream send_sync, shared vector storage,
   upstream hardware flags and register conventions. Native vectors should
   live in resident RAM and share the existing cartridge exclusion gate
   with GEMDRIVE and SIDEETH. Copying the old hook file into this build
   unchanged would not provide those guarantees.
2. Shared layout must be checked against compiled firmware definitions;
   the old assembly has hard-coded offsets. Geometry/BPB should be copied
   to Atari RAM rather than returning a Pico window pointer unchecked.
3. Image-selection utility is not supplied by current S: (only SIDETNFS.PRG
   and README). No FLOPPY.PRG binary was found in this local workspace.
   A native mount/control utility or an adapted local source is needed.
4. Native Wi-Fi readiness is maintained by r11_state; the legacy floppy
   command path uses sidetnfs_network_ok and the separate upstream TNFS
   backend. TNFS image support must be reconciled with native servicing,
   recovery and timing, not assumed ready because the C files compile.
5. Late GEMDRIVE activation does not provide early floppy boot hooks.
   Boot-image support requires separate boot-order/HDDRIVER tests.
6. The upstream image backend is read-only and supports specific BPB-
   validated .ST geometries. It is not FDC-level or copy-protection emulation.

Recommended next implementation milestone: explicitly selected read-only
SD .ST image as virtual B:, with no automatic takeover of A:, and no
boot redirection. Add resident BIOS/XBIOS hooks and a small mount/unmount
utility using the native bounded transport. Verify reads, BPB, media-change,
write protection, physical-drive chaining, missing/ejected media and
reentrancy with SIDEETH. Then expand to A:/boot selection, image switching
and TNFS-backed images. F33 deliberately does not install floppy vectors.
