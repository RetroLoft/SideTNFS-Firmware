#ifndef COMMANDS_H_
#define COMMANDS_H_

// The commands code is the combinatino of two bytes:
// - The most significant byte is the application code. All the commands of an app should have the same code
// - The least significant byte is the command code. Each command of an app should have a different code
#define APP_CONFIGURATOR 0x00 // The configurator app
#define APP_FLOPPYEMUL 0x02   // The floppy emulator app
#define APP_RTCEMUL 0x03      // The RTC emulator app
#define APP_GEMDRVEMUL 0x04   // The GEMDRIVE app.

// APP_CONFIGURATOR commands
#define GET_CONFIG 3            // Get the configuration of the device
#define PUT_CONFIG_STRING 4     // Put a configuration string parameter in the device
#define PUT_CONFIG_INTEGER 5    // Put a configuration integer parameter in the device
#define PUT_CONFIG_BOOL 6       // Put a configuration boolean parameter in the device
#define SAVE_CONFIG 7           // Persist the configuration in the FLASH of the device
#define RESET_DEVICE 8          // Reset the device to the default configuration
#define LAUNCH_SCAN_NETWORKS 9  // Launch the scan the networks. No results should return here
#define GET_SCANNED_NETWORKS 10 // Read the result of the scanned networks
#define CONNECT_NETWORK 11      // Connect to a network. Needs the SSID, password and auth method
#define GET_IP_DATA 12          // Get the IP, mask and gateway of the device
#define DISCONNECT_NETWORK 13   // Disconnect from the network
#define LOAD_FLOPPY_RO 15       // Load a floppy image from the SD card in read-only mode
#define LIST_FLOPPIES 16        // List the floppy images in the SD card
#define LOAD_FLOPPY_RW 17       // Load a floppy image from the SD card in read-write mode
#define QUERY_FLOPPY_DB 18      // Query the floppy database. Need to pass the letter or number to query
#define DOWNLOAD_FLOPPY 19      // Download a floppy image from the URL
#define GET_SD_DATA 20          // Get the SD card status, size, free space and folders
#define GET_LATEST_RELEASE 21   // Get the latest release version of the firmware
#define CREATE_FLOPPY 22        // Create a floppy image based in a template
#define BOOT_RTC 23             // Boot the RTC emulator
#define CLEAN_START 24          // Start the configurator when the app starts
#define BOOT_GEMDRIVE 25        // Boot the GEMDRIVE emulator
#define REBOOT 26               // Reboot the device

// APP_FLOPPYEMUL commands
#define FLOPPYEMUL_SAVE_VECTORS (APP_FLOPPYEMUL << 8 | 0)      // Save the vectors of the floppy emulator
#define FLOPPYEMUL_READ_SECTORS (APP_FLOPPYEMUL << 8 | 1)      // Read sectors from the floppy emulator
#define FLOPPYEMUL_WRITE_SECTORS (APP_FLOPPYEMUL << 8 | 2)     // Write sectors to the floppy emulator
#define FLOPPYEMUL_PING (APP_FLOPPYEMUL << 8 | 3)              // Ping the floppy emulator
#define FLOPPYEMUL_SAVE_HARDWARE (APP_FLOPPYEMUL << 8 | 4)     // Save the hardware of the floppy emulator
#define FLOPPYEMUL_SET_SHARED_VAR (APP_FLOPPYEMUL << 8 | 5)    // Set a shared variable
#define FLOPPYEMUL_RESET (APP_FLOPPYEMUL << 8 | 6)             // Reset the floppy emulator
#define FLOPPYEMUL_MOUNT_DRIVE_A (APP_FLOPPYEMUL << 8 | 7)     // Mount the drive A of the floppy emulator
#define FLOPPYEMUL_UNMOUNT_DRIVE_A (APP_FLOPPYEMUL << 8 | 8)   // Unmount the drive A of the floppy emulator
#define FLOPPYEMUL_MOUNT_DRIVE_B (APP_FLOPPYEMUL << 8 | 9)     // Mount the drive B of the floppy emulator
#define FLOPPYEMUL_UNMOUNT_DRIVE_B (APP_FLOPPYEMUL << 8 | 10)  // Unmount the drive B of the floppy emulator
#define FLOPPYEMUL_SHOW_VECTOR_CALL (APP_FLOPPYEMUL << 8 | 11) // Show the vector call of the floppy emulator

// APP_RTCEMUL commands
#define RTCEMUL_TEST_NTP (APP_RTCEMUL << 8 | 0)     // Test if the network is ready to use NTP
#define RTCEMUL_READ_TIME (APP_RTCEMUL << 8 | 1)    // Read the time from the internal RTC
#define RTCEMUL_SAVE_VECTORS (APP_RTCEMUL << 8 | 2) // Save the vectors of the RTC emulator
#define RTCEMUL_REENTRY_LOCK  (APP_RTCEMUL << 8 | 3) // Command code to lock the reentry to XBIOS in the Sidecart
#define RTCEMUL_REENTRY_UNLOCK  (APP_RTCEMUL << 8 | 4) // Command code to unlock the reentry to XBIOS in the Sidecart
#define RTCEMUL_SET_SHARED_VAR  (APP_RTCEMUL << 8 | 5) // Set a shared variable

// APP_GEMDRVEMUL commands
#define GEMDRVEMUL_PING (APP_GEMDRVEMUL << 8 | 0)              // Ping the GEMDRIVE emulator
#define GEMDRVEMUL_SAVE_VECTORS (APP_GEMDRVEMUL << 8 | 1)      // Save the vectors of the GEMDRIVE emulator
#define GEMDRVEMUL_SHOW_VECTOR_CALL (APP_GEMDRVEMUL << 8 | 2)  // Show the vector call of the GEMDRIVE emulator
#define GEMDRVEMUL_REENTRY_LOCK (APP_GEMDRVEMUL << 8 | 3)      // Lock the reentry of the GEMDRIVE emulator
#define GEMDRVEMUL_REENTRY_UNLOCK (APP_GEMDRVEMUL << 8 | 4)    // Unlock the reentry of the GEMDRIVE emulator
#define GEMDRVEMUL_CANCEL (APP_GEMDRVEMUL << 8 | 5)            // Cancel the current execution
#define GEMDRVEMUL_RTC_START (APP_GEMDRVEMUL << 8 | 6)         // Start RTC emulator
#define GEMDRVEMUL_RTC_STOP (APP_GEMDRVEMUL << 8 | 7)          // Stop RTC emulator
#define GEMDRVEMUL_NETWORK_START (APP_GEMDRVEMUL << 8 | 8)     // Start the network emulator
#define GEMDRVEMUL_NETWORK_STOP (APP_GEMDRVEMUL << 8 | 9)      // Stop the network emulator

#define GEMDRVEMUL_SAVE_XBIOS_VECTOR   (APP_GEMDRVEMUL << 8 | 10)     // Save the XBIOS vector in the Sidecart
#define GEMDRVEMUL_REENTRY_XBIOS_LOCK  (APP_GEMDRVEMUL << 8 | 11)     // Enable reentry XBIOS calls
#define GEMDRVEMUL_REENTRY_XBIOS_UNLOCK (APP_GEMDRVEMUL << 8 | 12)    // Disable reentry XBIOS calls

// SIDETNFS config-protocol probe -- subcommand 0x0D,
// re-verified free in both this file and the Atari-side
// sidecart-gemdrive-atari/src/gemdrive.s CMD_* table (highest used code
// there and here is 0x0C/0x8B; 0x0D-0x18 is free on both sides). No request
// payload, no SD/WiFi/TNFS/flash access, no handle/session changes --
// returns protocol_version/max_drives/drive_count/config_drive_letter/
// status (protocol version bumped to 2, fields renamed from the
// Server-list model -- see docs/sidetnfs-config-protocol.md).
#define GEMDRVEMUL_SIDETNFS_GET_CONFIG_INFO (APP_GEMDRVEMUL << 8 | 0x0D) // Get SIDETNFS config-protocol info

// Subcommand 0x0E, same free range as 0x0D above. Replaces the
// never-committed GEMDRVEMUL_SIDETNFS_GET_SERVER (same code
// point) -- the server-list model is gone, replaced by the drive-list
// model (romemul/include/sidetnfs_config.h). Request: one uint32_t index
// (0..SIDETNFS_MAX_DRIVES-1, ordinary drives only -- the config drive has
// no index, see GET_CONFIG_INFO's config_drive_letter). Response: one
// read-only drive record plus a status. See
// docs/sidetnfs-config-protocol.md.
#define GEMDRVEMUL_SIDETNFS_GET_DRIVE (APP_GEMDRVEMUL << 8 | 0x0E) // Get one SIDETNFS drive record

// Sets/replaces one ordinary drive record in the RAM copy only --
// no flash write. Request: uint32_t index followed by the same field
// layout as GET_DRIVE's response (minus status). Response: status only
// (GEMDRVEMUL_SIDETNFS_DRIVE_STATUS). See docs/sidetnfs-config-protocol.md.
#define GEMDRVEMUL_SIDETNFS_SET_DRIVE (APP_GEMDRVEMUL << 8 | 0x0F) // Set one SIDETNFS drive record (RAM only)

// Clears one ordinary drive record in the RAM copy only -- no
// flash write, and can never touch the config drive (it has no index in
// this array). Request: uint32_t index. Response: status only.
#define GEMDRVEMUL_SIDETNFS_DELETE_DRIVE (APP_GEMDRVEMUL << 8 | 0x10) // Delete one SIDETNFS drive record (RAM only)

// Changes the config drive letter in the RAM copy only -- no
// flash write. Request: uint32_t new_config_drive_letter (ASCII code).
// Response: status only.
#define GEMDRVEMUL_SIDETNFS_SET_CONFIG_DRIVE (APP_GEMDRVEMUL << 8 | 0x11) // Set the config drive letter (RAM only)

// Validates the full RAM drive list and, only if valid, erases +
// programs the standalone SIDETNFS_CONFIG_FLASH_OFFSET sector, reads it
// back via XIP, and verifies magic/version/CRC before reporting success.
// The only command in this protocol that ever touches flash. Does not
// change the currently active TNFS session -- a reboot is required before
// any future runtime code uses the saved list. Request: none. Response:
// status only.
#define GEMDRVEMUL_SIDETNFS_SAVE_CONFIG (APP_GEMDRVEMUL << 8 | 0x12) // Persist the RAM drive list to flash

// WiFi/network configuration accessible while GEMDRIVE runs.
// Subcommands 0x13-0x15, re-verified free in both this file (highest used
// low code before this addition was 0x12/SAVE_CONFIG, next used is
// 0x19/DGETDRV_CALL -- 0x13-0x18 free) and the reference Atari driver
// (sidecart-gemdrive-atari/src/gemdrive.s CMD_* table, highest used code
// there is 0x0C/0x8B; nothing defined at 0x13-0x15). Reuses the existing
// PARAM_WIFI_* configData entries and the existing 8KB CONFIG_FLASH sector
// (romemul/config.c) -- no second permanent network config sector, and
// never routed to/from the APP_CONFIGURATOR GET_CONFIG/PUT_CONFIG_*/
// SAVE_CONFIG command IDs in romloader.c (separate command-ID namespace,
// separate dispatcher). See romemul/include/sidetnfs_netconfig.h and
// docs/sidetnfs-config-protocol.md for the wire format.
#define GEMDRVEMUL_SIDETNFS_GET_NETWORK_CONFIG (APP_GEMDRVEMUL << 8 | 0x13)  // Read the current WiFi/network config
#define GEMDRVEMUL_SIDETNFS_SET_NETWORK_CONFIG (APP_GEMDRVEMUL << 8 | 0x14)  // Validate + stage a new WiFi/network config (RAM only)
#define GEMDRVEMUL_SIDETNFS_SAVE_NETWORK_CONFIG (APP_GEMDRVEMUL << 8 | 0x15) // Persist the staged WiFi/network config to flash

// Minimal "Set Atari clock using NTP" / NTP server / UTC offset
// configuration accessible while GEMDRIVE runs. Subcommands 0x16-0x18,
// re-verified free (highest used low code before this addition was
// 0x15/SAVE_NETWORK_CONFIG, next used is 0x19/DGETDRV_CALL -- 0x16-0x18
// free). Reuses the existing GEMDRIVE_RTC/RTC_NTP_SERVER_HOST/
// RTC_UTC_OFFSET configData entries and the existing 8KB CONFIG_FLASH
// sector (romemul/config.c) -- no new PARAM_* entry, MAX_ENTRIES
// unchanged. See romemul/include/sidetnfs_rtcconfig.h for the wire
// format.
#define GEMDRVEMUL_SIDETNFS_GET_RTC_CONFIG (APP_GEMDRVEMUL << 8 | 0x16)  // Read the current RTC/NTP config
#define GEMDRVEMUL_SIDETNFS_SET_RTC_CONFIG (APP_GEMDRVEMUL << 8 | 0x17)  // Validate + stage a new RTC/NTP config (RAM only)
#define GEMDRVEMUL_SIDETNFS_SAVE_RTC_CONFIG (APP_GEMDRVEMUL << 8 | 0x18) // Persist the staged RTC/NTP config to flash

#define GEMDRVEMUL_DGETDRV_CALL (APP_GEMDRVEMUL << 8 | 0x19)   // Show the Dgetdrv call
#define GEMDRVEMUL_FSETDTA_CALL (APP_GEMDRVEMUL << 8 | 0x1A)   // Show the Fsetdta call
#define GEMDRVEMUL_DFREE_CALL (APP_GEMDRVEMUL << 8 | 0x36)     // Show the Dfree call
#define GEMDRVEMUL_DCREATE_CALL (APP_GEMDRVEMUL << 8 | 0x39)   // Show the Dcreate call
#define GEMDRVEMUL_DDELETE_CALL (APP_GEMDRVEMUL << 8 | 0x3A)   // Show the Ddelete call
#define GEMDRVEMUL_DSETPATH_CALL (APP_GEMDRVEMUL << 8 | 0x3B)  // Show the Dgetpath call
#define GEMDRVEMUL_FCREATE_CALL (APP_GEMDRVEMUL << 8 | 0x3C)   // Show the Fcreate call
#define GEMDRVEMUL_FOPEN_CALL (APP_GEMDRVEMUL << 8 | 0x3D)     // Show the Fopen call
#define GEMDRVEMUL_FCLOSE_CALL (APP_GEMDRVEMUL << 8 | 0x3E)    // Show the Fclose call
#define GEMDRVEMUL_FDELETE_CALL (APP_GEMDRVEMUL << 8 | 0x41)   // Show the Fdelete call
#define GEMDRVEMUL_FSEEK_CALL (APP_GEMDRVEMUL << 8 | 0x42)     // Show the Fseek call
#define GEMDRVEMUL_FATTRIB_CALL (APP_GEMDRVEMUL << 8 | 0x43)   // Show the Fattrib call
#define GEMDRVEMUL_DGETPATH_CALL (APP_GEMDRVEMUL << 8 | 0x47)  // Show the Dgetpath call
#define GEMDRVEMUL_FSFIRST_CALL (APP_GEMDRVEMUL << 8 | 0x4E)   // Show the Fsfirst call
#define GEMDRVEMUL_FSNEXT_CALL (APP_GEMDRVEMUL << 8 | 0x4F)    // Show the Fsnext call
#define GEMDRVEMUL_FRENAME_CALL (APP_GEMDRVEMUL << 8 | 0x56)   // Show the Frename call
#define GEMDRVEMUL_FDATETIME_CALL (APP_GEMDRVEMUL << 8 | 0x57) // Show the Fdatetime call

#define GEMDRVEMUL_PEXEC_CALL (APP_GEMDRVEMUL << 8 | 0x4B)  // Show the Pexec call
#define GEMDRVEMUL_MALLOC_CALL (APP_GEMDRVEMUL << 8 | 0x48) // Show the Malloc call

#define GEMDRVEMUL_READ_BUFF_CALL (APP_GEMDRVEMUL << 8 | 0x81)   // Read from sdCard the read buffer call
#define GEMDRVEMUL_DEBUG (APP_GEMDRVEMUL << 8 | 0x82)            // Show the debug info
#define GEMDRVEMUL_SAVE_BASEPAGE (APP_GEMDRVEMUL << 8 | 0x83)    // Save a basepage
#define GEMDRVEMUL_SAVE_EXEC_HEADER (APP_GEMDRVEMUL << 8 | 0x84) // Save an exec header

#define GEMDRVEMUL_SET_SHARED_VAR (APP_GEMDRVEMUL << 8 | 0x87)   // Set a shared variable
#define GEMDRVEMUL_WRITE_BUFF_CALL (APP_GEMDRVEMUL << 8 | 0x88)  // Write to sdCard the write buffer call
#define GEMDRVEMUL_WRITE_BUFF_CHECK (APP_GEMDRVEMUL << 8 | 0x89) // Write to sdCard the write buffer check call
#define GEMDRVEMUL_DTA_EXIST_CALL (APP_GEMDRVEMUL << 8 | 0x8A)   // Check if the DTA exists in the rp2040 memory
#define GEMDRVEMUL_DTA_RELEASE_CALL (APP_GEMDRVEMUL << 8 | 0x8B) // Release the DTA from the rp2040 memory

// Controlled Pico reboot. Subcommand 0x1B, re-verified free in
// both this file (highest used low code before this addition was
// 0x1A/FSETDTA_CALL, next used is 0x36/DFREE_CALL -- 0x1B-0x35 free) and
// the reference Atari driver (sidecart-gemdrive-atari/src/gemdrive.s
// CMD_* table, highest used code there is 0x0C/0x8B; nothing defined at
// 0x1B). No request payload, no SAVE of any kind (drive/network/RTC
// config or flash defaults) -- only ever reads the already-persisted
// configuration on the next boot, exactly like any other reset. Response:
// the existing generic ACK (write_random_token()) only, written BEFORE a
// short fixed delay and watchdog_reboot() -- see gemdrvemul.c's handler.
#define GEMDRVEMUL_REBOOT_PICO (APP_GEMDRVEMUL << 8 | 0x1B) // Reboot the Pico (ACK first, no SAVE)

// Firmware-update version check. Subcommand 0x1C, re-verified free (see
// GEMDRVEMUL_REBOOT_PICO's own comment above -- 0x1C-0x35 still free after
// REBOOT_PICO took 0x1B). Fetches http://retroloft.net/sidetnfs/version.txt
// (plain HTTP -- see sidetnfs_update_check.c) and compares it against this
// firmware's own RELEASE_VERSION. Blocking, bounded by
// SIDETNFS_UPDATE_CHECK_TIMEOUT_MS -- DNS + TCP connect + a tiny HTTP GET
// all happen inside this one command, same "blocks the GEMDOS command loop
// for its own bounded budget" contract the boot-time NTP wait already
// uses. No request payload. Response: see GEMDRVEMUL_SIDETNFS_UPDATE_STATUS
// in gemdrvemul.h.
#define GEMDRVEMUL_SIDETNFS_CHECK_UPDATE (APP_GEMDRVEMUL << 8 | 0x1C) // Check github for a newer firmware version

// Subcommands 0x1D-0x22: formerly FLOPPY.PRG server-profile config
// (GET_CONFIG_INFO/GET_PROFILE/SET_PROFILE/DELETE_PROFILE/
// SET_ACTIVE_PROFILE/SAVE_PROFILES). Removed in the mixed-source
// Favorites/Carousel redesign -- the firmware no longer persists or
// knows about floppy source/mount profiles in flash at all; FLOPPY.PRG
// owns its own Browser source configuration (CONFIG.CFG) on local disk
// now, with zero round-trips to the Pico for source management. Left
// free, not reused immediately.

// FLOPPY.PRG LFN directory browser (Step 2/3). Subcommands 0x23-0x25, from
// the range the block above already reserved for this (0x26 free again as
// of Step 3 -- GET_DIR_PAGE/GET_FILE_PAGE merged into one GET_PAGE, see
// below). Browses ONE active source directly -- no GEMDOS drive/letter,
// no Fsfirst/Fsnext, no 8.3 conversion, no name aliasing. Exactly one CWD
// is active at a time (see sidetnfs_floppy_browse.h); a generation
// counter bumped by OPEN/CHANGE_DIR lets GET_PAGE detect a stale request
// (e.g. the Atari asking for a page from a directory it has since left).
// See romemul/include/sidetnfs_floppy_browse.h for status codes and
// romemul/include/gemdrvemul.h (GEMDRVEMUL_FLOPPY_BROWSE/_PAGE) for the
// wire layout.
//
// BROWSE_OPEN's request shape changed with the mixed-source redesign:
// backend(2) + port(2) + host(64) + start_directory(256), in that order,
// no leading header/skip of any kind -- since the firmware no longer has
// a stored profile to resolve a plain index against. TNFS always mounts
// "/" now; there is no mount_path field any more.
#define GEMDRVEMUL_FLOPPY_BROWSE_OPEN (APP_GEMDRVEMUL << 8 | 0x23)       // Open a source for browsing (resolves backend/session, CWD = start_directory or root)
#define GEMDRVEMUL_FLOPPY_BROWSE_CHANGE_DIR (APP_GEMDRVEMUL << 8 | 0x24) // Change the active CWD (subdir name, or go-up)
// Step 3: ONE combined page (dirs listed before files, GEMDRVEMUL_FLOPPY_PAGE_IS_DIR
// says which is which per slot) instead of separate dir/file pages -- was
// 0x25=GET_DIR_PAGE/0x26=GET_FILE_PAGE, now just this. Request payload
// unchanged (generation(4) + page_index(4)) -- no want_dirs parameter.
#define GEMDRVEMUL_FLOPPY_BROWSE_GET_PAGE (APP_GEMDRVEMUL << 8 | 0x25)   // Fetch one combined page (dirs then files) of the active CWD

// SideTNFS floppy emulator (Phase 2). Subcommands 0x26-0x2B, re-verified
// free (highest used low code before this addition was 0x25/
// GEMDRVEMUL_FLOPPY_BROWSE_GET_PAGE, next used is 0x36/GEMDRVEMUL_DFREE_CALL
// -- 0x26-0x35 free). Entirely independent from the browser commands
// above (0x1D-0x25): those describe browsing a source directory, these
// describe an active floppy-emulation SESSION (one mounted .ST image on
// virtual drive A: or B: -- see GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER,
// exactly one of the two, never both -- one packed Favorites table). See
// romemul/include/gemdrvemul.h (GEMDRVEMUL_FLOPPY_FAVORITES/_SESSION) for
// the wire layout. GEMDRVEMUL_FLOPPY_EXIT_ACK (0x2B) is RESERVED, UNUSED
// -- see GEMDRVEMUL_FLOPPY_SESSION_EXIT_ACK_SEEN's own comment in
// gemdrvemul.h for why the automatic-reset design it was part of got
// abandoned in favor of a manual Atari RESET.
//
// FAVORITES_WRITE_CHUNK/_WRITE_CHECK mirror the existing
// GEMDRVEMUL_WRITE_BUFF_CALL/_WRITE_BUFF_CHECK pattern exactly (chunk +
// Pico-computed checksum, verify, then commit) -- see
// SIDETNFS_FLOPPY_FAVORITES_CHUNK_MAX (gemdrvemul.h) for the per-round
// size (2048 bytes, matching the Atari driver's own BUFFER_WRITE_SIZE).
// This reuses the proven bulk-transfer transport instead of assuming
// ROM3 is directly writable by the Atari -- it is not, on real hardware
// (the cartridge ROM-select lines are read-only; every Atari->Pico byte,
// bulk or scalar, is sent as an address-encoded read, never a plain
// store).
#define GEMDRVEMUL_FLOPPY_FAVORITES_WRITE_CHUNK (APP_GEMDRVEMUL << 8 | 0x26) // Send one <=2048-byte chunk of the packed Favorites blob (offset+length+data)
#define GEMDRVEMUL_FLOPPY_FAVORITES_WRITE_CHECK (APP_GEMDRVEMUL << 8 | 0x27) // Verify the last chunk's checksum and commit it into the FAVORITES block
#define GEMDRVEMUL_FLOPPY_FAVORITES_COMMIT (APP_GEMDRVEMUL << 8 | 0x28)      // All chunks sent -- validate table/strings, publish COUNT/ACTIVE_INDEX

// Prepares a floppy session: validates the chosen image's
// BPB/file-size/geometry (req #2) against the already-uploaded Favorites
// (or a directly-supplied path) and, on success, publishes the
// REQUESTED Atari boot configuration (GEMDRVEMUL_FLOPPY_SESSION_
// INSTALL_GEMDRIVE/_INSTALL_FLOPPY). This does NOT switch any Pico-side
// mode -- the firmware always runs the same single image, answering both
// the GEMDOS-relay and floppy-session command sets regardless of these
// flags. INSTALL_GEMDRIVE/_INSTALL_FLOPPY only take effect on the
// ATARI's next boot, read by the Atari's own cartridge-init to decide
// what to install (see GEMDRVEMUL_FLOPPY_SESSION's own comment in
// gemdrvemul.h for the four valid combinations). FLOPPY.PRG sets these
// two flags however it needs (e.g. NO/YES for a clean floppy-only boot,
// YES/YES to keep GEMDOS drives too) before triggering its own reset;
// this command does not reset the Atari itself. Mixed-source redesign:
// request is now backend(2)+port(2)+host(64) (the FIRST Carousel entry's
// own self-contained source, same shape as one Favorites/Carousel table
// entry -- no more session-wide active_slot) + install_gemdrive(2) +
// install_floppy(2) + drive_number(2) (0=A:/1=B:, which unit
// INSTALL_FLOPPY emulates -- ignored when install_floppy==NO) + image path
// (string field). Response: status + geometry echo (see
// GEMDRVEMUL_FLOPPY_SESSION_SIDES/_SECTORS_PER_TRACK/_TRACKS/_BYTES_PER_SECTOR).
#define GEMDRVEMUL_FLOPPY_SESSION_START (APP_GEMDRVEMUL << 8 | 0x29) // Validate the image, prepare the floppy session, publish the requested Atari boot configuration

// One logical sector, one round trip -- request carries ONLY the 0-based
// logical sector index, sent as the command's own small payload
// (uint32_t, 4 bytes -- read via GET_PAYLOAD_PARAM32(payloadPtr) in the
// dispatch handler, exactly like every other GEMDRVEMUL_* request).
// Phase 4 correction: this is NOT a pre-written ROM3 field -- ROM3 is not
// Atari-writable on real hardware (see GEMDRVEMUL_FLOPPY_SESSION_SECTOR_LBA's
// own comment in gemdrvemul.h), so the LBA has to travel as an ordinary
// command payload like anything else the Atari sends. No drive/track/
// side/count fields: which drive (A: or B:, GEMDRVEMUL_FLOPPY_SESSION_
// DRIVE_NUMBER) is implicit -- this session emulates exactly one drive at
// a time, so once floppy.s's own hook has matched disk_number against it
// there is nothing left to disambiguate here -- and the Atari-side hdv_rw
// hook is responsible for converting
// TOS's own track/side/sector BIOS parameters into this one LBA value
// before sending the request -- exactly the same layering the existing
// GEMDOS relay already uses (GEMDOS-level concepts never leak into the
// wire protocol as separate fields when a single derived value says the
// same thing). Response: status + the LBA served (echoed back into
// GEMDRVEMUL_FLOPPY_SESSION_SECTOR_LBA, for the Atari's own desync
// sanity-check only) + 512 bytes written directly into
// GEMDRVEMUL_FLOPPY_SESSION_SECTOR_DATA (same "response data placed
// directly in the shared window" asymmetry GEMDRVEMUL_READ_BUFF already
// relies on). Read-only MVP: there is no WRITE_SECTOR command -- Atari
// writes are rejected by the driver itself, before ever reaching the
// wire (req #6).
#define GEMDRVEMUL_FLOPPY_READ_SECTOR (APP_GEMDRVEMUL << 8 | 0x2A) // Read one 512-byte logical sector from the mounted image (request: LBA(4) + caller_pc(4) + rwabs_count(4) payload -- caller_pc/rwabs_count are hardware bring-up diagnostics only; caller_pc is 0 from the not-yet-instrumented XBIOS Floprd path)

// RESERVED, UNUSED -- no handler exists on the Pico side. Was going to be
// a fire-and-forget ACK an Atari-side VBL handler sent back in response
// to a "reset requested" flag (the slot that field used has since been
// reclaimed for GEMDRVEMUL_FLOPPY_SESSION_DRIVE_NUMBER), as part of the
// abandoned automatic-reset design (see GEMDRVEMUL_FLOPPY_SESSION_EXIT_ACK_SEEN's
// own comment in gemdrvemul.h).
// Long-SELECT exit now disables floppy mode and confirms via LED
// (floppy_select_trigger_exit() in gemdrvemul.c); the user presses Atari
// RESET manually. Left defined, not renumbered, to avoid reshuffling the
// command-ID space for no benefit.
#define GEMDRVEMUL_FLOPPY_EXIT_ACK (APP_GEMDRVEMUL << 8 | 0x2B)

// Sent once by the Atari's floppy-hook installer, right after it reads
// (and before it overwrites) the current getbpb/rwabs/mediach vectors --
// request: the three original vector values (12-byte payload, d3/d4/d5,
// the maximum a plain send_sync call carries -- matches
// GEMDRVEMUL_SAVE_VECTORS' own shape for the GEMDOS trap). The Pico just
// stores them in GEMDRVEMUL_FLOPPY_SESSION_OLD_HDV_* (gemdrvemul.h) -- no
// backend involvement. This exists because ROM4 (where the driver's own
// code/data lives) is not Atari-writable either (same constraint
// GEMDRVEMUL_SAVE_VECTORS already works around for the GEMDOS trap, see
// gemdrive.s's old_handler comment), so none of these three old vectors
// can be cached locally for the hooks' own "not our drive, fall through"
// path -- they're stored here instead and read back with a fast,
// ordinary local ROM3 load on every such fall-through (no command
// round-trip per disk access). Response: status only.
#define GEMDRVEMUL_FLOPPY_SAVE_VECTORS (APP_GEMDRVEMUL << 8 | 0x2C) // Save the original getbpb/rwabs/mediach vectors before the floppy hooks overwrite them

// Same rationale as GEMDRVEMUL_FLOPPY_SAVE_VECTORS above, for the fourth
// vector (the XBIOS trap) separately -- a plain send_sync payload is
// capped at 12 bytes (d3/d4/d5), so the fourth longword needs its own
// call. Request: the original XBIOS trap vector (4-byte payload, d3).
// Stored in GEMDRVEMUL_FLOPPY_SESSION_OLD_XBIOS_VECTOR -- entirely
// separate from GEMDRIVE's own GEMDRVEMUL_OLD_XBIOS-style storage for its
// RTC Y2K-patch trap (GEMDRVEMUL_SAVE_XBIOS_VECTOR, subcommand 0xA): each
// installer only ever touches its own field, so both can coexist
// (YES/YES) without collision regardless of install order. Response:
// status only.
#define GEMDRVEMUL_FLOPPY_SAVE_XBIOS_VECTOR (APP_GEMDRVEMUL << 8 | 0x2D) // Save the original XBIOS trap vector before the floppy Floprd/Flopwr/Flopfmt/Flopver handling chains in front of it

// Phase 4A: sent by the Atari's mediach hook exactly once, the first time
// it sees GEMDRVEMUL_FLOPPY_SESSION_MEDIA_CHANGED nonzero -- clears that
// field back to 0 so the NEXT mediach call reports "unchanged" without
// needing a round trip at all (the common case). Zero-payload,
// fire-and-forget in spirit (the Atari doesn't need to wait for anything
// beyond the usual completion token) -- if it's ever lost to a network
// glitch, the only consequence is mediach reporting "changed" one extra
// time, which is harmless (TOS just re-fetches the BPB again). Designed
// for the future short-SELECT favorite-switching flow (Phase 6) as much
// as for the initial SESSION_START mount -- both are "a new image just
// became active" events using the same one field.
#define GEMDRVEMUL_FLOPPY_MEDIA_CHANGE_ACK (APP_GEMDRVEMUL << 8 | 0x2E) // Acknowledge a reported media change, clearing it back to unchanged
// Hardware bring-up investigation: Getbpb/Mediach are pure local ROM3
// reads on the Atari side (no wire round-trip needed for their actual
// function), which made them a total blind spot in every trace so far --
// these two zero-payload pings exist purely to make them visible.
// GETBPB_PING fires on new_getbpb_routine's disk_number=0/floppy-active/
// image-ready success path, right before it returns the BPB pointer.
// MEDIACH_PING fires only on new_mediach_routine's steady-state
// "unchanged" path -- the "changed" branch already has wire visibility
// via GEMDRVEMUL_FLOPPY_MEDIA_CHANGE_ACK above, not duplicated here.
#define GEMDRVEMUL_FLOPPY_GETBPB_PING (APP_GEMDRVEMUL << 8 | 0x2F) // Zero payload -- diagnostic only, see comment above
#define GEMDRVEMUL_FLOPPY_MEDIACH_PING (APP_GEMDRVEMUL << 8 | 0x30) // Zero payload -- diagnostic only, see comment above

typedef struct
{
    unsigned int value;
    const char *name;
} CommandName;

extern const CommandName commandStr[];
extern const int numCommands;
#endif // COMMANDS_H_