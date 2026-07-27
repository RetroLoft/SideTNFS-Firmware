/**
 * File: main.c
 * Author: Diego Parrilla Santamaría
 * Date: July 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: C file that launches the SideTNFS/GEMDRIVE emulator.
 * Fase 3 (Boot altijd GEMDRIVE): production always boots straight into
 * GEMDRIVE now, unconditionally -- the old ROM_EMULATOR/FLOPPY_EMULATOR/
 * RTC_EMULATOR/configurator mode selection below main()'s GEMDRIVE block
 * is dead code in production (init_gemdrvemul() never returns), kept
 * compiling/linking only, see this phase's own report.
 */

#include "include/romemul.h"
#include "include/rtcemul.h"
#include "include/gemdrvemul.h"
#include "include/network.h"
#include "include/sidetnfs_config.h"
#include "include/sidetnfs_system_config.h"
#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_longpress.h"

// Fase 12B2 (SELECT 10s factory reset / noodherstel): detects SELECT held
// continuously for SIDETNFS_FACTORY_RESET_HOLD_MS (10s) and, if so, wipes
// the SideTNFS drive-list flash config back to factory defaults and
// resets. Independent of WiFi/TNFS/GEMDOS/SIDETNFS.PRG and of whatever is
// currently in flash (a corrupt/incompatible config is exactly the case
// this exists to recover from).
//
// Placement: called right after network_wifi_init_for_settings() (the
// single CYW43 init call for the whole boot, see main()) succeeds, before
// load_all_entries()/any mode branch. This is as early as this project's
// hardware allows while still being able to signal the user: the onboard
// LED on a Pico W is wired through the CYW43 chip
// (cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, ...), same as every other
// blink_morse()/blink_error() call in this codebase), so it cannot be lit
// before cyw43 has been initialized -- there is no earlier point at which
// this feature could give the user any visible feedback at all on this
// hardware. Fase 4 (CYW43-initialisatie en WiFi-timeouts): load_all_entries()/
// sidetnfs_system_config_init() now run BEFORE this check (moved up, so the
// real country code is known before the single CYW43 init) -- this is safe
// because neither of those two calls ever touches GEMDOS/runtime-drive code
// or the SideTNFS drive-list flash this feature protects; only
// sidetnfs_config_init()/sidetnfs_probe_load_active_server()/init_gemdrvemul()
// (the actual GEMDOS-facing code) still run strictly after this check, so a
// corrupt SideTNFS drive-list config never gets a chance to reach GEMDOS/
// runtime-drive code before the user has had their full 10 seconds.
//
// Zero overhead when SELECT isn't held at boot at all: a single
// instantaneous gpio_get() and an immediate return, no polling loop
// entered. Continuous runtime (not just boot-time) SELECT monitoring
// would require polling from inside the GEMDOS command-dispatch loop in
// gemdrvemul.c, which is timing-critical cartridge-bus code this phase
// deliberately does not touch -- boot-time-only detection is the safe
// tradeoff (see report).
// Number of CONSECUTIVE low samples required before SELECT counts as
// genuinely released. sidetnfs_longpress_poll_step() documents that it
// wants a *debounced* reading, but this call site used to hand it the raw
// pin state, so a single contact bounce or noise glitch anywhere in the
// ~667 samples of a 10s hold silently aborted the whole procedure -- with
// no LED feedback to reveal it. At SIDETNFS_FACTORY_RESET_POLL_MS (15ms)
// per sample this is ~60ms of tolerance, far longer than mechanical
// bounce and far shorter than any intentional release.
#define SIDETNFS_FACTORY_RESET_RELEASE_SAMPLES 4u

static void sidetnfs_check_select_factory_reset(void)
{
    if (gpio_get(SELECT_GPIO) == 0)
    {
        return; // not held at boot at all -- nothing to do, nothing changed
    }

    // SELECT is down on the very first sample: light the LED for the whole
    // procedure so the user can actually see it is running. Without this
    // the entire 10s hold was completely dark and indistinguishable from
    // "nothing is happening" (see report).
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    uint32_t elapsed_ms = 0;
    uint32_t low_samples = 0;
    for (;;)
    {
        sleep_ms(SIDETNFS_FACTORY_RESET_POLL_MS);
        elapsed_ms += SIDETNFS_FACTORY_RESET_POLL_MS;

        if (gpio_get(SELECT_GPIO) != 0)
        {
            low_samples = 0; // still held -- any high sample breaks the low run
        }
        else
        {
            low_samples++;
        }

        // The debounced reading the helper's contract asks for: only a run
        // of SIDETNFS_FACTORY_RESET_RELEASE_SAMPLES consecutive lows counts
        // as released. elapsed_ms keeps accumulating through a bounce, so a
        // glitch costs no hold time.
        bool pressed = (low_samples < SIDETNFS_FACTORY_RESET_RELEASE_SAMPLES);

        sidetnfs_longpress_result_t result =
            sidetnfs_longpress_poll_step(pressed, elapsed_ms, SIDETNFS_FACTORY_RESET_HOLD_MS);

        if (result == SIDETNFS_LONGPRESS_CANCELLED)
        {
            DPRINTF("SELECT released before %lums -- factory reset cancelled, nothing changed.\n",
                    (unsigned long)SIDETNFS_FACTORY_RESET_HOLD_MS);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); // procedure aborted -- LED off, boot on
            return;
        }
        if (result == SIDETNFS_LONGPRESS_TRIGGERED)
        {
            break;
        }
        // else WAITING -- keep polling
    }

    DPRINTF("SELECT held %lums -- performing factory reset.\n", (unsigned long)SIDETNFS_FACTORY_RESET_HOLD_MS);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); // already on since the first sample -- kept explicit

    sidetnfs_config_status_t rc = sidetnfs_config_factory_reset();
    if (rc == SIDETNFS_CONFIG_STATUS_OK)
    {
        DPRINTF("Factory reset OK -- rebooting.\n");
        sleep_ms(1000);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        watchdog_reboot(0, 0, 0);
        while (true)
        {
            tight_loop_contents();
        }
    }

    // Fase 12B2: never reset as if it succeeded when it didn't -- the
    // existing configuration (whatever it was) was never actually
    // overwritten in this failure branch (sidetnfs_config_save() never
    // partially erases/programs, see its own doc comment), so looping
    // here rather than continuing to boot avoids re-entering the exact
    // failure this feature exists to recover from. Slow blink is
    // deliberately distinct from any existing fast blink_morse() pattern.
    DPRINTF("Factory reset FAILED (status=%d) -- halting with error pattern, never rebooting silently.\n", (int)rc);
    for (;;)
    {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(500);
    }
}

#if SIDETNFS_FORCE_CONFIG_RECOVERY
// Fase 12B5: one-shot, unconditional recovery build -- forcibly rewrites
// the SideTNFS drive-list flash sector to clean native-v3 factory
// defaults (S:/N: only) unless it already exactly matches those defaults
// (see sidetnfs_config_flash_matches_factory_defaults()). No UART: no
// printf, no DPRINTF call anywhere in this function (DPRINTF itself
// already compiles to nothing at _DEBUG=0, but this function avoids it
// entirely regardless, so the "zero UART code" guarantee does not
// depend on that macro's expansion). No SD access, no migration path --
// sidetnfs_config_factory_reset() calls sidetnfs_config_load_defaults()
// directly, never sidetnfs_config_init()'s migration logic.
//
// Placement: right after network_wifi_init_for_settings() (needed for the
// LED, same hardware constraint as sidetnfs_check_select_factory_reset()
// above), before sidetnfs_config_init()/any GEMDOS-facing code -- so a
// corrupt/incompatible SideTNFS drive-list config never gets a chance to
// reach GEMDOS/runtime-drive code at all in this build.
//
// Reset-loop avoidance: if flash already matches the factory defaults
// exactly (e.g. the second boot after a successful recovery write),
// this function returns immediately and normal boot proceeds -- no
// write, no LED, no reset. Only a genuine mismatch triggers a
// write+verify+reset cycle, and that cycle runs at most once per actual
// mismatch.
static void sidetnfs_force_config_recovery(void)
{
    if (sidetnfs_config_flash_matches_factory_defaults())
    {
        return; // already correct -- boot normally, no write, no reset
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    sidetnfs_config_status_t rc = sidetnfs_config_factory_reset();
    bool verified = (rc == SIDETNFS_CONFIG_STATUS_OK) && sidetnfs_config_flash_matches_factory_defaults();

    if (verified)
    {
        sleep_ms(1000);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        watchdog_reboot(0, 0, 0);
        while (true)
        {
            tight_loop_contents();
        }
    }

    // Write or verification failed -- never reset as if it succeeded,
    // never start TNFS/GEMDRIVE runtime. Same slow-blink error pattern
    // as sidetnfs_check_select_factory_reset()'s own failure path.
    for (;;)
    {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(500);
    }
}
#endif // SIDETNFS_FORCE_CONFIG_RECOVERY

// Fase 10 (Morse-LED en oude productstates verwijderen): the only LED
// signal left on the normal boot path. Three short flashes, purely as
// physical "the Pico came up / restarted" feedback for user and
// developer -- deliberately NOT a Morse character and NOT a status code:
// it never encodes product mode, WiFi, network, NTP or any error state.
//
// Replaces the old blink_morse('H') ("H" for HARDISK, i.e. a product-mode
// status code) that used to sit at this exact point. Written out here
// rather than routed through blink_morse() so nothing on the production
// boot path depends on the Morse alphabet any more; blink_morse()/
// blink_error() themselves stay compiled for the retained floppy
// emulation code (see report).
//
// Requires cyw43 to be initialized already -- the Pico W's onboard LED is
// wired through the CYW43 chip, so this can only run after
// network_wifi_init_for_settings() (see its call site in main()).
#define BOOT_FLASH_COUNT 3
#define BOOT_FLASH_ON_MS 150
#define BOOT_FLASH_OFF_MS 150

// Settle time given to CYW43/lwIP after the boot flashes, before the first
// DNS/NTP and TNFS activity in init_gemdrvemul() below.
//
// Not cosmetic: hardware isolation (see report) proved this is required.
// The old blink_morse('H') that used to sit at this point spent ~1200ms
// here ("H" = four Morse dots); boot_led_flash() spends ~900ms. Removing
// those ~300ms made the boot-time NTP sync fail with a timeout on every
// single boot, while the byte-identical build with the 300ms restored
// syncs normally. Four A/B/C isolation builds narrowed it to exactly this
// delay -- it is not the NTP timeout, not the WiFi retry logic and not
// DNS itself, all of which are untouched.
#define SIDETNFS_WIFI_SETTLE_DELAY_MS 300
static void boot_led_flash(void)
{
    for (int i = 0; i < BOOT_FLASH_COUNT; i++)
    {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(BOOT_FLASH_ON_MS);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(BOOT_FLASH_OFF_MS);
    }
}

int main()
{
    // Set the clock frequency. 20% overclocking
    set_sys_clock_khz(RP2040_CLOCK_FREQ_KHZ, true);

    // Set the voltage
    vreg_set_voltage(RP2040_VOLTAGE);

    // Configure the input pins for ROM4 and ROM3
    gpio_init(SELECT_GPIO);
    gpio_set_dir(SELECT_GPIO, GPIO_IN);
    gpio_set_pulls(SELECT_GPIO, false, true); // Pull down (false, true)
    gpio_pull_down(SELECT_GPIO);

#if _DEBUG
    // Initialize chosen serial port
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 1); // specify that the stream should be unbuffered
#endif
    // Only startup information to display
    DPRINTF("\n\nSidecart ROM emulator. %s (%s). %s mode.\n\n", RELEASE_VERSION, RELEASE_DATE, _DEBUG ? "DEBUG" : "RELEASE");

#if _DEBUG
    // Show information about the frequency and voltage
    int current_clock_frequency_khz = RP2040_CLOCK_FREQ_KHZ;
    const char *current_voltage = VOLTAGE_VALUES[RP2040_VOLTAGE];
    DPRINTF("Clock frequency: %i KHz\n", current_clock_frequency_khz);
    DPRINTF("Voltage: %s\n", current_voltage);
#endif

    // Fase 4 (opschonen): force_configurator (SELECT held at boot forcing
    // the old configurator) is removed -- Fase 3 already made GEMDRIVE the
    // sole reachable production path regardless of SELECT/BOOT_FEATURE, so
    // this boot-time GPIO read had no remaining effect on anything
    // reachable. The three legacy (already unreachable) mode blocks below
    // no longer reference it either -- see their own updated conditions.

    // Fase 4 (CYW43-initialisatie en WiFi-timeouts): load_all_entries()/
    // sidetnfs_system_config_init() moved here (from their old position
    // after the factory-reset checks below) so the real WiFi country code
    // is known BEFORE the one, single CYW43 init call -- see that call's
    // own comment for why this reordering is safe with respect to the
    // factory-reset/force-config-recovery safety guarantees.
    load_all_entries();

    // Fase 2: load/validate the independent SideTNFS system config
    // (WiFi/Network/RTC) exactly once, before anything (including the
    // CYW43 init right below) needs sys.country/sys.ssid/etc. Never writes
    // to flash itself; only an explicit SAVE from SIDETNFS.PRG persists a
    // migrated or newly-edited configuration.
    sidetnfs_system_config_init();

    sidetnfs_system_settings_t boot_sys;
    sidetnfs_system_config_get(&boot_sys);

    // Fase 4: the single, sole CYW43 init call for the entire boot --
    // resolves country from sys.country and calls
    // cyw43_arch_init_with_country() exactly once (see that function's own
    // comment in network.h/network.c). Replaces the old
    // cyw43_arch_init() (generic, no country) here, later undone by a
    // cyw43_arch_deinit() + cyw43_arch_init_with_country() cycle deep
    // inside init_gemdrvemul() -- that whole cycle is gone now.
    if (network_wifi_init_for_settings(&boot_sys))
    {
        DPRINTF("Wi-Fi init failed\n");
        return -1;
    }

#if SIDETNFS_FORCE_CONFIG_RECOVERY
    // Fase 12B5: one-shot, unconditional flash-config recovery -- runs
    // before the SELECT long-press check below (and before everything
    // else), see the function's own doc comment.
    sidetnfs_force_config_recovery();
#endif

    // Fase 12B2: SELECT held 10s -> factory reset (see the function's own
    // doc comment for exact placement reasoning). Must run before
    // sidetnfs_config_init()/any SideTNFS-drive-list-dependent code below.
    sidetnfs_check_select_factory_reset();

    // Fase 10 (Morse-LED en oude productstates verwijderen): the
    // PARAM_BOOT_FEATURE read that used to be here is gone. Boot dispatch
    // no longer consults it (Fase 3 already made GEMDRIVE the sole
    // reachable production path), so reading it only produced a log line
    // describing a decision nothing acts on. The config entry itself and
    // its flash layout/offsets are deliberately left untouched -- see
    // config.c's configEntries table.

    ConfigEntry *default_config_reboot_mode = find_entry(PARAM_SAFE_CONFIG_REBOOT);
    DPRINTF("SAFE_CONFIG_REBOOT: %s\n", default_config_reboot_mode->value);

    bool safe_config_reboot = default_config_reboot_mode->value[0] == 't' || default_config_reboot_mode->value[0] == 'T';

    // Fase 3 (Boot altijd GEMDRIVE): production always boots straight into
    // SideTNFS/GEMDRIVE now -- no more BOOT_FEATURE string comparison, no
    // more SELECT-at-boot override (Fase 4 removed the force_configurator
    // variable entirely -- see this file's own history). This used to be
    // the last of four mutually-exclusive `if (strcmp(...))` blocks below
    // (ROM_EMULATOR/FLOPPY_EMULATOR/RTC_EMULATOR/GEMDRIVE_EMULATOR, in
    // that order, plus a final "fall through to the old configurator"
    // default) -- moved here, unconditional, so it always wins regardless
    // of what PARAM_BOOT_FEATURE says. init_gemdrvemul() below never
    // returns, so every old mode-selection block still present
    // further down in this function (kept compiling/linking on purpose --
    // see this phase's own report, "verwijder nog geen bestanden, handlers
    // of CMake-sources") is now structurally unreachable in production.
    DPRINTF("GEMDRIVE_EMULATOR entry found in config. Launching.\n");

    // Copy the GEMDRIVE firmware emulator to RAM
    COPY_FIRMWARE_TO_RAM((uint16_t *)gemdrvemulROM, gemdrvemulROM_length);

    // Reserve memory for the protocol parser
    init_protocol_parser();

    // Hybrid way to initialize the ROM emulator:
    // IRQ handler callback to read the commands in ROM3, and NOT copy the FLASH ROMs to RAM
    // and start the state machine
    init_romemul(NULL, gemdrvemul_dma_irq_handler_lookup_callback, false);

#if _DEBUG
    //  Check if the USB is connected. If so, check if the SD card is inserted and initialize the USB Mass storage device
    if (cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN))
    {
        DPRINTF("USB connected\n");
        usb_mass_init();
    }
#endif
    change_spi_speed();

    DPRINTF("Ready to accept commands.\n");

    // Fase 10: three short LED flashes -- physical boot/restart feedback
    // only (see boot_led_flash()'s own comment). Replaces the old
    // blink_morse('H') product-mode status code.
    boot_led_flash();
    // Let CYW43/lwIP settle before the first DNS/NTP and TNFS traffic --
    // see SIDETNFS_WIFI_SETTLE_DELAY_MS's own comment for why this is
    // required and not cosmetic.
    sleep_ms(SIDETNFS_WIFI_SETTLE_DELAY_MS);

    // Fase 9C: load/validate the persistent SideTNFS drive-list flash
    // config exactly once, before GEMDRIVE can process any of the
    // GEMDRVEMUL_SIDETNFS_* configuration commands. Read-only here --
    // only SAVE_CONFIG (via SIDETNFS.PRG) ever writes to flash.
    sidetnfs_config_init();

    // Fase 4: sidetnfs_system_config_init() moved to run early, before the
    // single CYW43 init call near the top of main() -- no longer called a
    // second time here (an _init() function should only ever run once per
    // boot; sidetnfs_system_config_get() below and inside init_gemdrvemul()
    // is the cheap, repeatable, read-only way to consult its result).

#if SIDETNFS_ENABLE_DIAG_UART && !SIDETNFS_ENABLE_SLOT_DIAG
    // Fase 12B2: read-only boot-time config diagnosis -- must run
    // before sidetnfs_probe_load_active_server()/init_gemdrvemul()
    // (i.e. before any TNFS session or runtime-drive-table code),
    // and before the SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM override
    // below, so it always reports the REAL flash-derived config for
    // this boot regardless of that override. Fase 12B3: automatically
    // suppressed when SIDETNFS_ENABLE_SLOT_DIAG is on, so the two
    // diagnostics never both fire and clutter the boot log -- see
    // this phase's own compact Dump A below instead.
    sidetnfs_config_dump_uart();
#endif

#if SIDETNFS_ENABLE_DIAG_UART && SIDETNFS_ENABLE_SLOT_DIAG
    // Fase 12B4: explicitly requires SIDETNFS_ENABLE_DIAG_UART too --
    // SIDETNFS_ENABLE_SLOT_DIAG alone must never pull in any
    // UART/printf code path (see SIDETNFS_ENABLE_SD_SLOT_DUMP for the
    // UART-free, SD-only equivalent).
    // Fase 12B3, Dump A -- compact, per-slot summary of the loaded
    // configuration (short strings only: state/letter/type, never
    // host/mount_path/sd_path). Independent of the larger Fase 12B2
    // dump above (see SIDETNFS_ENABLE_SLOT_DIAG's own CMake comment).
    printf("\r\n--- SLOTDIAG A: loaded config slots ---\r\n");
    for (uint8_t _sd_i = 0; _sd_i < SIDETNFS_MAX_DRIVES; _sd_i++)
    {
        sidetnfs_drive_config_t _sd_d;
        sidetnfs_config_get_drive(_sd_i, &_sd_d);
        if (sidetnfs_drive_slot_is_empty(&_sd_d))
        {
            printf("CFG slot=%u state=EMPTY\r\n", (unsigned)_sd_i);
        }
        else
        {
            printf("CFG slot=%u state=%s letter=%c type=%u\r\n", (unsigned)_sd_i,
                   sidetnfs_drive_slot_is_enabled(&_sd_d) ? "ENABLED" : "DISABLED",
                   (char)_sd_d.drive_letter, (unsigned)_sd_d.type);
        }
    }
    printf("SETTINGS letter=%c\r\n", (char)sidetnfs_config_get_config_drive_letter());
    printf("--- END SLOTDIAG A ---\r\n\r\n");
#endif

#if SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM
    // Fase 12B2 (safe-factory-in-RAM diagnostic recovery build): the
    // real flash config was just read (and, if diag UART is also on,
    // dumped above) -- now discard it for RUNTIME purposes only.
    // Never writes to flash, so whatever is actually stored survives
    // untouched for later diagnosis. Boots with the factory-default
    // N: + SETTINGS S: only.
    sidetnfs_config_force_factory_ram();
#endif

    // Fase 9D: pick the first usable (used, TNFS, UDP) drive from that
    // same config as the active server for the real TNFS connection,
    // replacing the old hardcoded IP/port/mount-name constants in
    // sidetnfs_probe.c. Must run after sidetnfs_config_init() and
    // before init_gemdrvemul() (which is where the drive letter is
    // decided and where the actual mount probe eventually fires). If
    // no usable drive is found -- missing/invalid config, only SD
    // drives, or only TCP-configured TNFS drives (TCP stays
    // unsupported) -- this leaves the active server "not configured"
    // and GEMDRIVE still boots normally, falling back to its existing
    // no-network behavior (see sidetnfs_mark_network_skipped()).
    // (Briefly rolled back and re-confirmed: a stale/incorrect IP in
    // the saved flash config, not this wiring, caused the earlier
    // NO_NETW.TXT symptom -- fixed by re-saving the config.)
    sidetnfs_probe_load_active_server();

    init_gemdrvemul(safe_config_reboot);

    /* NOT REACHED */
    for (;;)
    {
        tight_loop_contents();
    }

    // Fase 10 (Morse-LED en oude productstates verwijderen): the four
    // legacy mode-selection blocks that used to follow here are gone --
    // ROM_EMULATOR, FLOPPY_EMULATOR, RTC_EMULATOR and the final
    // "fall through to the old configurator" default, each dispatched on
    // a PARAM_BOOT_FEATURE strcmp(). Fase 3 already made GEMDRIVE the
    // sole reachable production path (init_gemdrvemul() above never
    // returns), so all of it was provably dead boot dispatch, together
    // with the blink_morse() product-mode status codes it carried
    // ('D', 'E', 'T') and the init_firmware() configurator entry.
    //
    // Deliberately NOT removed: rtcemul.c, which still provides the NTP /
    // Pico-RTC / Atari-time helpers this flow calls every boot. The old
    // configurator, ROM loader, floppy emulator and standalone RTC emulator
    // are gone from the repository entirely.
    return 0;
}
