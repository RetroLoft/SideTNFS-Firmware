# F19 TNFS recovery and capture candidate

F19 uses the F18 native Atari ROM, with firmware-side TNFS recovery and
request diagnostics. It does not claim to fix the cause of missing replies.
The settings README now identifies the v1.1.0 integration base correctly;
the tested configuration executable is unchanged. Keep F17 available.

1. Start the TNFS-server packet capture and UART capture before cold boot.
   Flash sidetnfs_f19_falcon.uf2; confirm `F19 upstream native Falcon`.
   Keep mappings, Wi-Fi, native boot disk and AUTO configuration unchanged.
   Keep SIDEETH/SCSILINK disabled.
2. Wait for Wi-Fi up. Repeatedly Show the same small text file from O:.
   After an error, close the window, wait at least two seconds, then reopen
   the drive/file. Record the action and approximate time. Do not reboot
   before capturing whether the drive recovers. Repeat on M: separately.
3. Expected recovery: current operation fails, UART prints `F19 TNFS retire`,
   then a later access prints `F19 TNFS remount` and a new SESSION. Searches
   in the retired session are invalid: restart the listing, not Fsnext.
   The original operation is never automatically replayed in a new session.
4. Test R-Type loading only after the small-file capture. Then run unchanged
   R14ATEST.TOS from the native disk after NTP sync. Save the generated
   C:\R14ATEST.LOG, not the executable, with the complete matching UART.
5. Stop/flush the packet capture before copying it. Supply both successful
   and failed runs, including the result of reopening after a failure.

Writes/create/delete/rename with ambiguous failures remain blocked until
restart. Do not automatically repeat an uncertain mutation; inspect the
server first. Read-only recovery retires the UDP endpoint and server search
handles, while retaining the guest mapping token (file RPCs are path/offset
based). The daemon may retain abandoned sessions until its own expiry.

`F19 TX` logs guest op, offset, TNFS session, sequence, command, packet size
and remaining operation budget. This diagnostic build adds UART overhead.
Same-session sequence wrap and the underlying reply losses remain open.
