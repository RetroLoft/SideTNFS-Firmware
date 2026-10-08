# Falcon integration plan

The integration branch starts at freshly cloned RetroLoft upstream main.
Exact source and tested-R16 revisions are in falcon-integration-baseline.json.
The local recovery remote and falcon-r16-tested tag provide the validated
implementation and history without modifying either recovery checkout.
F17 now implements the initial opt-in port; see firmware docs/FALCON-BUILD.md.
SDK submodules have been initialized at upstream-pinned revisions. Both build
paths compile; Falcon and ST hardware validation of the integration is pending.

## Commit sequence

1. Introduce the native guest sources and host tests as an explicit Falcon
   build option. Keep upstream's ST assembly/floppy path and default build.
   Preserve R16 resident executable identity before any functional extension.
2. Add Pico endpoint/SD/TNFS/settings/NTP modules and explicit integration
   hooks to upstream sources. Do not replace upstream gemdrvemul.c/main.c
   wholesale with the older recovery backend. Keep the Falcon option separate
   from the new upstream virtual-floppy feature until explicitly integrated.
3. Reconcile configuration protocol/utility bytes, shared-memory layouts,
   Pico SDK/RTC compatibility and flash reservations before producing a UF2.
   Firmware and guest changes must be validated as a matching pair.
4. Review upstream TNFS peer/session correlation, late-response draining,
   bounded retries, RX buffer sizing and DTA lifecycle against the native
   implementation. Existing native equivalents must not be applied twice;
   host tests should target actual native code and distinguish unresolved
   same-session sequence reuse from already-fixed peer/session filtering.
5. Run native regressions and both build paths, then repeat Falcon hardware
   checks. Atari ST validation is needed before claiming upstream compatibility.
6. Only after this baseline passes, design SIDEETH on a separate feature
   branch: serialize cartridge transport ownership and coordinate network
   servicing, then stress simultaneous GEMDRIVE I/O and SIDEETH traffic.

## Initial upstream review

Firmware upstream is v1.1.0 and has new floppy-emulation and Pico 2 W work.
Commits requiring focused review include 3188406 (TNFS correlation/wrap),
83f44e8 (response drain latency), b0a912d (bounded retries), f40b759 (DTA
registry exhaustion), and f473eb0 (shared RX buffer/chunk sizing). These are
review inputs, not claims that every change is missing from native R16.
Upstream e5e11a7 disables UART; preserve a deliberate Falcon diagnostic build
for hardware comparison rather than silently losing the recovery evidence.
GEMDRIVE now includes floppy BIOS/XBIOS hooks; the validated native guest does
not yet implement those features. Configuration utility/protocol changes also
need reconciliation against the S: utility validated in R16.

## Release boundaries

R16 remains the working hardware fallback. The long-copy timeout, explicit
DNS recovery hardware test, standalone boot activation and broader handle
semantics remain open. No new source baseline should be called hardware-tested
until the integration build has passed those applicable regressions. No fetch,
merge or push changes were made to the original/recovery repositories.
