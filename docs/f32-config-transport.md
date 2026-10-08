# F32 configuration transport repair

F31 Mega STE evidence: the utility timed out GET_DRIVE slot 7 (transport
value 1). UART logged successful slots 0 through 6 and no slot-7 command.
Other launches completed all eight slots. The capture does not contain
the failing token; it cannot prove which raw address was intercepted.

The ST utility sent arbitrary 16-bit words as cartridge addresses. The
DMA dispatcher offers those addresses to native GEMDRIVE and SIDEETH
before the config parser. Their reserved ranges consume matching words,
even when no packet is pending. The random handshake token can therefore
lose a word, leaving the configuration request incomplete.

F32 uses the existing shifted configuration codec on ST/STE as well as
Falcon: header A5A5, high byte then low byte at 7E00 + (byte << 1).
The firmware already accepts this codec. No parser, retry, timeout,
saved-setting, or SIDEETH protocol change is required. Use the paired
utility/firmware; older upstream firmware is not claimed compatible.

The transport codec no longer chooses whether to execute MOVEC. The
cache helper requires both Falcon _MCH and _CPU=30; missing CPU cookies,
68000, 68040 and 68060 skip it. This does not implement accelerated
Falcon cache support. Supexec enters supervisor mode, MOVEC reads CACR,
OR adds CD bit 11 (00000800), and MOVEC writes it back. Other control
bits are preserved. The previous 00000008 was instruction-cache clear,
not data-cache clear. Reference: Motorola MC68030 User Manual section
6.3.1.3, https://www.nxp.com/docs/en/reference-manual/MC68030UM-P1.pdf.
The helper runs before reading the token seed and before each echo poll;
it does not permanently disable caches or touch Mega STE cache hardware.

Validation: host test uses actual native endpoint collectors and config
parser, reproduces swallowed raw tokens at reserved addresses, then
passes all 65536 token-half values with both odd ST/even Falcon headers.
It also checks cache policy for 68000, 68030, 68040, 68060 and missing
CPU cookie. Full host suite and 68000 cross compilation accompany build.
Hardware acceptance: repeated S: utility opens on Mega STE with UART;
then Falcon utility, NTP status, drive access and SIDEETH regression.
DEBUG diagnostics remain enabled; TRACE remains off.
